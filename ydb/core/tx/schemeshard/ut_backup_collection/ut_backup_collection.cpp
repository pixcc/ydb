    #include <ydb/core/testlib/actors/block_events.h>
    #include <ydb/core/tx/replication/service/worker.h>
#include <ydb/core/tx/schemeshard/ut_helpers/helpers.h>

#define DEFAULT_NAME_1 "MyCollection1"
#define DEFAULT_NAME_2 "MyCollection2"

using namespace NSchemeShardUT_Private;

Y_UNIT_TEST_SUITE(TBackupCollectionTests) {
    void SetupLogging(TTestActorRuntimeBase& runtime) {
        runtime.SetLogPriority(NKikimrServices::FLAT_TX_SCHEMESHARD, NActors::NLog::PRI_TRACE);
        runtime.SetLogPriority(NKikimrServices::REPLICATION_SERVICE, NActors::NLog::PRI_TRACE);
    }

    TString DefaultCollectionSettings() {
        return R"(
            Name: ")" DEFAULT_NAME_1 R"("

            ExplicitEntryList {
                Entries {
                    Type: ETypeTable
                    Path: "/MyRoot/Table1"
                }
            }
            Cluster: {}
        )";
    }

    TString DefaultIncrementalCollectionSettings() {
        return R"(
            Name: ")" DEFAULT_NAME_1 R"("

            ExplicitEntryList {
                Entries {
                    Type: ETypeTable
                    Path: "/MyRoot/Table1"
                }
            }
            Cluster {}
            IncrementalBackupConfig {}
        )";
    }

    TString CollectionSettings(const TString& name) {
        return Sprintf(R"(
            Name: "%s"

            ExplicitEntryList {
                Entries {
                    Type: ETypeTable
                    Path: "/MyRoot/Table1"
                }
            }
            Cluster: {}
        )", name.c_str());
    }

    void PrepareDirs(TTestBasicRuntime& runtime, TTestEnv& env, ui64& txId) {
        TestMkDir(runtime, ++txId, "/MyRoot", ".backups");
        env.TestWaitNotification(runtime, txId);
        TestMkDir(runtime, ++txId, "/MyRoot/.backups", "collections");
        env.TestWaitNotification(runtime, txId);
    }

    Y_UNIT_TEST(HiddenByFeatureFlag) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions());
        ui64 txId = 100;

        SetupLogging(runtime);

        PrepareDirs(runtime, env, txId);

        TestCreateBackupCollection(runtime, ++txId, "/MyRoot", DefaultCollectionSettings(), {NKikimrScheme::StatusPreconditionFailed});

        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/.backups/collections/" DEFAULT_NAME_1), {
            NLs::PathNotExist,
        });

        // must not be there in any case, smoke test
        TestDescribeResult(DescribePath(runtime, "/MyRoot/" DEFAULT_NAME_1), {
            NLs::PathNotExist,
        });
    }

    Y_UNIT_TEST(DisallowedPath) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableBackupService(true));
        ui64 txId = 100;

        SetupLogging(runtime);

        PrepareDirs(runtime, env, txId);

        {
            TestCreateBackupCollection(runtime, ++txId, "/MyRoot", DefaultCollectionSettings(), {NKikimrScheme::EStatus::StatusSchemeError});

            env.TestWaitNotification(runtime, txId);

            TestDescribeResult(DescribePath(runtime, "/MyRoot/.backups/collections/" DEFAULT_NAME_1), {
                NLs::PathNotExist,
            });

            TestDescribeResult(DescribePath(runtime, "/MyRoot/" DEFAULT_NAME_1), {
                NLs::PathNotExist,
            });
        }

        {
            TestCreateBackupCollection(runtime, ++txId, "/MyRoot/.backups", DefaultCollectionSettings(), {NKikimrScheme::EStatus::StatusSchemeError});

            env.TestWaitNotification(runtime, txId);

            TestDescribeResult(DescribePath(runtime, "/MyRoot/.backups/collections/" DEFAULT_NAME_1), {
                NLs::PathNotExist,
            });

            TestDescribeResult(DescribePath(runtime, "/MyRoot/.backups/" DEFAULT_NAME_1), {
                NLs::PathNotExist,
            });
        }

        {
            TestCreateBackupCollection(runtime, ++txId, "/MyRoot/.backups/collections/", CollectionSettings("SomePrefix/MyCollection1"), {NKikimrScheme::EStatus::StatusSchemeError});

            env.TestWaitNotification(runtime, txId);

            TestDescribeResult(DescribePath(runtime, "/MyRoot/.backups/collections/SomePrefix/MyCollection1"), {
                NLs::PathNotExist,
            });
        }
    }

    Y_UNIT_TEST(CreateAbsolutePath) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableBackupService(true));
        ui64 txId = 100;

        SetupLogging(runtime);

        PrepareDirs(runtime, env, txId);

        TestCreateBackupCollection(runtime, ++txId, "/MyRoot", CollectionSettings("/MyRoot/.backups/collections/" DEFAULT_NAME_1));

        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/.backups/collections/" DEFAULT_NAME_1), {
            NLs::PathExist,
            NLs::IsBackupCollection,
        });
    }

    Y_UNIT_TEST(Create) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableBackupService(true));
        ui64 txId = 100;

        SetupLogging(runtime);

        PrepareDirs(runtime, env, txId);

        TestCreateBackupCollection(runtime, ++txId, "/MyRoot/.backups/collections/", DefaultCollectionSettings());

        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/.backups/collections/" DEFAULT_NAME_1), {
            NLs::PathExist,
            NLs::IsBackupCollection,
        });
    }

    Y_UNIT_TEST(CreateTwice) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableBackupService(true));
        ui64 txId = 100;

        SetupLogging(runtime);

        PrepareDirs(runtime, env, txId);

        TestCreateBackupCollection(runtime, ++txId, "/MyRoot/.backups/collections/", DefaultCollectionSettings());

        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/.backups/collections/" DEFAULT_NAME_1), {
            NLs::PathExist,
            NLs::IsBackupCollection,
        });

        TestCreateBackupCollection(runtime, ++txId, "/MyRoot/.backups/collections/", DefaultCollectionSettings(), {NKikimrScheme::EStatus::StatusSchemeError});

        env.TestWaitNotification(runtime, txId);
    }

    Y_UNIT_TEST(ParallelCreate) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableBackupService(true));
        ui64 txId = 100;

        PrepareDirs(runtime, env, txId);

        AsyncCreateBackupCollection(runtime, ++txId, "/MyRoot/.backups/collections", CollectionSettings(DEFAULT_NAME_1));
        AsyncCreateBackupCollection(runtime, ++txId, "/MyRoot/.backups/collections", CollectionSettings(DEFAULT_NAME_2));
        TestModificationResult(runtime, txId - 1, NKikimrScheme::StatusAccepted);
        TestModificationResult(runtime, txId, NKikimrScheme::StatusAccepted);

        env.TestWaitNotification(runtime, {txId, txId - 1});

        TestDescribe(runtime, "/MyRoot/.backups/collections/" DEFAULT_NAME_1);
        TestDescribe(runtime, "/MyRoot/.backups/collections/" DEFAULT_NAME_2);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/.backups/collections"),
                           {NLs::PathVersionEqual(7)});
        TestDescribeResult(DescribePath(runtime, "/MyRoot/.backups/collections/" DEFAULT_NAME_1),
                           {NLs::PathVersionEqual(1)});
        TestDescribeResult(DescribePath(runtime, "/MyRoot/.backups/collections/" DEFAULT_NAME_2),
                           {NLs::PathVersionEqual(1)});
    }

    Y_UNIT_TEST(Drop) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableBackupService(true));
        ui64 txId = 100;

        PrepareDirs(runtime, env, txId);

        TestCreateBackupCollection(runtime, ++txId, "/MyRoot/.backups/collections", DefaultCollectionSettings());
        env.TestWaitNotification(runtime, txId);

        TestLs(runtime, "/MyRoot/.backups/collections/" DEFAULT_NAME_1, false, NLs::PathExist);

        TestDropBackupCollection(runtime, ++txId, "/MyRoot/.backups/collections", "Name: \"" DEFAULT_NAME_1 "\"");
        env.TestWaitNotification(runtime, txId);

        TestLs(runtime, "/MyRoot/.backups/collections/" DEFAULT_NAME_1, false, NLs::PathNotExist);
    }

    Y_UNIT_TEST(DropTwice) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableBackupService(true));
        ui64 txId = 100;

        PrepareDirs(runtime, env, txId);

        TestCreateBackupCollection(runtime, ++txId, "/MyRoot/.backups/collections", DefaultCollectionSettings());
        env.TestWaitNotification(runtime, txId);

        AsyncDropBackupCollection(runtime, ++txId, "/MyRoot/.backups/collections", "Name: \"" DEFAULT_NAME_1 "\"");
        AsyncDropBackupCollection(runtime, ++txId, "/MyRoot/.backups/collections", "Name: \"" DEFAULT_NAME_1 "\"");
        TestModificationResult(runtime, txId - 1);

        auto ev = runtime.GrabEdgeEvent<TEvSchemeShard::TEvModifySchemeTransactionResult>();
        UNIT_ASSERT(ev);

        const auto& record = ev->Record;
        UNIT_ASSERT_VALUES_EQUAL(record.GetTxId(), txId);
        UNIT_ASSERT_VALUES_EQUAL(record.GetStatus(), NKikimrScheme::StatusMultipleModifications);
        UNIT_ASSERT_VALUES_EQUAL(record.GetPathDropTxId(), txId - 1);

        env.TestWaitNotification(runtime, txId - 1);
        TestLs(runtime, "/MyRoot/.backups/collections/" DEFAULT_NAME_1, false, NLs::PathNotExist);
    }

    Y_UNIT_TEST(TableWithSystemColumns) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableBackupService(true));
        ui64 txId = 100;

        SetupLogging(runtime);

        PrepareDirs(runtime, env, txId);

        TestCreateBackupCollection(runtime, ++txId, "/MyRoot/.backups/collections/", DefaultCollectionSettings());

        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/.backups/collections/" DEFAULT_NAME_1), {
            NLs::PathExist,
            NLs::IsBackupCollection,
        });

        TestCreateTable(runtime, ++txId, "/MyRoot/.backups/collections", R"(
            Name: "Table1"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "__ydb_system_column" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: ".backups/collections/Table2"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "__ydb_system_column" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        TestCreateTable(runtime, ++txId, "/MyRoot/.backups/collections", R"(
            Name: "somepath/Table3"
            Columns { Name: "key" Type: "Utf8" }
            Columns { Name: "__ydb_system_column" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);
    }

    Y_UNIT_TEST(BackupAbsentCollection) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableBackupService(true));
        ui64 txId = 100;

        SetupLogging(runtime);

        PrepareDirs(runtime, env, txId);

        TestBackupBackupCollection(runtime, ++txId, "/MyRoot",
            R"(Name: ".backups/collections/)" DEFAULT_NAME_1 R"(")",
            {NKikimrScheme::EStatus::StatusPathDoesNotExist});
        env.TestWaitNotification(runtime, txId);
    }

    Y_UNIT_TEST(BackupDroppedCollection) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableBackupService(true));
        ui64 txId = 100;

        SetupLogging(runtime);

        PrepareDirs(runtime, env, txId);

        TestCreateBackupCollection(runtime, ++txId, "/MyRoot/.backups/collections", DefaultCollectionSettings());
        env.TestWaitNotification(runtime, txId);

        TestDropBackupCollection(runtime, ++txId, "/MyRoot/.backups/collections", "Name: \"" DEFAULT_NAME_1 "\"");
        env.TestWaitNotification(runtime, txId);

        TestBackupBackupCollection(runtime, ++txId, "/MyRoot",
            R"(Name: ".backups/collections/)" DEFAULT_NAME_1 R"(")",
            {NKikimrScheme::EStatus::StatusPathDoesNotExist});
        env.TestWaitNotification(runtime, txId);
    }

    Y_UNIT_TEST(BackupAbsentDirs) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableBackupService(true));
        ui64 txId = 100;

        SetupLogging(runtime);

        PrepareDirs(runtime, env, txId);

        TestCreateBackupCollection(runtime, ++txId, "/MyRoot/.backups/collections", DefaultCollectionSettings());
        env.TestWaitNotification(runtime, txId);

        TestBackupBackupCollection(runtime, ++txId, "/MyRoot",
            R"(Name: ".backups/collections/)" DEFAULT_NAME_1 R"(")",
            {NKikimrScheme::EStatus::StatusPathDoesNotExist});
        env.TestWaitNotification(runtime, txId);
    }

    Y_UNIT_TEST(BackupNonIncrementalCollection) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableBackupService(true));
        ui64 txId = 100;

        SetupLogging(runtime);

        PrepareDirs(runtime, env, txId);

        TestCreateBackupCollection(runtime, ++txId, "/MyRoot/.backups/collections/", DefaultCollectionSettings());

        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/.backups/collections/" DEFAULT_NAME_1), {
            NLs::PathExist,
            NLs::IsBackupCollection,
        });

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table1"
            Columns { Name: "key" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        TestBackupBackupCollection(runtime, ++txId, "/MyRoot",
            R"(Name: ".backups/collections/)" DEFAULT_NAME_1 R"(")");
        env.TestWaitNotification(runtime, txId);

        TestBackupIncrementalBackupCollection(runtime, ++txId, "/MyRoot",
            R"(Name: ".backups/collections/)" DEFAULT_NAME_1 R"(")",
            {NKikimrScheme::EStatus::StatusInvalidParameter});
        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/.backups/collections/" DEFAULT_NAME_1), {
            NLs::PathExist,
            NLs::IsBackupCollection,
            NLs::ChildrenCount(1),
            NLs::Finished,
        });
    }

    Y_UNIT_TEST(IncrementalBackupOperation) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableBackupService(true));
        ui64 txId = 100;

        SetupLogging(runtime);

        PrepareDirs(runtime, env, txId);

        TestCreateBackupCollection(runtime, ++txId, "/MyRoot/.backups/collections/", DefaultIncrementalCollectionSettings());

        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/.backups/collections/" DEFAULT_NAME_1), {
            NLs::PathExist,
            NLs::IsBackupCollection,
        });

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table1"
            Columns { Name: "key" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        TestBackupBackupCollection(runtime, ++txId, "/MyRoot",
            R"(Name: ".backups/collections/)" DEFAULT_NAME_1 R"(")");
        env.TestWaitNotification(runtime, txId);

        runtime.AdvanceCurrentTime(TDuration::Seconds(1));

        TestBackupIncrementalBackupCollection(runtime, ++txId, "/MyRoot",
            R"(Name: ".backups/collections/)" DEFAULT_NAME_1 R"(")");
        const auto backupId = txId;
        env.TestWaitNotification(runtime, backupId);

        auto r1 = TestGetIncrementalBackup(runtime, backupId, "/MyRoot");
        UNIT_ASSERT_VALUES_EQUAL(r1.GetIncrementalBackup().GetProgress(), Ydb::Backup::BackupProgress::PROGRESS_TRANSFER_DATA);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/.backups/collections/" DEFAULT_NAME_1), {
            NLs::PathExist,
            NLs::IsBackupCollection,
            NLs::ChildrenCount(2),
            NLs::Finished,
        });

        runtime.SimulateSleep(TDuration::Seconds(5));

        auto r2 = TestGetIncrementalBackup(runtime, backupId, "/MyRoot");
        UNIT_ASSERT_VALUES_EQUAL(r2.GetIncrementalBackup().GetProgress(), Ydb::Backup::BackupProgress::PROGRESS_DONE);

        TestForgetIncrementalBackup(runtime, txId++, "/MyRoot", backupId);

        TestGetIncrementalBackup(runtime, backupId, "/MyRoot", Ydb::StatusIds::NOT_FOUND);
    }

    Y_UNIT_TEST(EmptyIncrementalBackupRace) {
        TTestBasicRuntime runtime;
        TTestEnv env(runtime, TTestEnvOptions().EnableBackupService(true));
        ui64 txId = 100;

        SetupLogging(runtime);

        PrepareDirs(runtime, env, txId);

        TestCreateBackupCollection(runtime, ++txId, "/MyRoot/.backups/collections/", DefaultIncrementalCollectionSettings());

        env.TestWaitNotification(runtime, txId);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/.backups/collections/" DEFAULT_NAME_1), {
            NLs::PathExist,
            NLs::IsBackupCollection,
        });

        TestCreateTable(runtime, ++txId, "/MyRoot", R"(
            Name: "Table1"
            Columns { Name: "key" Type: "Utf8" }
            KeyColumnNames: ["key"]
        )");
        env.TestWaitNotification(runtime, txId);

        TestBackupBackupCollection(runtime, ++txId, "/MyRoot",
            R"(Name: ".backups/collections/)" DEFAULT_NAME_1 R"(")");
        env.TestWaitNotification(runtime, txId);

        runtime.AdvanceCurrentTime(TDuration::Seconds(1));

        // TEvDataEnd should be received before TEvHandshake with writer
        TBlockEvents<NReplication::NService::TEvWorker::TEvHandshake> block(runtime);
        runtime.SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
            if (ev->GetTypeRewrite() == NReplication::NService::TEvWorker::TEvDataEnd::EventType) {
                block.Unblock();
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        });

        TestBackupIncrementalBackupCollection(runtime, ++txId, "/MyRoot",
            R"(Name: ".backups/collections/)" DEFAULT_NAME_1 R"(")");
        const auto backupId = txId;
        env.TestWaitNotification(runtime, backupId);

        runtime.WaitFor("block handshakes with reader & writer", [&] { return block.size() == 2; });

        // Unblock TEvHandhshake with reader, but stil block TEvHandshake with writer
        block.Stop();
        block.Unblock(1);

        auto r1 = TestGetIncrementalBackup(runtime, backupId, "/MyRoot");
        UNIT_ASSERT_VALUES_EQUAL(r1.GetIncrementalBackup().GetProgress(), Ydb::Backup::BackupProgress::PROGRESS_TRANSFER_DATA);

        TestDescribeResult(DescribePath(runtime, "/MyRoot/.backups/collections/" DEFAULT_NAME_1), {
            NLs::PathExist,
            NLs::IsBackupCollection,
            NLs::ChildrenCount(2),
            NLs::Finished,
        });

        runtime.SimulateSleep(TDuration::Seconds(5));

        auto r2 = TestGetIncrementalBackup(runtime, backupId, "/MyRoot");
        UNIT_ASSERT_VALUES_EQUAL(r2.GetIncrementalBackup().GetProgress(), Ydb::Backup::BackupProgress::PROGRESS_DONE);

        TestForgetIncrementalBackup(runtime, txId++, "/MyRoot", backupId);

        TestGetIncrementalBackup(runtime, backupId, "/MyRoot", Ydb::StatusIds::NOT_FOUND);
    }
} // TBackupCollectionTests
