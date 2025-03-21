#include <ydb/core/kqp/ut/common/kqp_ut_common.h>
#include <ydb/core/kqp/rm_service/kqp_rm_service.h>
#include <ydb/core/tx/datashard/datashard.h>
#include <ydb/core/tx/datashard/datashard_ut_common_kqp.h>
#include <ydb/core/base/tablet_pipecache.h>
#include <ydb/core/tx/scheme_cache/scheme_cache.h>

namespace NKikimr {
namespace NKqp {

using namespace Ydb;
using namespace NYql;
using namespace Tests;
using namespace NKikimr::NDataShard::NKqpHelpers;
using namespace NKikimrTxDataShard;

namespace {

bool HasIssueImpl(const TIssues& issues, ui32 code, TStringBuf message, std::function<bool(const TIssue& issue)> predicate, bool contains) {
    bool hasIssue = false;

    for (auto& issue : issues) {
        WalkThroughIssues(issue, false, [&] (const TIssue& issue, int) {
            if (!hasIssue && issue.GetCode() == code && (!message || message == issue.GetMessage() || (contains && issue.GetMessage().Contains(message)))) {
                hasIssue = !predicate || predicate(issue);
            }
        });
    }

    return hasIssue;
}

bool HasIssue(const TIssues& issues, ui32 code, TStringBuf message, std::function<bool(const TIssue& issue)> predicate = {}) {
    return HasIssueImpl(issues, code, message, predicate, false);
}

bool HasIssueContains(const TIssues& issues, ui32 code, TStringBuf message, std::function<bool(const TIssue& issue)> predicate = {}) {
    return HasIssueImpl(issues, code, message, predicate, true);
}

} // anonymous namespace

class TLocalFixture {
public:
    TLocalFixture(bool enableResourcePools = true, std::optional<bool> enableOltpSink = std::nullopt) {
        TPortManager pm;
        NKikimrConfig::TAppConfig app;
        app.MutableFeatureFlags()->SetEnableResourcePools(enableResourcePools);
        if (enableOltpSink) {
            app.MutableTableServiceConfig()->SetEnableOltpSink(*enableOltpSink);
        }
        TServerSettings serverSettings(pm.GetPort(2134));
        serverSettings.SetDomainName("Root")
            .SetNodeCount(2)
            .SetUseRealThreads(false)
            .SetEnableResourcePools(enableResourcePools)
            .SetAppConfig(app);

        Server = new TServer(serverSettings);
        Runtime = Server->GetRuntime();

        Runtime->SetLogPriority(NKikimrServices::KQP_RESOURCE_MANAGER, NActors::NLog::PRI_DEBUG);

        TDispatchOptions rmReady;
        rmReady.CustomFinalCondition = [this] {
            for (ui32 i = 0; i < Runtime->GetNodeCount(); ++i) {
                ui32 nodeId = Runtime->GetNodeId(i);
                if (TryGetKqpResourceManager(nodeId) == nullptr) {
                    Cerr << "... wait for RM on node " << nodeId << Endl;
                    return false;
                }
            }
            return true;
        };
        Runtime->DispatchEvents(rmReady);

        Runtime->SetLogPriority(NKikimrServices::KQP_RESOURCE_MANAGER, NActors::NLog::PRI_NOTICE);
//        Runtime->SetLogPriority(NKikimrServices::TX_DATASHARD, NLog::PRI_DEBUG);
//        Runtime->SetLogPriority(NKikimrServices::TX_PROXY, NLog::PRI_DEBUG);
        Runtime->SetLogPriority(NKikimrServices::KQP_EXECUTER, NActors::NLog::PRI_TRACE);

        auto sender = Runtime->AllocateEdgeActor();
        InitRoot(Server, sender);
        CreateShardedTable(Server, sender, "/Root", "table-1", 4);
        ExecSQL(Server, sender, "UPSERT INTO `/Root/table-1` (key, value) VALUES (1, 1), (2, 2), (3, 3)");

        Client = Runtime->AllocateEdgeActor();
    }

