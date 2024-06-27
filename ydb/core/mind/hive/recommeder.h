#pragma once

#include <util/datetime/base.h>

namespace NKikimr::NHive {

struct TRecommenderSettings {
    double TargetCpuUtilization = 0;
    double ThresholdMargin = 0;
    ui64 ScaleOutWindowSize = 0;
    ui64 ScaleInWindowSize = 0;
};

struct TResourceRecommendation {
    ui64 CpuCores = 0;
    TInstant Timestamp;
};

} // NKikimr::NHive
