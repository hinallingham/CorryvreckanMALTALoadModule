#ifndef CORRYVRECKAN_EVENTLOADERMALTA_H
#define CORRYVRECKAN_EVENTLOADERMALTA_H

#include <chrono>
#include <iostream>
#include <vector>
#include <string>

#include "TChain.h"
#include "TH1F.h"
#include "TProfile.h"

#include "core/module/Module.hpp"
#include "objects/Pixel.hpp"
#include "objects/Event.hpp"

namespace corryvreckan {

    struct HitData {
        UInt_t pixel, group, parity, dcolumn, bcid, winid, l1id, l1idC, phase, isDuplicate;
        float  timer;
        void setBranchAddresses(TChain* chain) {
            chain->SetBranchAddress("pixel",       &pixel);
            chain->SetBranchAddress("group",       &group);
            chain->SetBranchAddress("parity",      &parity);
            chain->SetBranchAddress("dcolumn",     &dcolumn);
            chain->SetBranchAddress("bcid",        &bcid);
            chain->SetBranchAddress("winid",       &winid);
            chain->SetBranchAddress("l1id",        &l1id);
            chain->SetBranchAddress("l1idC",       &l1idC);
            chain->SetBranchAddress("phase",       &phase);
            chain->SetBranchAddress("isDuplicate", &isDuplicate);
            chain->SetBranchAddress("timer",       &timer);
        }
    };

    class EventLoaderMALTA : public Module {
    public:
        EventLoaderMALTA(Configuration& config, std::vector<std::shared_ptr<Detector>> detectors);
        ~EventLoaderMALTA() override {
            for(auto& chain : chains_) {
                if(chain) {
                    chain->ResetBranchAddresses();
                    chain.release();
                }
            }
        }

        void initialize() override;
        StatusCode run(const std::shared_ptr<Clipboard>& clipboard) override;
        void finalize(const std::shared_ptr<ReadonlyClipboard>&) override;

    private:
        void decodeAndStore(const HitData& data, PixelVector& hits, const std::string& det_name);

        std::vector<std::unique_ptr<TChain>> chains_;
        std::vector<std::vector<std::string>> chain_file_paths_;
        std::vector<HitData> data_buffer_;
        std::vector<std::string> det_names_;
        std::vector<Long64_t> num_entries_;
        std::vector<Long64_t> current_indices_;

        TProfile* hCoincidenceRateTrend;
        std::vector<TProfile*> hHitsPerEventTrend_;
        std::vector<TH1F*> hHitsPerEventDist_;

        TProfile* hL1idCVsEvent_    = nullptr;
        TH1F*     hL1idCPlaneDelta_ = nullptr;
        TProfile* hTimerVsEvent_    = nullptr;

        // counter for events processed, used for debugging and logging

        uint32_t ev_count;
        size_t skip_entries_{0};
        size_t max_events_{0};
        double total_ev_count = 0;
        double coincidence_ev_count = 0;

        enum class ColumnFilter { NONE, EVEN, ODD };
        ColumnFilter column_filter_{ColumnFilter::NONE};

        bool online_mode_{false};
        bool real_data_{false};
        bool use_l1id_{false};
        std::chrono::steady_clock::time_point last_refresh_time_;
    };
}
#endif