#include "hive_impl.h"
#include "hive_log.h" // TODO(pixcc): add logs
#include "node_info.h"

#include <ydb/library/actors/core/actor_bootstrapped.h>

namespace NKikimr::NHive {

using namespace NMetrics;

class THiveRecommender : public NActors::TActorBootstrapped<THiveRecommender>, public ISubActor {
protected:
    THive* Hive;
    double TargetCpuUtilization = 0;
    double ThresholdMargin = 0;
    ui64 ScaleInWindowSize = 0;
    ui64 ScaleOutWindowSize = 0;
    std::vector<double> AvgUtilizationWindow;
    ui64 CurrentCpuCores = 0;

    TString GetLogPrefix() const {
        return Hive->GetLogPrefix();
    }

    void PassAway() override {
        Hive->RemoveSubActor(this);
        return IActor::PassAway();
    }

    void Cleanup() override {
        PassAway();
    }

    TString GetDescription() const override {
        return TStringBuilder() << "Recommender";
    }

    TSubActorId GetId() const override {
        return SelfId().LocalId();
    }

    void AggregateUtilization() {
        size_t windowSize = 0;
        for (const auto& ni : Hive->Nodes) {
            windowSize = std::max(windowSize, ni.second.MaximumCPUUsage.ValuesSize());
            // TODO(pixcc): take it from hive?
            if (ni.second.IsAlive()) {
                CurrentCpuCores += std::get<EResource::CPU>(ni.second.GetResourceMaximumValues()) * 100 / 1000000;
            }
        }
        AvgUtilizationWindow.resize(windowSize);

        std::vector<double> utilizationSum(windowSize);
        std::vector<size_t> utilizationCount(windowSize);
        for (const auto& ni : Hive->Nodes) {
            for (size_t i = 0; i < ni.second.MaximumCPUUsage.ValuesSize(); ++i) {
                utilizationSum[i] += ni.second.MaximumCPUUsage.GetValues(i);
                ++utilizationCount[i];
            }
        }

        for (size_t i = 0; i < windowSize; ++i) {
            if (utilizationCount[i] > 0) {
                AvgUtilizationWindow[i] = utilizationSum[i] / utilizationCount[i];
            }
        }
    }

    void Recommend(ui64 newCpuCores) const {
        TResourceRecommendation recommendation = {
            .CpuCores = newCpuCores,
            .Timestamp = TActivationContext::Now()
        };
        Hive->TabletCounters->Simple()[NHive::COUNTER_RECOMMENDED_CPU].Set(newCpuCores);
        Hive->LastRecommendation = recommendation;
    }

    // TODO(pixcc): take it from hive?
    // ui64 GetCurrentCpuCores() const {
    //     return std::get<EResource::CPU>(Hive->TotalRawResourceValues) / 1000000;
    // }

    void RecommendNothing() const {
        Recommend(CurrentCpuCores);
        Hive->TabletCounters->Cumulative()[NHive::COUNTER_RECOMMENDED_SCALE_NOTHING].Increment(1);
    }

    bool TryRecommendScaleOut() const {
        auto scaleOutWindowBegin = AvgUtilizationWindow.end() - ScaleOutWindowSize;
        double minUtilization = *std::min_element(scaleOutWindowBegin, AvgUtilizationWindow.end());
        if (TargetCpuUtilization > 0 && minUtilization > TargetCpuUtilization) {
            double maxUtilization = *std::min_element(scaleOutWindowBegin, AvgUtilizationWindow.end());
            double ratio = maxUtilization / TargetCpuUtilization;
            ui64 newCpuCores = std::ceil(CurrentCpuCores * ratio);
            Recommend(newCpuCores);
            Hive->TabletCounters->Cumulative()[NHive::COUNTER_RECOMMENDED_SCALE_OUT].Increment(1);
            return true;
        }
        return false;
    }
    
    // target / cpuTime / maxCpuTime = 60 * maxCpuTime

    bool TryRecommendScaleIn() const {
        auto scaleInWindowBegin = AvgUtilizationWindow.end() - ScaleInWindowSize;
        double maxUtilization = *std::max_element(scaleInWindowBegin, AvgUtilizationWindow.end());
        double bottomThreshold = TargetCpuUtilization - ThresholdMargin;
        if (bottomThreshold > 0 && maxUtilization < bottomThreshold) {
            double ratio = maxUtilization / TargetCpuUtilization;
            ui64 newCpuCores = std::ceil(CurrentCpuCores * ratio);
            Recommend(newCpuCores);
            Hive->TabletCounters->Cumulative()[NHive::COUNTER_RECOMMENDED_SCALE_IN].Increment(1);
            return true;
        }
        return false;
    }

    void MakeRecommendation() const {
        size_t requiredSize = std::max(ScaleOutWindowSize, ScaleInWindowSize);
        if (AvgUtilizationWindow.size() < requiredSize) {
            RecommendNothing();
            return;
        }

        if (TryRecommendScaleOut()) {
            return;
        }

        if (TryRecommendScaleIn()) {
            return;
        }

        RecommendNothing();
    }

public:
    static constexpr NKikimrServices::TActivity::EType ActorActivityType() {
        return NKikimrServices::TActivity::HIVE_RECOMMENDER_ACTOR;
    }

    THiveRecommender(THive* hive, TRecommenderSettings&& settings)
        : Hive(hive)
        , TargetCpuUtilization(settings.TargetCpuUtilization)
        , ThresholdMargin(settings.ThresholdMargin)
        , ScaleInWindowSize(settings.ScaleInWindowSize)
        , ScaleOutWindowSize(settings.ScaleOutWindowSize)
    {}

    void Bootstrap() {
        Hive->TabletCounters->Cumulative()[NHive::COUNTER_RECOMMENDER_EXECUTED].Increment(1);
        Become(&THiveRecommender::StateWork);
        AggregateUtilization();
        MakeRecommendation();
        PassAway();
    }

    STATEFN(StateWork) {
        switch (ev->GetTypeRewrite()) {
            cFunc(TEvents::TSystem::PoisonPill, PassAway);
        }
    }
};

void THive::StartHiveRecommender(TRecommenderSettings&& settings) {
    auto* recommender = new THiveRecommender(this, std::move(settings));
    SubActors.emplace_back(recommender);
    RegisterWithSameMailbox(recommender);
}

} // NKikimr::NHive
