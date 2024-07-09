#pragma once

#include "hive.h"
#include "metrics.h"

namespace NKikimr {
namespace NHive {

enum class ENodeSelectionPolicy : ui32 {
    Default,
    PreferObjectDomain,
};

struct TDomainInfo {
    TString Path;
    TTabletId HiveId = 0;
    TMaybeServerlessComputeResourcesMode ServerlessComputeResourcesMode;

    ui64 TabletsTotal = 0;
    ui64 TabletsAlive = 0;
    ui64 TabletsAliveInObjectDomain = 0;

    TMetricsMaximumUsage UserPoolUsageWindow;

    ENodeSelectionPolicy GetNodeSelectionPolicy() const;
};

} // NHive
} // NKikimr
