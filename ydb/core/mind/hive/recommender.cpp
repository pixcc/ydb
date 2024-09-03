#include "hive_impl.h"
#include "hive_log.h" // TODO(pixcc): add logs
#include "node_info.h"

#include <ydb/library/actors/core/actor_bootstrapped.h>

namespace NKikimr::NHive {

using namespace NMetrics;

class THiveRecommender : public TActorBootstrapped<THiveRecommender>, public ISubActor {
protected:
    THive* Hive;
    double TargetCpuUtilization = 0;
    double ThresholdMargin = 0;
    ui64 ScaleInWindowSize = 0;
    ui64 ScaleOutWindowSize = 0;
    TMetricsMaximumUsage& CpuUsageWindow;
    ui64 CurrentNodes = 0;

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

    // TODO(pixcc): usage vs utilization
    void AggregateUtilization() {    
        double usageSum = 0;

        for (auto& [_, node] : Hive->Nodes) {
            if (node.IsAlive()) {
                if (node.AveragedNodeTotalCpuUsage.IsValueReady()) {
                    usageSum += node.AveragedNodeTotalCpuUsage.GetValue();
                    ++CurrentNodes;
                }
            }

            node.AveragedNodeTotalCpuUsageHistory.PushBack(node.AveragedNodeTotalCpuUsage.GetValue());
            node.AveragedNodeTotalCpuUsage.Clear();
        }
        
        double averagedUsage = CurrentNodes > 0 ? usageSum / CurrentNodes : 0;
        CpuUsageWindow.SetValue(averagedUsage);

        // TODO(pixcc): listen to success, and after that make recommendation
        Hive->Execute(Hive->CreateUpdateDomain(Hive->PrimaryDomainKey));
    }

    void Recommend(ui64 newNodes, ERecommendationDirection direction = ERecommendationDirection::SCALE_NOTHING) const {
        TResourceRecommendation recommendation = {
            .Nodes = newNodes,
            .Timestamp = TActivationContext::Now(),
            .CurrentNodes = CurrentNodes,
            .Direction = direction
        };
        Hive->TabletCounters->Simple()[NHive::COUNTER_RECOMMENDED_CPU].Set(newNodes);
        Hive->LastRecommendation = recommendation;
    }

    void RecommendNothing() const {
        Recommend(CurrentNodes);
        Hive->TabletCounters->Cumulative()[NHive::COUNTER_RECOMMENDED_SCALE_NOTHING].Increment(1);
    }

    bool TryRecommendScaleOut() const {
        auto scaleOutWindowBegin = CpuUsageWindow.values().end() - ScaleOutWindowSize;
        double minUtilization = *std::min_element(scaleOutWindowBegin, CpuUsageWindow.values().end());
        if (TargetCpuUtilization > 0 && minUtilization > TargetCpuUtilization) {
            double maxUtilization = *std::min_element(scaleOutWindowBegin, CpuUsageWindow.values().end());
            double ratio = maxUtilization / TargetCpuUtilization;
            ui64 newNodes = std::ceil(CurrentNodes * ratio);
            Recommend(newNodes, ERecommendationDirection::SCALE_OUT);
            Hive->TabletCounters->Cumulative()[NHive::COUNTER_RECOMMENDED_SCALE_OUT].Increment(1);
            return true;
        }
        return false;
    }
    
    bool TryRecommendScaleIn() const {
        auto scaleInWindowBegin = CpuUsageWindow.values().end() - ScaleInWindowSize;
        double maxUtilization = *std::max_element(scaleInWindowBegin, CpuUsageWindow.values().end());
        double bottomThreshold = TargetCpuUtilization - ThresholdMargin;
        if (bottomThreshold > 0 && maxUtilization < bottomThreshold) {
            double ratio = maxUtilization / TargetCpuUtilization;
            ui64 newNodes = std::ceil(CurrentNodes * ratio);
            double newUtilization = (CurrentNodes * maxUtilization) / newNodes;
            // TODO(pixcc): margin?
            // TODO(pixcc): remove true and recheck
            if (true || newUtilization < TargetCpuUtilization) {
                Recommend(newNodes, ERecommendationDirection::SCALE_IN);
                Hive->TabletCounters->Cumulative()[NHive::COUNTER_RECOMMENDED_SCALE_IN].Increment(1);
                return true;
            }
        }
        return false;
    }

    void MakeRecommendation() const {
        size_t requiredSize = std::max(ScaleOutWindowSize, ScaleInWindowSize);
        if (CpuUsageWindow.ValuesSize() < requiredSize) {
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
        , CpuUsageWindow(Hive->Domains[Hive->PrimaryDomainKey].UserPoolUsageWindow)
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
