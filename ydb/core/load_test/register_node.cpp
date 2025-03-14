#include "service_actor.h"

#include <ydb/core/base/appdata_fwd.h>
#include <ydb/core/base/counters.h>
#include <ydb/core/base/nameservice.h>
#include <ydb/core/config/init/init.h>

#include <library/cpp/monlib/service/pages/templates.h>
#include <library/cpp/histogram/hdr/histogram.h>
#include <library/cpp/time_provider/time_provider.h>

#include <util/generic/queue.h>
#include <util/random/fast.h>
#include <util/random/shuffle.h>
#include <util/system/hp_timer.h>

namespace NKikimr {

class TRegisterNodeLoadActor : public TActorBootstrapped<TRegisterNodeLoadActor> {
public:
    static constexpr auto ActorActivityType() {
        return NKikimrServices::TActivity::KQP_TEST_WORKLOAD;
    }

    TRegisterNodeLoadActor(const NKikimr::TEvLoadTestRequest::TRegisterNodeLoad& cmd, const TActorId& parent,
            const TIntrusivePtr<::NMonitoring::TDynamicCounters>& counters, ui64 index, ui64 tag)
        : Parent(parent)
        , Tag(tag)
        , LatencyHist(60000, 2)
        , Client(NConfig::MakeDefaultNodeBrokerClient())
        , Env(NConfig::MakeDefaultEnv())
        , Logger(NConfig::MakeNoopInitLogger())
    {
        Y_UNUSED(index);
        VERIFY_PARAM(DurationSeconds);

        google::protobuf::TextFormat::PrintToString(cmd, &ConfigString);

        DurationSeconds = cmd.GetDurationSeconds();
        Offset = cmd.GetIcPortOffset();

        Y_ASSERT(DurationSeconds > DelayBeforeMeasurements.Seconds());

        GrpcSettings.PathToGrpcCaFile = AppData()->GrpcConfig.GetPathToCaFile();
        GrpcSettings.PathToGrpcCertFile = AppData()->GrpcConfig.GetPathToCertificateFile();
        GrpcSettings.PathToGrpcPrivateKeyFile = AppData()->GrpcConfig.GetPathToPrivateKeyFile();

        NodeBrokerAddrs = {
            "vla5-2583.search.yandex.net:2135",
            "vla5-2585.search.yandex.net:2135",
            "vla5-2594.search.yandex.net:2135",
            "vla5-2584.search.yandex.net:2135",
            "vla5-2586.search.yandex.net:2135",
            "vla5-2592.search.yandex.net:2135"
        };

        Settings = {
            AppData()->DomainsConfig.GetDomain(0).GetName(),
            Env->FQDNHostName(),
            "",
            "",
            AppData()->TenantName,
            false,
            0,
            {},
            "root@builtin",
        };

        // Monitoring initialization

        LoadCounters = counters->GetSubgroup("tag", Sprintf("%" PRIu64, tag));
        Registrations = LoadCounters->GetCounter("Registrations", true);
        LatenciesMs = LoadCounters->GetHistogram("LatenciesMs",
            NMonitoring::ExponentialHistogram(20, 2, 1));
    }

    ~TRegisterNodeLoadActor() {
        LoadCounters->ResetCounters();
    }

    void Bootstrap(const TActorContext& ctx) {
        LOG_DEBUG_S(ctx, NKikimrServices::KQP_LOAD_TEST, "Tag# " << Tag << " TRegisterNodeLoadActor Bootstrap called");

        Become(&TRegisterNodeLoadActor::StateMain);

        LOG_INFO_S(ctx, NKikimrServices::KQP_LOAD_TEST, "Tag# " << Tag << " Schedule PoisonPill");
        EarlyStop = false;
        ctx.Schedule(TDuration::Seconds(DurationSeconds), new TEvents::TEvPoisonPill);
        TestStartTime = TAppData::TimeProvider->Now();
        SendRegistrationQuery();
    }

    STRICT_STFUNC(StateMain,
        CFunc(TEvents::TSystem::PoisonPill, HandlePoisonPill)
        cFunc(TEvents::TSystem::Wakeup, HandleWakeup)
        HFunc(NMon::TEvHttpInfo, HandleHTML)
    )

    void SendRegistrationQuery() {
        ShuffleRange(NodeBrokerAddrs);
        auto settings = Settings;
        settings.NodeHost = Settings.NodeHost + ToString(Offset);
        Offset++;
        settings.InterconnectPort = SelfId().NodeId();
        
        Registrations->Inc();
        THPTimer timer;
        auto result = Client->RegisterDynamicNode(GrpcSettings, NodeBrokerAddrs, settings, *Env, *Logger);
        // TODO(pixcc): no Apply that can be costly
        TDuration passed = TDuration::Seconds(timer.Passed());
        LatencyHist.RecordValue(passed.MicroSeconds());
        LatenciesMs->Collect(passed.MilliSeconds());
        Send(SelfId(), new TEvents::TEvWakeup);
    }

    void HandleWakeup() {
        SendRegistrationQuery();
    }

private:

    // death

    void HandlePoisonPill(const TActorContext& ctx) {
        EarlyStop = (TAppData::TimeProvider->Now() - TestStartTime).Seconds() < DurationSeconds;
        LOG_CRIT_S(ctx, NKikimrServices::KQP_LOAD_TEST, "Tag# " << Tag << " HandlePoisonPill, "
            << "but it is supposed to pass away by receiving TEvKqpWorkerResponse from all of the workers");
        StartDeathProcess(ctx);
    }