    Tests::TServer::TPtr Server;
    TTestActorRuntime* Runtime;
    TActorId Client;
};

Y_UNIT_TEST_SUITE(KqpErrors) {

Y_UNIT_TEST(ResolveTableError) {
    // Disable resource pool, because workload manager also got TEvNavigateKeySetResult for default pool creation
    TLocalFixture fixture(false);
    auto mitm = [&](TAutoPtr<IEventHandle> &ev) {
        if (ev->GetTypeRewrite() == TEvTxProxySchemeCache::TEvNavigateKeySetResult::EventType) {
            auto event = ev.Get()->Get<TEvTxProxySchemeCache::TEvNavigateKeySetResult>();
            event->Request->ErrorCount = 1;
            auto& entries = event->Request->ResultSet;
            entries[0].Status = NSchemeCache::TSchemeCacheNavigate::EStatus::LookupError;
        }
        return TTestActorRuntime::EEventAction::PROCESS;
    };
    fixture.Runtime->SetObserverFunc(mitm);

    SendRequest(*fixture.Runtime, fixture.Client, MakeSQLRequest("select * from `/Root/table-1`"));

    auto ev = fixture.Runtime->GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(fixture.Client);
    auto& record = ev->Get()->Record;

    // Cerr << record.DebugString() << Endl;

    UNIT_ASSERT_VALUES_EQUAL_C(record.GetYdbStatus(), Ydb::StatusIds::UNAVAILABLE, record.DebugString());

    TIssues issues;
    IssuesFromMessage(record.GetResponse().GetQueryIssues(), issues);
    UNIT_ASSERT(HasIssue(issues, NYql::TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE));
}

Y_UNIT_TEST(ProposeError) {
    TLocalFixture fixture(true, false);
    THashSet<TActorId> knownExecuters;

    using TMod = std::function<void(NKikimrTxDataShard::TEvProposeTransactionResult&)>;

    auto test = [&](auto proposeStatus, auto ydbStatus, auto issue, auto issueMessage, TMod mod = {}) {
        auto client = fixture.Runtime->AllocateEdgeActor();

        bool done = false;
        auto mitm = [&](TAutoPtr<IEventHandle> &ev) {
            if (!done && ev->GetTypeRewrite() == TEvDataShard::TEvProposeTransactionResult::EventType &&
                !knownExecuters.contains(ev->Recipient))
            {
                auto event = ev.Get()->Get<TEvDataShard::TEvProposeTransactionResult>();
                event->Record.SetStatus(proposeStatus);
                if (mod) {
                    mod(event->Record);
                }
                knownExecuters.insert(ev->Recipient);
                done = true;
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        fixture.Runtime->SetObserverFunc(mitm);

        SendRequest(*fixture.Runtime, client, MakeSQLRequest(Q_("upsert into `/Root/table-1` (key, value) values (5, 5);")));

        auto ev = fixture.Runtime->GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(client);
        auto& record = ev->Get()->Record;
        UNIT_ASSERT_VALUES_EQUAL_C(record.GetYdbStatus(), ydbStatus, record.DebugString());

        // Cerr << record.DebugString() << Endl;

        TIssues issues;
        IssuesFromMessage(record.GetResponse().GetQueryIssues(), issues);
        UNIT_ASSERT_C(HasIssue(issues, issue, issueMessage), "issue not found, issue: " << (int) issue
            << ", message: " << issueMessage << ", response: " << record.GetResponse().DebugString());
    };

    test(TEvProposeTransactionResult::OVERLOADED,                    // propose error
         Ydb::StatusIds::OVERLOADED,                                 // ydb status
         NYql::TIssuesIds::KIKIMR_OVERLOADED,                        // issue status
         "Kikimr cluster or one of its subsystems is overloaded.");  // main issue message (more detailed info can be in subissues)

    test(TEvProposeTransactionResult::ABORTED,
         Ydb::StatusIds::ABORTED,
         NYql::TIssuesIds::KIKIMR_OPERATION_ABORTED,
         "Operation aborted.");

    test(TEvProposeTransactionResult::TRY_LATER,
         Ydb::StatusIds::UNAVAILABLE,
         NYql::TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE,
         "Kikimr cluster or one of its subsystems was unavailable.");

    test(TEvProposeTransactionResult::RESULT_UNAVAILABLE,
         Ydb::StatusIds::UNDETERMINED,
         NYql::TIssuesIds::KIKIMR_RESULT_UNAVAILABLE,
         "Result of Kikimr query didn't meet requirements and isn't available");

    test(TEvProposeTransactionResult::CANCELLED,
         Ydb::StatusIds::CANCELLED,
         NYql::TIssuesIds::KIKIMR_OPERATION_CANCELLED,
         "Operation cancelled.");

    test(TEvProposeTransactionResult::BAD_REQUEST,
         Ydb::StatusIds::BAD_REQUEST,
         NYql::TIssuesIds::KIKIMR_BAD_REQUEST,
         "Bad request.");

    test(TEvProposeTransactionResult::ERROR,
         Ydb::StatusIds::UNAVAILABLE,
         NYql::TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE,
         "Kikimr cluster or one of its subsystems was unavailable.");

    test(TEvProposeTransactionResult::ERROR,
         Ydb::StatusIds::ABORTED,
         NYql::TIssuesIds::KIKIMR_SCHEME_MISMATCH,
         "blah-blah-blah",
         [](NKikimrTxDataShard::TEvProposeTransactionResult& x) {
             auto* error = x.MutableError()->Add();
             error->SetKind(NKikimrTxDataShard::TError::SCHEME_CHANGED);
             error->SetReason("blah-blah-blah");
         });

    test(TEvProposeTransactionResult::ERROR,
        Ydb::StatusIds::ABORTED,
        NYql::TIssuesIds::KIKIMR_SCHEME_MISMATCH,
        "blah-blah-blah",
        [](NKikimrTxDataShard::TEvProposeTransactionResult& x) {
            auto* error = x.MutableError()->Add();
            error->SetKind(NKikimrTxDataShard::TError::SCHEME_ERROR);
            error->SetReason("blah-blah-blah");
        });

    test(TEvProposeTransactionResult::EXEC_ERROR,
         Ydb::StatusIds::GENERIC_ERROR,
         NYql::TIssuesIds::DEFAULT_ERROR,
         "Error executing transaction (ExecError): Execution failed");

    test(TEvProposeTransactionResult::EXEC_ERROR,
             Ydb::StatusIds::PRECONDITION_FAILED,
             NYql::TIssuesIds::KIKIMR_PRECONDITION_FAILED,
             "Kikimr precondition failed",
             [](NKikimrTxDataShard::TEvProposeTransactionResult& x) {
                 auto* error = x.MutableError()->Add();
                 error->SetKind(NKikimrTxDataShard::TError::PROGRAM_ERROR);
                 error->SetReason("blah-blah-blah");
             });

    test(TEvProposeTransactionResult::RESPONSE_DATA,
            Ydb::StatusIds::GENERIC_ERROR,
            NYql::TIssuesIds::DEFAULT_ERROR,
            "Error executing transaction: transaction failed.");
}

Y_UNIT_TEST(ProposeErrorEvWrite) {
    TLocalFixture fixture(true, true);
    THashSet<TActorId> knownExecuters;

    using TMod = std::function<void(NKikimrDataEvents::TEvWriteResult&)>;

    auto test = [&](auto proposeStatus, auto ydbStatus, auto issue, auto issueMessage, TMod mod = {}) {
        auto client = fixture.Runtime->AllocateEdgeActor();

        bool done = false;
        auto mitm = [&](TAutoPtr<IEventHandle> &ev) {
            if (!done && ev->GetTypeRewrite() == NKikimr::NEvents::TDataEvents::TEvWriteResult::EventType &&
                !knownExecuters.contains(ev->Recipient))
            {
                auto event = ev.Get()->Get<NKikimr::NEvents::TDataEvents::TEvWriteResult>();
                event->Record.SetStatus(proposeStatus);
                if (mod) {
                    mod(event->Record);
                }
                knownExecuters.insert(ev->Recipient);
                done = true;
            }
            return TTestActorRuntime::EEventAction::PROCESS;
        };
        fixture.Runtime->SetObserverFunc(mitm);

        SendRequest(*fixture.Runtime, client, MakeSQLRequest(Q_("upsert into `/Root/table-1` (key, value) values (5, 5);")));

        auto ev = fixture.Runtime->GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(client);
        auto& record = ev->Get()->Record;
        UNIT_ASSERT_VALUES_EQUAL_C(record.GetYdbStatus(), ydbStatus, record.DebugString());

        // Cerr << record.DebugString() << Endl;

        TIssues issues;
        IssuesFromMessage(record.GetResponse().GetQueryIssues(), issues);
        UNIT_ASSERT_C(HasIssueContains(issues, issue, issueMessage), "issue not found, issue: " << (int) issue
            << ", message: " << issueMessage << ", response: " << record.GetResponse().DebugString());
    };

    test(NKikimrDataEvents::TEvWriteResult::STATUS_OVERLOADED,                    // propose error
         Ydb::StatusIds::OVERLOADED,                                 // ydb status
         NYql::TIssuesIds::KIKIMR_OVERLOADED,                        // issue status
         "Kikimr cluster or one of its subsystems is overloaded.");  // main issue message (more detailed info can be in subissues)

    test(NKikimrDataEvents::TEvWriteResult::STATUS_UNSPECIFIED,
         Ydb::StatusIds::STATUS_CODE_UNSPECIFIED,
         NYql::TIssuesIds::DEFAULT_ERROR,
         "Unspecified error.");

    test(NKikimrDataEvents::TEvWriteResult::STATUS_ABORTED,
         Ydb::StatusIds::ABORTED,
         NYql::TIssuesIds::KIKIMR_OPERATION_ABORTED,
         "Operation aborted.");

    test(NKikimrDataEvents::TEvWriteResult::STATUS_INTERNAL_ERROR,
         Ydb::StatusIds::INTERNAL_ERROR,
         NYql::TIssuesIds::KIKIMR_INTERNAL_ERROR,
         "Internal error while executing transaction.");

    test(NKikimrDataEvents::TEvWriteResult::STATUS_CANCELLED,
         Ydb::StatusIds::CANCELLED,
         NYql::TIssuesIds::KIKIMR_OPERATION_CANCELLED,
         "Operation cancelled.");

    test(NKikimrDataEvents::TEvWriteResult::STATUS_BAD_REQUEST,
         Ydb::StatusIds::BAD_REQUEST,
         NYql::TIssuesIds::KIKIMR_BAD_REQUEST,
         "Bad request.");

    test(NKikimrDataEvents::TEvWriteResult::STATUS_SCHEME_CHANGED,
         Ydb::StatusIds::ABORTED,
         NYql::TIssuesIds::KIKIMR_SCHEME_MISMATCH,
         "Scheme changed.");

    test(NKikimrDataEvents::TEvWriteResult::STATUS_LOCKS_BROKEN,
         Ydb::StatusIds::ABORTED,
         NYql::TIssuesIds::KIKIMR_LOCKS_INVALIDATED,
         "Transaction locks invalidated.");
    
    test(NKikimrDataEvents::TEvWriteResult::STATUS_DISK_SPACE_EXHAUSTED,
         Ydb::StatusIds::UNAVAILABLE,
         NYql::TIssuesIds::KIKIMR_DISK_SPACE_EXHAUSTED,
         "Disk space exhausted.");

    test(NKikimrDataEvents::TEvWriteResult::STATUS_WRONG_SHARD_STATE,
         Ydb::StatusIds::UNAVAILABLE,
         NYql::TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE,
         "Wrong shard state.");

    test(NKikimrDataEvents::TEvWriteResult::STATUS_CONSTRAINT_VIOLATION,
         Ydb::StatusIds::PRECONDITION_FAILED,
         NYql::TIssuesIds::KIKIMR_CONSTRAINT_VIOLATION,
         "Constraint violated.");
    
    test(NKikimrDataEvents::TEvWriteResult::STATUS_OUT_OF_SPACE,
         Ydb::StatusIds::OVERLOADED,
         NYql::TIssuesIds::KIKIMR_OVERLOADED,
         "out of space.");
}

void TestProposeResultLost(TTestActorRuntime& runtime, TActorId client, const TString& query,
                           std::function<void(const NKikimrKqp::TEvQueryResponse& resp)> fn)
{
    TActorId executer;
    ui32 droppedEvents = 0;

    auto prev = runtime.SetObserverFunc([&](TAutoPtr<IEventHandle>& ev) {
        if (ev->GetTypeRewrite() == TEvPipeCache::TEvForward::EventType) {
            auto* fe = ev.Get()->Get<TEvPipeCache::TEvForward>();
            if (fe->Ev->Type() == TEvDataShard::TEvProposeTransaction::EventType
                || fe->Ev->Type() == NKikimr::NEvents::TDataEvents::TEvWrite::EventType) {
                executer = ev->Sender;
                // Cerr << "-- executer: " << executer << Endl;
                return TTestActorRuntime::EEventAction::PROCESS;
            }
        }

        if (ev->GetTypeRewrite() == TEvDataShard::TEvProposeTransactionResult::EventType) {
            auto* msg = ev.Get()->Get<TEvDataShard::TEvProposeTransactionResult>();
            if (msg->Record.GetStatus() == NKikimrTxDataShard::TEvProposeTransactionResult::PREPARED) {
                if (ev->Sender.NodeId() == executer.NodeId()) {
                    ++droppedEvents;
                    // Cerr << "-- send undelivery to " << ev->Recipient << ", executer: " << executer << Endl;
                    runtime.Send(new IEventHandle(executer, ev->Sender,
                        new TEvPipeCache::TEvDeliveryProblem(msg->GetOrigin(), /* NotDelivered */ false)));
                    return TTestActorRuntime::EEventAction::DROP;
                }
            }
        }

        if (ev->GetTypeRewrite() == NKikimr::NEvents::TDataEvents::TEvWriteResult::EventType) {
            auto* msg = ev.Get()->Get<NKikimr::NEvents::TDataEvents::TEvWriteResult>();
            if (msg->Record.GetStatus() == NKikimrDataEvents::TEvWriteResult::STATUS_PREPARED) {
                if (ev->Sender.NodeId() == executer.NodeId()) {
                    ++droppedEvents;
                    // Cerr << "-- send undelivery to " << ev->Recipient << ", executer: " << executer << Endl;
                    runtime.Send(new IEventHandle(executer, ev->Sender,
                        new TEvPipeCache::TEvDeliveryProblem(msg->Record.GetOrigin(), /* NotDelivered */ false)));
                    return TTestActorRuntime::EEventAction::DROP;
                }
            }
        }

        return TTestActorRuntime::EEventAction::PROCESS;
    });
    SendRequest(runtime, client, MakeSQLRequest(query));

    auto ev = runtime.GrabEdgeEventRethrow<NKqp::TEvKqp::TEvQueryResponse>(client);
    UNIT_ASSERT(droppedEvents > 0 && droppedEvents < 4);

    auto& record = ev->Get()->Record;
    // Cerr << record.DebugString() << Endl;
    fn(record);

    runtime.SetObserverFunc(prev);
}

Y_UNIT_TEST_TWIN(ProposeResultLost_RwTx, UseSink) {
    TLocalFixture fixture(true, UseSink);
    TestProposeResultLost(*fixture.Runtime, fixture.Client,
        Q_(R"(
            upsert into `/Root/table-1` (key, value) VALUES
                (1, 11), (1073741823, 1073741823), (2147483647, 2147483647), (4294967295, 4294967295)
           )"),
        [](const NKikimrKqp::TEvQueryResponse& record) {
            UNIT_ASSERT_VALUES_EQUAL_C(record.GetYdbStatus(), Ydb::StatusIds::UNAVAILABLE, record.DebugString());

            TIssues issues;
            IssuesFromMessage(record.GetResponse().GetQueryIssues(), issues);
            UNIT_ASSERT_C(
                HasIssueContains(issues, NYql::TIssuesIds::KIKIMR_TEMPORARILY_UNAVAILABLE,
                    "Kikimr cluster or one of its subsystems was unavailable."),
                record.GetResponse().DebugString());
        });

    // Verify that the transaction didn't commit
    UNIT_ASSERT_VALUES_EQUAL(
        KqpSimpleExec(*fixture.Runtime,
            Q_("SELECT key, value FROM `/Root/table-1` ORDER BY key")),
        "{ items { uint32_value: 1 } items { uint32_value: 1 } }, "
        "{ items { uint32_value: 2 } items { uint32_value: 2 } }, "
        "{ items { uint32_value: 3 } items { uint32_value: 3 } }");
}

} // suite

} // namespace NKqp
} // namespace NKikimr
