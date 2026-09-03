#ifndef CORRYVRECKAN_EVENTLOADERMALTA_H
#define CORRYVRECKAN_EVENTLOADERMALTA_H

#include <chrono>
#include <iostream>
#include <vector>
#include <string>

#include "TChain.h"
#include "TH1F.h"
#include "TH2F.h"
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
        void decodeAndStore(const HitData& data, PixelVector& hits, const std::string& det_name, size_t plane_idx);

        std::vector<std::unique_ptr<TChain>> chains_;
        std::vector<std::vector<std::string>> chain_file_paths_;
        std::vector<HitData> data_buffer_;
        std::vector<std::string> det_names_;
        std::vector<Long64_t> num_entries_;
        std::vector<Long64_t> current_indices_;

        // Coincidence requirement (run()): a candidate l1id is only accepted as a real event if
        // every plane with coincidence_required_[i]==true has a hit at that l1id. Non-DUT (regular)
        // planes only -- DUTs are exempt, since requiring the DUT to match too would systematically
        // discard genuine DUT-inefficiency events. See run()'s doc comment.
        std::vector<bool> coincidence_required_;
        size_t n_coincidence_planes_{0};

        TProfile* hCoincidenceRateTrend;
        std::vector<TProfile*> hHitsPerEventTrend_;
        std::vector<TH1F*> hHitsPerEventDist_;

        // Acceptance rate: fraction of scanned l1id candidates (both real coincidences and
        // discarded single-plane noise / ROI-violated ones) that became a real event. Filled once
        // per accepted event with 100/candidates_this_call, so a low value on the trend means many
        // candidates were discarded to find that one real event -- a live noise-rate proxy now that
        // discarded candidates are otherwise invisible downstream (see run()'s coincidence
        // requirement). total_l1id_scanned_/total_l1id_accepted_ are the matching cumulative
        // counters used for the periodic LOG(INFO) health check.
        TProfile* hL1idAcceptanceRateTrend_ = nullptr;
        long long total_l1id_scanned_ = 0;
        long long total_l1id_accepted_ = 0;

        TProfile* hL1idCVsEvent_    = nullptr;
        TH1F*     hL1idCPlaneDelta_ = nullptr;
        TProfile* hTimerVsEvent_    = nullptr;

        std::vector<TH1F*> hHitsPerL1DC_beforeROI_;
        std::vector<TH1F*> hHitsPerL1DC_afterROI_;

        // TEMPORARY investigation-only: raw-field diagnostics for the address-decode investigation (work order
        // v2). Observation only -- no cuts or corrections are derived from these, and only fields that cannot
        // be reconstructed downstream from (column, row) are captured here (isDuplicate, popcount(pixel)); the
        // rest of the diagnosis lives in the AnalysisMALTADecoding module. Filled/booked only when
        // enable_diagnostics_ is true. One histogram per plane in each vector, indexed by plane_idx.
        bool enable_diagnostics_{true};

        // word-level (one entry per decoded HitData word, before the isDuplicate check for hDuplicateFlag_,
        // after it for the rest)
        std::vector<TH1F*> hDuplicateFlag_;
        std::vector<TH1F*> hPixPopcount_;
        std::vector<TH1F*> hBcidRaw_;
        std::vector<TH1F*> hWinidRaw_;
        std::vector<TH1F*> hPhaseRaw_;

        // hit-level (one entry per set bit in the pixel one-hot word), filled after xpos/ypos are computed but
        // before column_filter_/masked() so cut hits are captured too
        std::vector<TH1F*> hGroupRaw_;
        std::vector<TH2F*> hHitmapRaw_;

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