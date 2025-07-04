#include <ydb/core/protos/sys_view_types.pb.h>
#include <ydb/core/tx/schemeshard/ut_helpers/helpers.h>

namespace {

    using namespace NSchemeShardUT_Private;
    using NKikimrScheme::EStatus;
    using NKikimrSysView::ESysViewType;

    void ExpectEqualSysViewDescription(const NKikimrScheme::TEvDescribeSchemeResult& describeResult,
        const TString& sysViewName, const ESysViewType sysViewType, const TPathId& sourceObjectPathId)
    {
        UNIT_ASSERT(describeResult.HasPathDescription());
        UNIT_ASSERT(describeResult.GetPathDescription().HasSysViewDescription());
        const auto& sysViewDescription = describeResult.GetPathDescription().GetSysViewDescription();
        UNIT_ASSERT_VALUES_EQUAL(sysViewDescription.GetName(), sysViewName);
        UNIT_ASSERT(sysViewDescription.GetType() == sysViewType);
        UNIT_ASSERT_VALUES_EQUAL(TPathId::FromProto(sysViewDescription.GetSourceObject()), sourceObjectPathId);
    }
}

Y_UNIT_TEST_SUITE(TSchemeShardSysViewTest) {
    Y_UNIT_TEST(CreateSysView) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableSystemNamesProtection(true));
        ui64 txId = 100;

        TestMkDir(runtime, ++txId, "/MyRoot", ".sys");
        env.TestWaitNotification(runtime, txId);

        TestCreateSysView(runtime, ++txId, "/MyRoot/.sys",
                          R"(
                             Name: "new_sys_view"
                             Type: EPartitionStats
                            )");
        env.TestWaitNotification(runtime, txId);

        {
            const auto describeResult = DescribePath(runtime, "/MyRoot/.sys/new_sys_view");
            const auto& sysViewPath = describeResult.GetPathDescription().GetSelf();
            const auto domainPathId = TPathId(sysViewPath.GetSchemeshardId(), 1);
            TestDescribeResult(describeResult, {NLs::Finished, NLs::IsSysView});
            ExpectEqualSysViewDescription(describeResult, "new_sys_view", ESysViewType::EPartitionStats, domainPathId);
        }

        TActorId sender = runtime.AllocateEdgeActor();
        RebootTablet(runtime, TTestTxConfig::SchemeShard, sender);

        {
            const auto describeResult = DescribePath(runtime, "/MyRoot/.sys/new_sys_view");
            const auto& sysViewPath = describeResult.GetPathDescription().GetSelf();
            const auto domainPathId = TPathId(sysViewPath.GetSchemeshardId(), 1);
            TestDescribeResult(describeResult, {NLs::Finished, NLs::IsSysView});
            ExpectEqualSysViewDescription(describeResult, "new_sys_view", ESysViewType::EPartitionStats, domainPathId);
        }
    }

    Y_UNIT_TEST(DropSysView) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableSystemNamesProtection(true));
        ui64 txId = 100;

        TestMkDir(runtime, ++txId, "/MyRoot", ".sys");
        env.TestWaitNotification(runtime, txId);
        TestCreateSysView(runtime, ++txId, "/MyRoot/.sys",
                          R"(
                             Name: "new_sys_view"
                             Type: EPartitionStats
                            )");
        env.TestWaitNotification(runtime, txId);
        TestLs(runtime, "/MyRoot/.sys/new_sys_view", false, NLs::PathExist);

        TestDropSysView(runtime, ++txId, "/MyRoot/.sys", "new_sys_view");
        env.TestWaitNotification(runtime, txId);
        TestLs(runtime, "/MyRoot/.sys/new_sys_view", false, NLs::PathNotExist);

        TActorId sender = runtime.AllocateEdgeActor();
        RebootTablet(runtime, TTestTxConfig::SchemeShard, sender);
        TestLs(runtime, "/MyRoot/.sys/new_sys_view", false, NLs::PathNotExist);
    }

    Y_UNIT_TEST(CreateExistingSysView) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableSystemNamesProtection(true));
        ui64 txId = 100;

        TestMkDir(runtime, ++txId, "/MyRoot", ".sys");
        env.TestWaitNotification(runtime, txId);

        TestCreateSysView(runtime, ++txId, "/MyRoot/.sys",
                          R"(
                             Name: "new_sys_view"
                             Type: EPartitionStats
                            )");
        env.TestWaitNotification(runtime, txId);
        TestLs(runtime, "/MyRoot/.sys/new_sys_view", false, NLs::PathExist);

        TestCreateSysView(runtime, ++txId, "/MyRoot/.sys",
                          R"(
                             Name: "new_sys_view"
                             Type: ENodes
                            )",
                          {EStatus::StatusSchemeError, EStatus::StatusAlreadyExists});
        env.TestWaitNotification(runtime, txId);
        const auto describeResult = DescribePath(runtime, "/MyRoot/.sys/new_sys_view");
        const auto& sysViewPath = describeResult.GetPathDescription().GetSelf();
        const auto domainPathId = TPathId(sysViewPath.GetSchemeshardId(), 1);
        TestDescribeResult(describeResult, {NLs::Finished, NLs::IsSysView});
        ExpectEqualSysViewDescription(describeResult, "new_sys_view", ESysViewType::EPartitionStats, domainPathId);
    }

    Y_UNIT_TEST(AsyncCreateDifferentSysViews) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableSystemNamesProtection(true));
        ui64 txId = 100;

        TestMkDir(runtime, ++txId, "/MyRoot", ".sys");
        env.TestWaitNotification(runtime, txId);

        AsyncCreateSysView(runtime, ++txId, "/MyRoot/.sys",
                           R"(
                              Name: "sys_view_1"
                              Type: EPartitionStats
                             )");
        AsyncCreateSysView(runtime, ++txId, "/MyRoot/.sys",
                           R"(
                              Name: "sys_view_2"
                              Type: ENodes
                             )");

        TestModificationResult(runtime, txId - 1);
        TestModificationResult(runtime, txId);
        env.TestWaitNotification(runtime, {txId - 1, txId});

        {
            const auto describeResult = DescribePath(runtime, "/MyRoot/.sys/sys_view_1");
            const auto& sysViewPath = describeResult.GetPathDescription().GetSelf();
            const auto domainPathId = TPathId(sysViewPath.GetSchemeshardId(), 1);
            TestDescribeResult(describeResult, {NLs::Finished, NLs::IsSysView});
            ExpectEqualSysViewDescription(describeResult, "sys_view_1", ESysViewType::EPartitionStats, domainPathId);
        }
        {
            const auto describeResult = DescribePath(runtime, "/MyRoot/.sys/sys_view_2");
            const auto& sysViewPath = describeResult.GetPathDescription().GetSelf();
            const auto domainPathId = TPathId(sysViewPath.GetSchemeshardId(), 1);
            TestDescribeResult(describeResult, {NLs::Finished, NLs::IsSysView});
            ExpectEqualSysViewDescription(describeResult, "sys_view_2", ESysViewType::ENodes, domainPathId);
        }
    }

    Y_UNIT_TEST(AsyncCreateDirWithSysView) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableSystemNamesProtection(true));
        ui64 txId = 100;

        AsyncMkDir(runtime, ++txId, "/MyRoot", ".sys");
        AsyncCreateSysView(runtime, ++txId, "/MyRoot/.sys",
                           R"(
                              Name: "new_sys_view"
                              Type: EPartitionStats
                             )");

        TestModificationResult(runtime, txId - 1);
        TestModificationResult(runtime, txId);
        env.TestWaitNotification(runtime, {txId - 1, txId});

        TestDescribeResult(DescribePath(runtime, "/MyRoot/.sys"), {NLs::Finished});

        const auto describeResult = DescribePath(runtime, "/MyRoot/.sys/new_sys_view");
        const auto& sysViewPath = describeResult.GetPathDescription().GetSelf();
        const auto domainPathId = TPathId(sysViewPath.GetSchemeshardId(), 1);
        TestDescribeResult(describeResult, {NLs::Finished, NLs::IsSysView});
        ExpectEqualSysViewDescription(describeResult, "new_sys_view", ESysViewType::EPartitionStats, domainPathId);
    }

    Y_UNIT_TEST(AsyncCreateSameSysView) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableSystemNamesProtection(true));
        ui64 txId = 100;

        TestMkDir(runtime, ++txId, "/MyRoot", ".sys");
        env.TestWaitNotification(runtime, txId);

        AsyncCreateSysView(runtime, ++txId, "/MyRoot/.sys",
                           R"(
                              Name: "new_sys_view"
                              Type: EPartitionStats
                             )");
        AsyncCreateSysView(runtime, ++txId, "/MyRoot/.sys",
                           R"(
                              Name: "new_sys_view"
                              Type: EPartitionStats
                             )");
        const TVector<TExpectedResult> expectedResults = {EStatus::StatusAccepted,
                                                          EStatus::StatusMultipleModifications,
                                                          EStatus::StatusAlreadyExists};
        TestModificationResults(runtime, txId - 1, expectedResults);
        TestModificationResults(runtime, txId, expectedResults);
        env.TestWaitNotification(runtime, {txId - 1, txId});

        const auto describeResult = DescribePath(runtime, "/MyRoot/.sys/new_sys_view");
        const auto& sysViewPath = describeResult.GetPathDescription().GetSelf();
        const auto domainPathId = TPathId(sysViewPath.GetSchemeshardId(), 1);
        TestDescribeResult(describeResult, {NLs::Finished, NLs::IsSysView});
        ExpectEqualSysViewDescription(describeResult, "new_sys_view", ESysViewType::EPartitionStats, domainPathId);
    }

    Y_UNIT_TEST(AsyncDropSameSysView) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableSystemNamesProtection(true));
        ui64 txId = 100;

        TestMkDir(runtime, ++txId, "/MyRoot", ".sys");
        env.TestWaitNotification(runtime, txId);
        TestCreateSysView(runtime, ++txId, "/MyRoot/.sys",
                          R"(
                             Name: "new_sys_view"
                             Type: EPartitionStats
                            )");
        env.TestWaitNotification(runtime, txId);
        TestLs(runtime, "/MyRoot/.sys/new_sys_view", false, NLs::PathExist);

        AsyncDropSysView(runtime, ++txId, "/MyRoot/.sys", "new_sys_view");
        AsyncDropSysView(runtime, ++txId, "/MyRoot/.sys", "new_sys_view");
        const TVector<TExpectedResult> expectedResults = {EStatus::StatusAccepted,
                                                          EStatus::StatusMultipleModifications,
                                                          EStatus::StatusPathDoesNotExist};
        TestModificationResults(runtime, txId - 1, expectedResults);
        TestModificationResults(runtime, txId, expectedResults);
        env.TestWaitNotification(runtime, {txId - 1, txId});

        TestLs(runtime, "/MyRoot/.sys/new_sys_view", false, NLs::PathNotExist);
    }

    Y_UNIT_TEST(ReadOnlyMode) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableSystemNamesProtection(true));
        ui64 txId = 100;

        TestMkDir(runtime, ++txId, "/MyRoot", ".sys");
        env.TestWaitNotification(runtime, txId);
        SetSchemeshardReadOnlyMode(runtime, true);
        TActorId sender = runtime.AllocateEdgeActor();
        RebootTablet(runtime, TTestTxConfig::SchemeShard, sender);

        TestCreateSysView(runtime, ++txId, "/MyRoot/.sys",
                          R"(
                             Name: "new_sys_view"
                             Type: EPartitionStats
                            )",
                          {{EStatus::StatusReadOnly}});
        env.TestWaitNotification(runtime, txId);
        TestLs(runtime, "/MyRoot/.sys/new_sys_view", false, NLs::PathNotExist);

        SetSchemeshardReadOnlyMode(runtime, false);
        sender = runtime.AllocateEdgeActor();
        RebootTablet(runtime, TTestTxConfig::SchemeShard, sender);

        TestCreateSysView(runtime, ++txId, "/MyRoot/.sys",
                          R"(
                             Name: "new_sys_view"
                             Type: EPartitionStats
                            )");
        env.TestWaitNotification(runtime, txId);
        TestLs(runtime, "/MyRoot/.sys/new_sys_view", false, NLs::PathExist);
    }

    Y_UNIT_TEST(EmptyName) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableSystemNamesProtection(true));
        ui64 txId = 100;

        TestMkDir(runtime, ++txId, "/MyRoot", ".sys");
        env.TestWaitNotification(runtime, txId);

        TestCreateSysView(runtime, ++txId, "/MyRoot/.sys",
                          R"(
                             Name: ""
                             Type: EPartitionStats
                            )",
                          {{EStatus::StatusSchemeError, "error: path part shouldn't be empty"}});
        env.TestWaitNotification(runtime, txId);
    }
}
