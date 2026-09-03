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
    use_l1id_    = config_.get<bool>("use_l1id", false);
    enable_diagnostics_ = config_.get<bool>("enable_diagnostics", true);
    if(use_l1id_) {
        LOG(STATUS) << "Event grouping: using l1id (12-bit, wraps at 4096) instead of l1idC.";
    }

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
                   << "_" << i << ".root";
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

    // Coincidence requirement (see run()): only non-DUT, non-auxiliary, non-passive planes must
    // agree on an l1id for it to count as a real event -- mirrors Module::get_regular_detectors(false).
    coincidence_required_.assign(n_planes, false);
    for (size_t i = 0; i < n_planes; ++i) {
        auto detector = get_detector(det_names_[i]);
        coincidence_required_[i] = !detector->hasRole(DetectorRole::DUT) &&
                                   !detector->hasRole(DetectorRole::AUXILIARY) &&
                                   !detector->hasRole(DetectorRole::PASSIVE);
        if (coincidence_required_[i]) n_coincidence_planes_++;
    }
    if (n_coincidence_planes_ == 0) {
        LOG(WARNING) << "No non-DUT reference planes found among detector_names -- coincidence "
                     << "requirement disabled, every l1id will be accepted as an event (old behaviour).";
    } else {
        LOG(STATUS) << "Coincidence requirement: " << n_coincidence_planes_ << " reference plane(s) "
                    << "must agree on l1idC for an event to be accepted; DUT planes are exempt.";
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

    hL1idAcceptanceRateTrend_ = new TProfile("hL1idAcceptanceRateTrend",
                                             "l1id Acceptance Rate Trend (100/candidates scanned per accepted event);"
                                             "Event Number;Acceptance Rate [%]",
                                             kTrendNBins, x_start, x_start + kTrendNBins);
    hL1idAcceptanceRateTrend_->SetCanExtend(TH1::kAllAxes);

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

        name  = "hHitsPerL1DC_beforeROI_" + det_names_[i];
        title = "Hit pixels per L1DC before ROI cut [" + det_names_[i] + "]"
                ";Hit pixels per L1DC;Entries";
        hHitsPerL1DC_beforeROI_.push_back(new TH1F(name.c_str(), title.c_str(), 200, -0.5, 199.5));

        name  = "hHitsPerL1DC_afterROI_" + det_names_[i];
        title = "Hit pixels per L1DC after ROI cut [" + det_names_[i] + "]"
                ";Hit pixels per L1DC;Entries";
        hHitsPerL1DC_afterROI_.push_back(new TH1F(name.c_str(), title.c_str(), 200, -0.5, 199.5));

        // TEMPORARY investigation-only: address-decode diagnostic histograms (work order v2). Only fields that
        // cannot be reconstructed downstream from (column, row) are captured here -- the rest of the diagnosis
        // lives in the AnalysisMALTADecoding module. Booked only when enable_diagnostics_ is set (default true).
        if(enable_diagnostics_) {
            const std::string& det = det_names_[i];

            hDuplicateFlag_.push_back(new TH1F(
                ("hDuplicateFlag_" + det).c_str(), ("isDuplicate [" + det + "];isDuplicate;Words").c_str(), 2, -0.5, 1.5));
            hPixPopcount_.push_back(new TH1F(("hPixPopcount_" + det).c_str(),
                                             ("Bits set in pixel word [" + det + "];Bits set in pixel word;Words").c_str(),
                                             17,
                                             -0.5,
                                             16.5));
            hBcidRaw_.push_back(new TH1F(
                ("hBcidRaw_" + det).c_str(), ("bcid [" + det + "];bcid;Words").c_str(), 64, -0.5, 63.5));
            hWinidRaw_.push_back(new TH1F(
                ("hWinidRaw_" + det).c_str(), ("winid [" + det + "];winid;Words").c_str(), 256, -0.5, 255.5));
            hPhaseRaw_.push_back(new TH1F(
                ("hPhaseRaw_" + det).c_str(), ("phase [" + det + "];phase;Words").c_str(), 16, -0.5, 15.5));

            hGroupRaw_.push_back(new TH1F(
                ("hGroupRaw_" + det).c_str(), ("group [" + det + "];group;Hits").c_str(), 32, -0.5, 31.5));
            hHitmapRaw_.push_back(new TH2F(("hHitmapRaw_" + det).c_str(),
                                           ("Raw hitmap (no cuts) [" + det + "];column;row").c_str(),
                                           512,
                                           -0.5,
                                           511.5,
                                           800,
                                           -300.5,
                                           499.5));
        }
    }
}