    void StartDeathProcess(const TActorContext& ctx) {
        LOG_NOTICE_S(ctx, NKikimrServices::KQP_LOAD_TEST, "Tag# " << Tag << " TRegisterNodeLoadActor StartDeathProcess called");
        DeathReport(ctx);
    }

    void DeathReport(const TActorContext& ctx) {
        TIntrusivePtr<TEvLoad::TLoadReport> report = nullptr;
        TString errorReason;
 
        if (EarlyStop) {
            errorReason = "Abort, stop signal received";
        } else {
            errorReason = "OK, called StartDeathProcess";
            report.Reset(new TEvLoad::TLoadReport());
            report->Duration = TDuration::Seconds(DurationSeconds);
        }

        auto* finishEv = new TEvLoad::TEvLoadTestFinished(Tag, report, errorReason);
        finishEv->LastHtmlPage = RenderHTML();
        finishEv->JsonResult = GetJsonResult();
        ctx.Send(Parent, finishEv);
        LOG_NOTICE_S(ctx, NKikimrServices::KQP_LOAD_TEST, "Tag# " << Tag << " DeathReport");
        PassAway();
    }

private:

    NJson::TJsonValue GetJsonResult() const {
        NJson::TJsonValue value;
        value["duration_s"] = DurationSeconds;
        value["txs"] = LatencyHist.GetTotalCount();
        value["rps"] = LatencyHist.GetTotalCount() / static_cast<double>(DurationSeconds);
        value["errors"] = 0;
        {
            auto& p = value["percentile"];
            p["50"] = LatencyHist.GetValueAtPercentile(50.0) / 1000.0;
            p["95"] = LatencyHist.GetValueAtPercentile(95.0) / 1000.0;
            p["99"] = LatencyHist.GetValueAtPercentile(99.0) / 1000.0;
            p["100"] = LatencyHist.GetMax() / 1000.0;
        }
        value["config"] = ConfigString;
        return value;
    }

    // monitoring
    TString RenderHTML() {
        TStringStream str;
        HTML(str) {
            if (Error) {
                DIV() {
                    str << "ERROR: " << Error;
                }
            }
            TABLE_CLASS("table table-condensed") {
                TABLEHEAD() {
                    TABLER() {
                        TABLEH() {
                            str << "Passed/Total, sec";
                        }
                        TABLEH() {
                            str << "Regs";
                        }
                        TABLEH() {
                            str << "Regs/Sec";
                        }
                        TABLEH() {
                            str << "Errors";
                        }
                        TABLEH() {
                            str << "p50(ms)";
                        }
                        TABLEH() {
                            str << "p95(ms)";
                        }
                        TABLEH() {
                            str << "p99(ms)";
                        }
                        TABLEH() {
                            str << "pMax(ms)";
                        }
                    }
                }
                TABLEBODY() {
                    TABLER() {
                        TABLED() {
                            if (TestStartTime) {
                                str << (TAppData::TimeProvider->Now() - TestStartTime).Seconds() << " / " << DurationSeconds;
                            } else {
                                str << -1 << " / " << DurationSeconds;
                            }
                        };
                        TABLED() { str << LatencyHist.GetTotalCount(); };
                        TABLED() { str << LatencyHist.GetTotalCount() / static_cast<double>(DurationSeconds); };
                        TABLED() { str << 0; }; // errors
                        TABLED() { str << LatencyHist.GetValueAtPercentile(50.0) / 1000.0; };
                        TABLED() { str << LatencyHist.GetValueAtPercentile(95.0) / 1000.0; };
                        TABLED() { str << LatencyHist.GetValueAtPercentile(99.0) / 1000.0; };
                        TABLED() { str << LatencyHist.GetMax() / 1000.0; };
                    }
                }
            }
            COLLAPSED_BUTTON_CONTENT(Sprintf("configProtobuf%" PRIu64, Tag), "Config") {
                str << "<pre>" << ConfigString << "</pre>";
            }
        }
        return str.Str();
    }

    void HandleHTML(NMon::TEvHttpInfo::TPtr& ev, const TActorContext& ctx) {
        ctx.Send(ev->Sender, new NMon::TEvHttpInfoRes(RenderHTML(), ev->Get()->SubRequestId));
    }

    // common
    TInstant TestStartTime;
    bool EarlyStop = false;
    TString ConfigString;
    TString WorkloadClass;

    const TActorId Parent;
    ui64 Tag;
    ui32 DurationSeconds;
    ui64 Offset;

    // Monitoring
    TString Error;
    TIntrusivePtr<::NMonitoring::TDynamicCounters> LoadCounters;
    NMonitoring::TDynamicCounters::TCounterPtr Registrations;
    NMonitoring::THistogramPtr LatenciesMs;
    NHdr::THistogram LatencyHist;

    std::unique_ptr<NConfig::INodeBrokerClient> Client;
    std::unique_ptr<NConfig::IEnv> Env;
    std::unique_ptr<NConfig::IInitLogger> Logger;

    NConfig::TNodeRegistrationSettings Settings;
    NConfig::TGrpcSslSettings GrpcSettings;
    TVector<TString> NodeBrokerAddrs;
};

IActor * CreateRegisterNodeLoadActor(const NKikimr::TEvLoadTestRequest::TRegisterNodeLoad& cmd,
        const TActorId& parent, const TIntrusivePtr<::NMonitoring::TDynamicCounters>& counters, ui64 index, ui64 tag) {
    return new TRegisterNodeLoadActor(cmd, parent, counters, index, tag);
}

}
