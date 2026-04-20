#ifndef CORRYVRECKAN_EVENTLOADERMALTA_H
#define CORRYVRECKAN_EVENTLOADERMALTA_H

#include <iostream>
#include <vector>
#include <string>

#include "TChain.h"

#include "core/module/Module.hpp"
#include "objects/Pixel.hpp"
#include "objects/Event.hpp"

namespace corryvreckan {

    struct HitData {
        UInt_t pixel, group, parity, dcolumn, bcid, winid, l1id, phase, isDuplicate;
        void setBranchAddresses(TChain* chain) {
            chain->SetBranchAddress("pixel", &pixel);
            chain->SetBranchAddress("group", &group);
            chain->SetBranchAddress("parity", &parity);
            chain->SetBranchAddress("dcolumn", &dcolumn);
            chain->SetBranchAddress("bcid", &bcid);
            chain->SetBranchAddress("winid", &winid);
            chain->SetBranchAddress("l1id", &l1id);
            chain->SetBranchAddress("phase", &phase);
            chain->SetBranchAddress("isDuplicate", &isDuplicate);
        }
    };

    class EventLoaderMALTA : public Module {
    public:
        EventLoaderMALTA(Configuration& config, std::vector<std::shared_ptr<Detector>> detectors);
        ~EventLoaderMALTA() {}

        void initialize() override;
        StatusCode run(const std::shared_ptr<Clipboard>& clipboard) override;

    private:
        void decodeAndStore(const HitData& data, PixelVector& hits, const std::string& det_name);

        std::vector<std::unique_ptr<TChain>> chains_;
        std::vector<HitData> data_buffer_;
        std::vector<std::string> det_names_;
        std::vector<Long64_t> num_entries_;
        std::vector<Long64_t> current_indices_;

        uint32_t ev_count;
    };
}
#endif