void EventLoaderMALTA::decodeAndStore(const HitData& data, PixelVector& hits, const std::string& det_name, size_t plane_idx) {

    // TEMPORARY investigation-only: word-level diagnostics (see work order). Observation only.
    if (enable_diagnostics_) {
        hDuplicateFlag_[plane_idx]->Fill(static_cast<double>(data.isDuplicate));
    }

    if (data.isDuplicate == 1) return;

    if (enable_diagnostics_) {
        const int npix = __builtin_popcount(static_cast<unsigned int>(data.pixel) & 0xFFFFu);
        hPixPopcount_[plane_idx]->Fill(npix);
        hBcidRaw_[plane_idx]->Fill(static_cast<double>(data.bcid));
        hWinidRaw_[plane_idx]->Fill(static_cast<double>(data.winid));
        hPhaseRaw_[plane_idx]->Fill(static_cast<double>(data.phase));
    }

    // get detector information
    auto detector = get_detector(det_name);

    for (int pix = 0; pix < 16; pix++) {
        if (((data.pixel >> pix) & 1) == 0) continue;

        // TEMPORARY investigation-only: hit-level diagnostics (work order v2). Computed independently from the
        // real xpos/ypos below (identical formula) so they can be filled before column_filter_/masked(),
        // without touching the existing decode logic or its ordering at all.
        if (enable_diagnostics_) {
            int dxpos = static_cast<int>(data.dcolumn) * 2 + (pix > 7 ? 1 : 0);
            int dypos = static_cast<int>(data.group) * 16 + (data.parity == 1 ? 8 : 0) + (pix % 8);
            if (real_data_) dypos -= 288;

            hGroupRaw_[plane_idx]->Fill(static_cast<double>(data.group));
            hHitmapRaw_[plane_idx]->Fill(dxpos, dypos);
        }

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

        // MALTA hits carry no charge-equivalent (ToT/ADC) value, so the Pixel's raw() field, which would otherwise
        // hold that, is repurposed here to carry the original bcid instead (needed for debugging, e.g. ClusterDump).
        // winid/phase are not stored separately but can be recovered from (timestamp - bcid * 25ns), since they were
        // folded into time_ns using the same formula above.
        hits.push_back(std::make_shared<Pixel>(det_name, xpos, ypos, static_cast<int>(data.bcid), 1, time_ns));
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

    // Select which branch to use as the event-grouping trigger ID.
    // For real hardware, l1idC is the 32-bit non-wrapping counter (avoids 12-bit rollover).
    // For simulation data where l1idC is not written (always 0), set use_l1id=true to
    // use l1id instead.
    auto triggerID = [&](size_t i) -> uint32_t {
        return use_l1id_ ? data_buffer_[i].l1id : data_buffer_[i].l1idC;
    };

    uint32_t target_l1id = 0xFFFFFFFF;
    double wall_clock_time = 0.0;
    bool has_wall_clock_time = false;
    std::vector<PixelVector> all_hits_vec(n_planes);
    long long candidates_this_call = 0;

    // Scan forward through l1ids until a real coincident event is found (every
    // coincidence_required_ plane -- i.e. every non-DUT reference plane -- has a hit at that
    // l1id, and no hit falls outside ROI), or data runs out. Single-plane noise/dark-count hits
    // (an l1id only one plane happens to have) and ROI-violated l1ids are discarded here and
    // never become a corryvreckan Event: this is what makes ev_count -- and the framework's own
    // number_of_events -- mean "real coincident events" rather than "l1ids attempted", and keeps
    // per-plane cluster-count trend plots (e.g. OnlineMonitor's Timeline) from being dominated by
    // degenerate zero-hit points. DUT planes are always exempt from the coincidence requirement:
    // requiring the DUT to match too would systematically discard genuine DUT-inefficiency
    // events, destroying the ability to measure DUT efficiency downstream. A single run() call
    // can therefore silently consume many discarded l1ids before returning.
    while (true) {
        target_l1id = 0xFFFFFFFF;
        uint32_t max_l1idC_scan = 0;
        bool any_data_left = false;

        for (size_t i = 0; i < n_planes; ++i) {
            if (current_indices_[i] < num_entries_[i]) {
                any_data_left = true;
                chains_[i]->GetEntry(current_indices_[i]);
                uint32_t current_l1id = triggerID(i);

                if (current_l1id < target_l1id) target_l1id = current_l1id;
                if (current_l1id > max_l1idC_scan) max_l1idC_scan = current_l1id;
            }
        }

        has_wall_clock_time = false;
        if (any_data_left) {
            double ev_abs = static_cast<double>(ev_count + skip_entries_);
            if (hL1idCVsEvent_)    hL1idCVsEvent_->Fill(ev_abs, static_cast<double>(target_l1id));
            if (hL1idCPlaneDelta_) hL1idCPlaneDelta_->Fill(static_cast<double>(max_l1idC_scan - target_l1id));

            // timer: wall-clock seconds since run start (from MaltaDAQ).
            // Use the first plane that has an entry matching target_l1id.
            for (size_t i = 0; i < n_planes; ++i) {
                if (current_indices_[i] < num_entries_[i] &&
                    triggerID(i) == target_l1id) {
                    wall_clock_time = static_cast<double>(data_buffer_[i].timer);
                    has_wall_clock_time = true;
                    if (hTimerVsEvent_) hTimerVsEvent_->Fill(ev_abs, wall_clock_time);
                    break;
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
                        uint32_t current_l1id = triggerID(i);
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

        // A genuine candidate l1id (some plane really had an entry for it) -- count it towards the
        // acceptance-rate diagnostic regardless of whether it survives the ROI/coincidence checks
        // below.
        candidates_this_call++;
        total_l1id_scanned_++;

        // Collect hits from all planes that match the target L1ID
        for (auto& hits : all_hits_vec) hits.clear();
        for (size_t i = 0; i < n_planes; ++i) {
            if (current_indices_[i] >= num_entries_[i]) continue;

            // Compare the current entry's L1ID with the target L1ID
            if (triggerID(i) == target_l1id) {
                // get all hits for this L1ID in the current plane
                while (current_indices_[i] < num_entries_[i]) {
                    chains_[i]->GetEntry(current_indices_[i]);
                    if (triggerID(i) == target_l1id) {
                        decodeAndStore(data_buffer_[i], all_hits_vec[i], det_names_[i], i);
                        current_indices_[i]++;
                    } else {
                        break;
                    }
                }
            }
        }

        // Fill before-ROI histograms
        for (size_t i = 0; i < n_planes; ++i) {
            hHitsPerL1DC_beforeROI_[i]->Fill(static_cast<double>(all_hits_vec[i].size()));
        }

        // ROI filter: any hit outside ROI in any plane -> discard this l1id entirely (not just
        // clear its hits -- see run()'s doc comment above)
        bool roi_violation = false;
        for (size_t i = 0; i < n_planes && !roi_violation; ++i) {
            auto det = get_detector(det_names_[i]);
            for (const auto& pixel : all_hits_vec[i]) {
                if (!det->isWithinROI(pixel->column(), pixel->row())) {
                    roi_violation = true;
                    break;
                }
            }
        }
        if (roi_violation) {
            LOG(TRACE) << "L1ID " << target_l1id << ": hit outside ROI, discarding (not a real event)";
            continue;
        }

        // Coincidence requirement: every non-DUT reference plane must have a hit at target_l1id.
        bool coincident = true;
        if (n_coincidence_planes_ > 0) {
            for (size_t i = 0; i < n_planes; ++i) {
                if (coincidence_required_[i] && all_hits_vec[i].empty()) {
                    coincident = false;
                    break;
                }
            }
        }
        if (!coincident) {
            LOG(TRACE) << "L1ID " << target_l1id << ": not all reference planes coincide, discarding (noise)";
            continue;
        }

        // Fill after-ROI histograms (only for l1ids that passed both the ROI cut and the
        // coincidence requirement, i.e. that are about to become a real event)
        for (size_t i = 0; i < n_planes; ++i) {
            hHitsPerL1DC_afterROI_[i]->Fill(static_cast<double>(all_hits_vec[i].size()));
        }

        break; // found a real event
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
    if (has_wall_clock_time) {
        // Wall-clock seconds since run start, for run-long trend plots (e.g. TopupSearch).
        // Distinct from the event's own time window above, which is local to this L1ID.
        event->addTag("wall_clock_time", std::to_string(wall_clock_time));
    }
    clipboard->putEvent(event);

    // Update the clipboard with the collected hits for this event. Non-DUT planes are now
    // guaranteed non-empty by the coincidence requirement in the scan loop above, so
    // all_plane_have_hits effectively tracks DUT participation (with 0 configured DUTs, or a DUT
    // that happened to also register a hit, this is always true) -- i.e. hCoincidenceRateTrend
    // below is now closer to a live DUT-efficiency proxy than a reference-plane sync-health check.
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
    total_l1id_accepted_++;

    // Output the coincidence rate trend for monitoring synchronization health. The rate is calculated as the percentage of events where all planes have hits (coincidence) out of the total events processed.
    hCoincidenceRateTrend->Fill(static_cast<double>(ev_count + skip_entries_), (all_plane_have_hits ? 100.0 : 0.0));

    // l1id acceptance rate: how many candidates (real coincidences + discarded noise/ROI
    // violations) it took to find this one accepted event. 100% = every candidate scanned this
    // round was accepted immediately (no noise); a low value means many candidates were discarded
    // first -- see the member declaration in EventLoaderMALTA.h.
    hL1idAcceptanceRateTrend_->Fill(static_cast<double>(ev_count + skip_entries_),
                                    100.0 / static_cast<double>(candidates_this_call));

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
    rebinTrend(hL1idAcceptanceRateTrend_);
    rebinTrend(hL1idCVsEvent_);
    rebinTrend(hTimerVsEvent_);
    for (auto* p : hHitsPerEventTrend_) rebinTrend(p);

    if (ev_count % 1000 == 0 && ev_count > 0) {
        double rate = static_cast<double>(coincidence_ev_count) / total_ev_count * 100.0;
        double acceptance_rate = (total_l1id_scanned_ > 0)
                                      ? static_cast<double>(total_l1id_accepted_) / static_cast<double>(total_l1id_scanned_) * 100.0
                                      : 100.0;
        // Reference-plane sync is guaranteed by the coincidence requirement above (every accepted
        // event already has hits on every non-DUT plane), so this rate now reflects DUT
        // participation only: with n_coincidence_planes_ == n_planes (no DUT configured) it is
        // always 100%.
        LOG(INFO) << "Health Check: Run progress = " << ev_count << " events, DUT participation rate = "
                  << std::fixed << std::setprecision(2) << rate
                  << "%, l1id acceptance rate = " << acceptance_rate << "% ("
                  << total_l1id_accepted_ << "/" << total_l1id_scanned_ << " candidates)";

        if (n_coincidence_planes_ < n_planes && rate < 10.0) {
            LOG(WARNING) << "!!! WARNING: DUT participation rate is very low (" << rate
                         << "%). Check DUT connection, threshold, or alignment!";
        }
        if (n_coincidence_planes_ > 0 && acceptance_rate < 10.0) {
            LOG(WARNING) << "!!! WARNING: l1id acceptance rate is very low (" << acceptance_rate
                         << "%, " << total_l1id_scanned_ - total_l1id_accepted_
                         << " candidates discarded so far). Check per-plane noise rate or thresholds!";
        }
    }

    return StatusCode::Success;
}