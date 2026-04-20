#include "EventLoaderMALTA.h"
#include "core/utils/log.h"
#include <limits>
#include <algorithm>

using namespace corryvreckan;

EventLoaderMALTA::EventLoaderMALTA(Configuration& config, std::vector<std::shared_ptr<Detector>> detectors)
    : Module(config, std::move(detectors)) {
    config_.setDefault<std::string>("file_lower", "");
    config_.setDefault<std::string>("file_upper", "");
}

void EventLoaderMALTA::initialize() {
    std::string fileL = config_.get<std::string>("file_lower");
    std::string fileU = config_.get<std::string>("file_upper");

    if (fileL.empty() || fileU.empty()) {
        throw InvalidValueError(config_, "file_lower", "Both ROOT files must be provided.");
    }

    chL.Add(fileL.c_str());
    chU.Add(fileU.c_str());

    dataL.setBranchAddresses(&chL);
    dataU.setBranchAddresses(&chU);

    nL = chL.GetEntries();
    nU = chU.GetEntries();
    iL = 0;
    iU = 0;
    ev_count = 0;

    LOG(STATUS) << "Initialized EventLoaderMALTA. Lower: " << nL << ", Upper: " << nU;
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
            case 0: case 8: ypos += 0; break; case 1: case 9: ypos += 1; break;
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
    if (iL >= nL) return StatusCode::EndRun;

    PixelVector hitsL, hitsU;
    chL.GetEntry(iL);
    UInt_t current_event_l1id = dataL.l1id;

    // Lowerを回収
    while (iL < nL) {
        chL.GetEntry(iL);
        if (dataL.l1id != current_event_l1id) break;
        decodeAndStore(dataL, hitsL, "MALTA_0");
        iL++;
    }

    // Upperを追い越さない程度に進める
    while (iU < nU) {
        chU.GetEntry(iU);
        if (dataU.l1id < current_event_l1id) iU++;
        else break;
    }

    // Upperを回収
    while (iU < nU) {
        chU.GetEntry(iU);
        if (dataU.l1id == current_event_l1id) {
            decodeAndStore(dataU, hitsU, "MALTA_1");
            iU++;
        } else break;
    }

    if (hitsL.empty() && hitsU.empty()) return StatusCode::NoData;

    double min_t = std::numeric_limits<double>::max();
    double max_t = std::numeric_limits<double>::lowest();
    for (auto& p : hitsL) { min_t = std::min(min_t, p->timestamp()); max_t = std::max(max_t, p->timestamp()); }
    for (auto& p : hitsU) { min_t = std::min(min_t, p->timestamp()); max_t = std::max(max_t, p->timestamp()); }
    if (min_t == std::numeric_limits<double>::max()) { min_t = 0; max_t = 100; }

    auto event = std::make_shared<Event>(min_t - 10.0, max_t + 10.0);
    event->addTrigger(current_event_l1id, min_t);
    clipboard->putEvent(event);

    if (!hitsL.empty()) clipboard->putData(hitsL, "MALTA_0");
    if (!hitsU.empty()) clipboard->putData(hitsU, "MALTA_1");

    ev_count++;
    return StatusCode::Success;
}