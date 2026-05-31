#include "EventLoaderMALTA.h"
#include "core/utils/log.h"
#include <limits>
#include <algorithm>
#include <iomanip>
#include <thread>
#include "TChainElement.h"

using namespace corryvreckan;

EventLoaderMALTA::EventLoaderMALTA(Configuration& config, std::vector<std::shared_ptr<Detector>> detectors)
    : Module(config, std::move(detectors)) {
}

void EventLoaderMALTA::initialize() {
    
    det_names_ = config_.getArray<std::string>("detector_names");
    size_t n_planes = det_names_.size();

    size_t skip_entries = config_.get<size_t>("skip_entries", 0);
    max_events_ = config_.get<size_t>("number_of_events", 0);
    online_mode_ = config_.get<bool>("online_mode", false);
    real_data_   = config_.get<bool>("real_data", false);

    std::string filter_type = config_.get<std::string>("column_filter", "none");
    std::transform(filter_type.begin(), filter_type.end(), filter_type.begin(), ::tolower);

    if (filter_type == "even") {
        column_filter_ = ColumnFilter::EVEN;
        LOG(STATUS) << "Column filter enabled: ONLY EVEN columns will be processed.";
    } else if (filter_type == "odd") {
        column_filter_ = ColumnFilter::ODD;
        LOG(STATUS) << "Column filter enabled: ONLY ODD columns will be processed.";
    } else {
        column_filter_ = ColumnFilter::NONE;
        LOG(STATUS) << "Column filter disabled: All columns will be processed.";
    }

    LOG(STATUS) << "Near-Online Setup: Skip=" << skip_entries 
                << ", MaxEvents=" << (max_events_ > 0 ? std::to_string(max_events_) : "Unlimited");

    // Check for custom explicit file list
    std::vector<std::string> file_paths;
    if(config_.has("file_names")) {
        file_paths = config_.getArray<std::string>("file_names");
        LOG(STATUS) << "Custom file_names provided. Ignoring base_path and run_number multi-merge configuration.";
    }

    data_buffer_.resize(n_planes);
    num_entries_.resize(n_planes);
    current_indices_.assign(n_planes, 0);
    ev_count = 0;

    // Get list of runs to merge (supports single int or array of ints)
    std::vector<int> run_numbers;
    if (file_paths.empty()) {
        run_numbers = config_.getArray<int>("run_number");
        LOG(STATUS) << "Multi-Run Merger active. Merging " << run_numbers.size() << " run dataset(s).";
    }

    // Outer Loop: Process each tracking detector plane sequentially
    for (size_t i = 0; i < n_planes; ++i) {
        auto chain = std::make_unique<TChain>("MALTA");
        chain->ResetBit(TObject::kCanDelete);
        chain->SetDirectory(nullptr);

        if (!file_paths.empty()) {
            // Case A: User provided explicit file list via config
            if (i < file_paths.size()) {
                chain->Add(file_paths[i].c_str());
                LOG(STATUS) << "Plane " << i << " [" << det_names_[i] << "] Loaded explicit path: " << file_paths[i];
            } else {
                throw InvalidValueError(config_, "file_names", "Not enough file paths provided for all detectors!");
            }
        } else {
            // Case B: Automated Multi-Run Stream Stitching based on run_number array
            std::string base_path = config_.get<std::string>("base_path");
            
            for (size_t r = 0; r < run_numbers.size(); ++r) {
                std::ostringstream ss;
                ss << base_path << "/run_" 
                   << std::setw(6) << std::setfill('0') << run_numbers[r] 
                   << "_" << i << ".root.root";
                std::string file_path = ss.str();
                
                chain->Add(file_path.c_str());
                LOG(STATUS) << "Plane " << i << " [" << det_names_[i] << "] Appended Stream: " << file_path;
            }
        }

        // Store file paths for chain recreation in online mode
        std::vector<std::string> paths;
        TObjArray* file_list = chain->GetListOfFiles();
        for(int j = 0; j < file_list->GetEntries(); j++) {
            paths.push_back(static_cast<TChainElement*>(file_list->At(j))->GetTitle());
        }
        chain_file_paths_.push_back(paths);

        // Validate complete chained dataset allocation.
        // Zero entries is allowed: MaltaDAQ may not have called AutoSave yet.
        // run() will refresh the chain and pick up entries as they arrive.
        num_entries_[i] = chain->GetEntries();
        if (num_entries_[i] == 0) {
            LOG(WARNING) << "Plane " << i << " [" << det_names_[i] << "] has 0 entries at startup."
                         << " Waiting for MaltaDAQ to write data...";
        } else {
            LOG(INFO) << "Plane " << i << " [" << det_names_[i] << "] Total Chained Entries = " << num_entries_[i];
        }

        current_indices_[i] = static_cast<long long int>(std::min(skip_entries, static_cast<size_t>(num_entries_[i])));

        data_buffer_[i].setBranchAddresses(chain.get());
        chains_.push_back(std::move(chain));
    }

    last_refresh_time_ = std::chrono::steady_clock::now();

    // Trend profiles start with a small initial range and grow dynamically.
    // SetCanExtend lets ROOT double the range when a fill falls outside;
    // run() then rebins back to kTrendNBins so the bin count stays constant.
    static constexpr int kTrendNBins = 200;
    double x_start = static_cast<double>(skip_entries);

    hCoincidenceRateTrend = new TProfile("hCoincidenceRateTrend",
                                         "Coincidence Rate Trend;Event Number;Coincidence Rate [%]",
                                         kTrendNBins, x_start, x_start + kTrendNBins);
    hCoincidenceRateTrend->SetCanExtend(TH1::kAllAxes);

    hL1idCVsEvent_ = new TProfile("hL1idCVsEvent",
                                   "Hardware Trigger Count vs Corryvreckan Event;"
                                   "Corryvreckan Event;l1idC (hardware trigger)",
                                   kTrendNBins, x_start, x_start + kTrendNBins);
    hL1idCVsEvent_->SetCanExtend(TH1::kAllAxes);

    hL1idCPlaneDelta_ = new TH1F("hL1idCPlaneDelta",
                                  "l1idC Sync across planes;"
                                  "max(l1idC) - min(l1idC) [triggers];Events",
                                  20, -0.5, 19.5);

    hTimerVsEvent_ = new TProfile("hTimerVsEvent",
                                   "Wall-clock Time vs Corryvreckan Event;"
                                   "Corryvreckan Event;Elapsed time [s]",
                                   kTrendNBins, x_start, x_start + kTrendNBins);
    hTimerVsEvent_->SetCanExtend(TH1::kAllAxes);

    for (size_t i = 0; i < n_planes; ++i) {
        std::string name  = "hHitsPerEventTrend_" + det_names_[i];
        std::string title = "Hits per event trend [" + det_names_[i] + "]"
                            ";Event Number;Mean hits per event";
        auto* p = new TProfile(name.c_str(), title.c_str(),
                               kTrendNBins, x_start, x_start + kTrendNBins);
        p->SetCanExtend(TH1::kAllAxes);
        hHitsPerEventTrend_.push_back(p);

        name  = "hHitsPerEventDist_" + det_names_[i];
        title = "Hits per event distribution [" + det_names_[i] + "]"
                ";Hits per event;Events";
        hHitsPerEventDist_.push_back(new TH1F(name.c_str(), title.c_str(), 100, -0.5, 99.5));
    }
}

