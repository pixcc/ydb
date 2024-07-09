#pragma once

#include <util/datetime/base.h>

namespace NKikimr::NHive {

struct TRecommenderSettings {
    double TargetCpuUtilization = 0;
    double ThresholdMargin = 0;
    ui64 ScaleOutWindowSize = 0;
    ui64 ScaleInWindowSize = 0;
};

enum class ERecommendationDirection {
    SCALE_OUT,
    SCALE_IN,
    SCALE_NOTHING
};

struct TResourceRecommendation {
    ui64 CpuCores = 0;
    TInstant Timestamp;

    // TODO(pixcc): testing
    ui64 CurrentCpuCores = 0;
    ERecommendationDirection Direction = ERecommendationDirection::SCALE_NOTHING;
};

} // NKikimr::NHive
