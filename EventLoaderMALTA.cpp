#include "EventLoaderMALTA.h"
#include "core/utils/log.h"
#include <limits>
#include <algorithm>
#include <iomanip>

using namespace corryvreckan;

EventLoaderMALTA::EventLoaderMALTA(Configuration& config, std::vector<std::shared_ptr<Detector>> detectors)
    : Module(config, std::move(detectors)) {
}

void EventLoaderMALTA::initialize() {
    
    // Get configuration parameters
    std::string base_path = config_.get<std::string>("base_path");
    int run_number = config_.get<int>("run_number");
    det_names_ = config_.getArray<std::string>("detector_names"); // ["Plane0", "Plane1", "Plane2", ...]

    size_t n_planes = det_names_.size();
    
    // ready the data files and set up TChains
    data_buffer_.resize(n_planes);
    num_entries_.resize(n_planes);
    current_indices_.assign(n_planes, 0);
    ev_count = 0;

    // Automatically construct file paths and set up TChains for each plane
    for (size_t i = 0; i < n_planes; ++i) {

        std::ostringstream ss;
        // path example: /data/run_000123_0.root.root, /data/run_000123_1.root.root, ...
        ss << base_path << "/run_" 
           << std::setw(6) << std::setfill('0') << run_number 
           << "_" << i << ".root.root";
        
        std::string file_path = ss.str();
        
        auto chain = std::make_unique<TChain>("MALTA");
        chain->Add(file_path.c_str());
        
        num_entries_[i] = chain->GetEntries();
        if (num_entries_[i] == 0) {
            throw InvalidValueError(config_, "run_number", "No entries found in file: " + file_path);
        }

        data_buffer_[i].setBranchAddresses(chain.get());
        chains_.push_back(std::move(chain));

        LOG(STATUS) << "Plane " << i << " [" << det_names_[i] << "] Completed: " << file_path;
    }

    hCoincidenceRateTrend = new TProfile("hCoincidenceRateTrend", 
                                         "Coincidence Rate Trend;Event Number;Coincidence Rate [%]", 
                                         1000, 0, 10000000);
}

void EventLoaderMALTA::decodeAndStore(const HitData& data, PixelVector& hits, const std::string& det_name) {
    
    if (data.isDuplicate == 1) return;

    // get detector information
    auto detector = get_detector(det_name);

    for (int pix = 0; pix < 16; pix++) {
        if (((data.pixel >> pix) & 1) == 0) continue;
        int xpos = static_cast<int>(data.dcolumn) * 2;
        if (pix > 7) xpos += 1;
        int ypos = static_cast<int>(data.group) * 16;
        if (data.parity == 1) ypos += 8;
        switch(pix) {
            case 0: case 8:  ypos += 0; break; case 1: case 9:  ypos += 1; break;
            case 2: case 10: ypos += 2; break; case 3: case 11: ypos += 3; break;
            case 4: case 12: ypos += 4; break; case 5: case 13: ypos += 5; break;
            case 6: case 14: ypos += 6; break; case 7: case 15: ypos += 7; break;
            default: break;
        }
        ypos = ypos - 288;

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

StatusCode EventLoaderMALTA::run(const std::shared_ptr<Clipboard>& clipboard) {

    size_t n_planes = chains_.size();
    uint32_t target_l1id = 0xFFFFFFFF;
    bool any_data_left = false;

    // Scan all planes to find the minimum L1ID (target_l1id) among the current entries
    for (size_t i = 0; i < n_planes; ++i) {
        if (current_indices_[i] < num_entries_[i]) {
            any_data_left = true;
            chains_[i]->GetEntry(current_indices_[i]);
            uint32_t current_l1id = static_cast<uint32_t>(data_buffer_[i].l1id);
        
            if (current_l1id < target_l1id) {
                target_l1id = current_l1id;
            }
        }
    }

    // if no data is left in any plane, we are done with the run
    if (!any_data_left) {
        auto dummy_event = std::make_shared<Event>(0, 1);
        clipboard->putEvent(dummy_event);
        return StatusCode::EndRun;
    }

    // Collect hits from all planes that match the target L1ID
    std::vector<PixelVector> all_hits_vec(n_planes);
    for (size_t i = 0; i < n_planes; ++i) {
        if (current_indices_[i] >= num_entries_[i]) continue;

        // Compare the current entry's L1ID with the target L1ID
        if (static_cast<uint32_t>(data_buffer_[i].l1id) == target_l1id) {
            // get all hits for this L1ID in the current plane
            while (current_indices_[i] < num_entries_[i]) {
                chains_[i]->GetEntry(current_indices_[i]);
                if (static_cast<uint32_t>(data_buffer_[i].l1id) == target_l1id) {
                    decodeAndStore(data_buffer_[i], all_hits_vec[i], det_names_[i]);
                    current_indices_[i]++;
                } else {
                    break; // Move to the next plane after processing all entries with the current target L1ID
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
    hCoincidenceRateTrend->Fill(static_cast<double>(ev_count), (all_plane_have_hits ? 100.0 : 0.0));

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