void EventLoaderMALTA::decodeAndStore(const HitData& data, PixelVector& hits, const std::string& det_name) {
    
    if (data.isDuplicate == 1) return;

    // get detector information
    auto detector = get_detector(det_name);

    for (int pix = 0; pix < 16; pix++) {
        if (((data.pixel >> pix) & 1) == 0) continue;
        int xpos = static_cast<int>(data.dcolumn) * 2;
        if (pix > 7) xpos += 1;

        if (column_filter_ == ColumnFilter::EVEN && xpos % 2 != 0) continue;
        if (column_filter_ == ColumnFilter::ODD && xpos % 2 == 0) continue;
        
        int ypos = static_cast<int>(data.group) * 16;
        if (data.parity == 1) ypos += 8;
        switch(pix) {
            case 0: case 8:  ypos += 0; break; case 1: case 9:  ypos += 1; break;
            case 2: case 10: ypos += 2; break; case 3: case 11: ypos += 3; break;
            case 4: case 12: ypos += 4; break; case 5: case 13: ypos += 5; break;
            case 6: case 14: ypos += 6; break; case 7: case 15: ypos += 7; break;
            default: break;
        }
        if (real_data_) ypos -= 288;

        // Masking step
        if (detector->masked(xpos, ypos)) {
            LOG(TRACE) << "Masking pixel (col, row) = (" << xpos << ", " << ypos << ")";
            continue;
        }

        double time_ns = static_cast<double>(data.bcid) * 25.0 + 
                         static_cast<double>(data.winid) * 3.125 + 
                         static_cast<double>(data.phase) * 0.39;

        hits.push_back(std::make_shared<Pixel>(det_name, xpos, ypos, 1, 1, time_ns));
    }
}

