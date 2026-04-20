#include "EventLoaderMALTA.h"
#include "core/utils/log.h"
#include <limits>
#include <algorithm>

using namespace corryvreckan;

EventLoaderMALTA::EventLoaderMALTA(Configuration& config, std::vector<std::shared_ptr<Detector>> detectors)
    : Module(config, std::move(detectors)) {
}

void EventLoaderMALTA::initialize() {
    
    std::vector<std::string> files = config_.getArray<std::string>("file_names");
    det_names_ = config_.getArray<std::string>("detector_names");

    if (files.size() != det_names_.size()) {
        throw InvalidValueError(config_, "file_names", "file_names and detector_names are not consistent in length");
    }

    size_t n_planes = files.size();
    data_buffer_.resize(n_planes);
    num_entries_.resize(n_planes);
    current_indices_.assign(n_planes, 0);
    ev_count = 0;

    for (size_t i = 0; i < n_planes; ++i) {
        auto chain = std::make_unique<TChain>("MALTA");
        chain->Add(files[i].c_str());
        
        num_entries_[i] = chain->GetEntries();
        if (num_entries_[i] == 0) {
            throw InvalidValueError(config_, "file_names", "Not found or empty: " + files[i]);
        }

        data_buffer_[i].setBranchAddresses(chain.get());
        chains_.push_back(std::move(chain));

        LOG(STATUS) << "Plane " << i << " [" << det_names_[i] << "]: " << num_entries_[i] << " entries.";
    }
}

void EventLoaderMALTA::decodeAndStore(const HitData& data, PixelVector& hits, const std::string& det_name) {
    if (data.isDuplicate == 1) return;
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
        double time_ns = static_cast<double>(data.bcid) * 25.0 + 
                         static_cast<double>(data.winid) * 3.125 + 
                         static_cast<double>(data.phase) * 0.39;

        hits.push_back(std::make_shared<Pixel>(det_name, xpos, ypos, 1, 1, time_ns));
    }
}

StatusCode EventLoaderMALTA::run(const std::shared_ptr<Clipboard>& clipboard) {
    size_t n_planes = chains_.size();
    if (current_indices_[0] >= num_entries_[0]) return StatusCode::EndRun;

    // Fixed master plane (0) to determine target L1ID, and then catch up other planes to the same L1ID, and collect all hits with that L1ID
    chains_[0]->GetEntry(current_indices_[0]);
    UInt_t target_l1id = data_buffer_[0].l1id;

    std::vector<PixelVector> all_hits_vec(n_planes);

    for (size_t i = 0; i < n_planes; ++i) {

        if (i > 0) {
            while (current_indices_[i] < num_entries_[i]) {
                chains_[i]->GetEntry(current_indices_[i]);
                if (data_buffer_[i].l1id < target_l1id) current_indices_[i]++;
                else break;
            }
        }
        while (current_indices_[i] < num_entries_[i]) {
            chains_[i]->GetEntry(current_indices_[i]);
            if (data_buffer_[i].l1id == target_l1id) {
                decodeAndStore(data_buffer_[i], all_hits_vec[i], det_names_[i]);
                current_indices_[i]++;
            } else break;
        }
    }

    double min_t = std::numeric_limits<double>::max();
    double max_t = std::numeric_limits<double>::lowest();
    bool has_any_hits = false;

    for (const auto& hits : all_hits_vec) {
        for (const auto& p : hits) {
            min_t = std::min(min_t, p->timestamp());
            max_t = std::max(max_t, p->timestamp());
            has_any_hits = true;
        }
    }

    if (!has_any_hits) return StatusCode::NoData;

    auto event = std::make_shared<Event>(min_t - 10.0, max_t + 10.0);
    event->addTrigger(target_l1id, min_t);
    clipboard->putEvent(event);

    for (size_t i = 0; i < n_planes; ++i) {
        if (!all_hits_vec[i].empty()) {
            clipboard->putData(all_hits_vec[i], det_names_[i]);
        }
    }

    ev_count++;
    return StatusCode::Success;
}