void EventLoaderMALTA::finalize(const std::shared_ptr<ReadonlyClipboard>&) {
    for(auto& chain : chains_) {
        if(chain) {
            chain->ResetBranchAddresses();
            // TChain::LoadTree() calls SetDirectory(fFile) internally, registering
            // the chain in the input file's directory. Without this call, when
            // TChain::~TChain() closes the file, TFile::Close() would find the
            // chain in the directory and try to delete it — causing a double-free.
            chain->SetDirectory(nullptr);
            if(TTree* tree = chain->GetTree()) {
                tree->SetDirectory(nullptr);
            }
            chain.reset();
        }
    }
    chains_.clear();
}

StatusCode EventLoaderMALTA::run(const std::shared_ptr<Clipboard>& clipboard) {

    if (max_events_ > 0 && ev_count >= max_events_) {
        LOG(STATUS) << "Reached max_events (" << max_events_ << "). Ending run.";

        auto dummy_event = std::make_shared<Event>(0, 0);
        clipboard->putEvent(dummy_event);

        return StatusCode::EndRun;
    }

    size_t n_planes = chains_.size();
    uint32_t target_l1id = 0xFFFFFFFF;
    bool any_data_left = false;

    // Scan all planes to find the minimum L1ID (target_l1id) among the current entries.
    // l1idC is the 32-bit non-wrapping trigger counter written by both MaltaDAQ and
    // Malta2TreeWriter.  Using l1idC instead of l1id avoids the 12-bit (0-4095) rollover
    // that occurs with real hardware after every 4096 triggers.
    uint32_t max_l1idC_scan = 0;
    for (size_t i = 0; i < n_planes; ++i) {
        if (current_indices_[i] < num_entries_[i]) {
            any_data_left = true;
            chains_[i]->GetEntry(current_indices_[i]);
            uint32_t current_l1id = data_buffer_[i].l1idC;

            if (current_l1id < target_l1id) target_l1id = current_l1id;
            if (current_l1id > max_l1idC_scan) max_l1idC_scan = current_l1id;
        }
    }

    if (any_data_left) {
        double ev_abs = static_cast<double>(ev_count + skip_entries_);
        if (hL1idCVsEvent_)    hL1idCVsEvent_->Fill(ev_abs, static_cast<double>(target_l1id));
        if (hL1idCPlaneDelta_) hL1idCPlaneDelta_->Fill(static_cast<double>(max_l1idC_scan - target_l1id));

        // timer: wall-clock seconds since run start (from MaltaDAQ).
        // Use the first plane that has an entry matching target_l1id.
        if (hTimerVsEvent_) {
            for (size_t i = 0; i < n_planes; ++i) {
                if (current_indices_[i] < num_entries_[i] &&
                    data_buffer_[i].l1idC == target_l1id) {
                    hTimerVsEvent_->Fill(ev_abs, static_cast<double>(data_buffer_[i].timer));
                    break;
                }
            }
        }
    }

    // If no data is currently available, try to refresh the chains to pick up
    // new entries written by MaltaDAQ (AutoSave flushes the TTree key ~1 s).
    // Refresh is throttled to at most once every 200 ms to avoid excessive I/O.
    if (!any_data_left) {
        auto now = std::chrono::steady_clock::now();
        double ms_elapsed = std::chrono::duration<double, std::milli>(now - last_refresh_time_).count();
        if (ms_elapsed >= 200.0) {
            bool any_new_entries = false;
            for (size_t i = 0; i < n_planes; ++i) {
                Long64_t before = num_entries_[i];

                // Recreate the chain from scratch so ROOT reads a fresh file header.
                // TChain::Refresh() updates the entry count but leaves the internal
                // TTree's fEntries stale, causing GetEntry() to silently skip new rows.
                auto fresh = std::make_unique<TChain>("MALTA");
                fresh->ResetBit(TObject::kCanDelete);
                fresh->SetDirectory(nullptr);
                for (const auto& path : chain_file_paths_[i]) {
                    fresh->Add(path.c_str());
                }
                Long64_t new_entries = fresh->GetEntries();

                if (new_entries > before) {
                    chains_[i]->ResetBranchAddresses();
                    chains_[i] = std::move(fresh);
                    data_buffer_[i].setBranchAddresses(chains_[i].get());
                    num_entries_[i] = new_entries;
                    any_new_entries = true;
                    LOG(DEBUG) << "Plane " << i << " [" << det_names_[i] << "] chain refreshed: "
                               << before << " -> " << new_entries << " entries";
                }
            }
            last_refresh_time_ = now;
            // Re-scan for the minimum L1ID after refresh
            for (size_t i = 0; i < n_planes; ++i) {
                if (current_indices_[i] < num_entries_[i]) {
                    any_data_left = true;
                    chains_[i]->GetEntry(current_indices_[i]);
                    uint32_t current_l1id = data_buffer_[i].l1idC;
                    if (current_l1id < target_l1id) {
                        target_l1id = current_l1id;
                    }
                }
            }
            if (!any_data_left && !any_new_entries && !online_mode_) {
                LOG(STATUS) << "All data consumed. Ending run.";
                clipboard->putEvent(std::make_shared<Event>(0, 0));
                return StatusCode::EndRun;
            }
        }
        if (!any_data_left) {
            if(!online_mode_) {
                clipboard->putEvent(std::make_shared<Event>(0, 0));
                return StatusCode::EndRun;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return StatusCode::DeadTime;
        }
    }

    // Collect hits from all planes that match the target L1ID
    std::vector<PixelVector> all_hits_vec(n_planes);
    for (size_t i = 0; i < n_planes; ++i) {
        if (current_indices_[i] >= num_entries_[i]) continue;

        // Compare the current entry's L1ID with the target L1ID
        if (data_buffer_[i].l1idC == target_l1id) {
            // get all hits for this L1ID in the current plane
            while (current_indices_[i] < num_entries_[i]) {
                chains_[i]->GetEntry(current_indices_[i]);
                if (data_buffer_[i].l1idC == target_l1id) {
                    decodeAndStore(data_buffer_[i], all_hits_vec[i], det_names_[i]);
                    current_indices_[i]++;
                } else {
                    break;
                }
            }
        }
    }

   // Set the event time window based on the earliest and latest timestamps among the collected hits
    double actual_min = std::numeric_limits<double>::max();
    double actual_max = std::numeric_limits<double>::lowest();
    bool has_any_hits = false;

    for (const auto& hits : all_hits_vec) {
        for (const auto& p : hits) {
            actual_min = std::min(actual_min, p->timestamp());
            actual_max = std::max(actual_max, p->timestamp());
            has_any_hits = true;
        }
    }

    // time window is set to [min-10ns, max+10ns] to allow for some timing uncertainty and ensure all hits are included in the event
    double min_t = has_any_hits ? actual_min : 0;
    double max_t = has_any_hits ? actual_max : 100;

    // Clipboard requires an event object, we create one with the time window covering all hits and add the target L1ID as a trigger
    auto event = std::make_shared<Event>(min_t - 10.0, max_t + 10.0);
    event->addTrigger(target_l1id, min_t);
    clipboard->putEvent(event);

    // Update the clipboard with the collected hits for this event, only if all planes have at least one hit to ensure we are working with coincident events
    bool all_plane_have_hits = true;
    for (size_t i = 0; i < n_planes; ++i) {
        if (!all_hits_vec[i].empty()) {
            clipboard->putData(all_hits_vec[i], det_names_[i]);
        } else {
            all_plane_have_hits = false;
        }
    }

    total_ev_count++;
    if (all_plane_have_hits) coincidence_ev_count++;
    ev_count++;

    // Output the coincidence rate trend for monitoring synchronization health. The rate is calculated as the percentage of events where all planes have hits (coincidence) out of the total events processed.
    hCoincidenceRateTrend->Fill(static_cast<double>(ev_count + skip_entries_), (all_plane_have_hits ? 100.0 : 0.0));

    for (size_t i = 0; i < n_planes; ++i) {
        double nhits = static_cast<double>(all_hits_vec[i].size());
        hHitsPerEventTrend_[i]->Fill(static_cast<double>(ev_count + skip_entries_), nhits);
        hHitsPerEventDist_[i]->Fill(nhits);
    }

    // Rebin trend profiles to maintain ~200 bins regardless of total event count.
    // SetCanExtend doubles the bin count when a fill falls outside the range;
    // this lambda merges them back down so the display stays readable.
    auto rebinTrend = [](TProfile* p) {
        if (!p) return;
        int n = p->GetNbinsX();
        if (n > 400) p->RebinX(n / 200);
    };
    rebinTrend(hCoincidenceRateTrend);
    rebinTrend(hL1idCVsEvent_);
    rebinTrend(hTimerVsEvent_);
    for (auto* p : hHitsPerEventTrend_) rebinTrend(p);

    if (ev_count % 1000 == 0 && ev_count > 0) {
        double rate = static_cast<double>(coincidence_ev_count) / total_ev_count * 100.0;
        LOG(INFO) << "Sync Health Check: Run progress = " << ev_count << " events, Coincidence Rate = "
                  << std::fixed << std::setprecision(2) << rate << "%";

        if (rate < 10.0) {
            LOG(WARNING) << "!!! WARNING: Sync rate is very low (" << rate << "%). Check Plane connections or L1ID consistency!";
        }
    }

    return StatusCode::Success;
}