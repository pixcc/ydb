#include "node_broker_impl.h"
#include "node_broker__scheme.h"

#include <util/random/random.h>
#include <ydb/core/protos/counters_node_broker.pb.h>

namespace NKikimr {
namespace NNodeBroker {

class TNodeBroker::TTxLoadInsert : public TTransactionBase<TNodeBroker> {
public:
    TTxLoadInsert(TNodeBroker *self)
        : TBase(self)
    {
    }

    TTxType GetTxType() const override { return TXTYPE_LOAD_INSERT; }

    bool Execute(TTransactionContext &txc, const TActorContext &ctx) override
    {
        LOG_DEBUG(ctx, NKikimrServices::NODE_BROKER, "TTxLoadInsert Execute");

        if (!Self->LoadGenerator) {
            return true;
        }

        for (size_t i = 0; i < Self->LoadGenerator->BatchSize; ++i) {
            // Generate synthetic node ID
            ui32 nodeId = Self->Dirty.FreeIds.FirstNonZeroBit();
            Self->Dirty.FreeIds.Reset(nodeId);

            // Generate synthetic data
            TString host = TStringBuilder() << "vla5-2583.search.yandex.net-" << nodeId;
            TString address = "2a02:6b8:bf00:300a:eaeb:d3ff:fee7:a6c2";
            ui16 port = 19001;
            TNodeLocation location("klg", "", "klg-06:6a13", "107963821");

            TNodeInfo node(nodeId, address, host, host, port, location);
            node.Lease = 1;
            node.Expire = TInstant::Max();
            node.Version = Self->Dirty.Epoch.Version + 1;
            node.State = ENodeState::Active;
            node.AuthorizedByCertificate = false;

            Self->Dirty.DbAddNode(node, txc);

            Self->LoadGenerator->ExecutedOps++;
        }

        return true;
    }

    void Complete(const TActorContext &ctx) override
    {
        LOG_DEBUG(ctx, NKikimrServices::NODE_BROKER, "TTxLoadInsert Complete");
        Y_UNUSED(ctx);
    }
};

class TNodeBroker::TTxLoadUpdate : public TTransactionBase<TNodeBroker> {
public:
    TTxLoadUpdate(TNodeBroker *self)
        : TBase(self)
    {
    }

    TTxType GetTxType() const override { return TXTYPE_LOAD_UPDATE; }

    bool Execute(TTransactionContext &txc, const TActorContext &) override
    {
        if (!Self->LoadGenerator) {
            return true;
        }

        for (size_t i = 0; i < Self->LoadGenerator->BatchSize; ++i) {
            // Generate synthetic node ID
            ui32 nodeId = Self->MinDynamicId + 10 + RandomNumber<ui32>(Self->Dirty.FreeIds.FirstNonZeroBit() - Self->MinDynamicId - 10);

            // Generate synthetic data
            TString host = TStringBuilder() << "vla5-2583.search.yandex.net-" << nodeId;
            TString address = "2a02:6b8:bf00:300a:eaeb:d3ff:fee7:a6c2";
            ui16 port = 19001;
            TNodeLocation location("klg", "", "klg-06:6a13", "107963821");

            TNodeInfo node(nodeId, address, host, host, port, location);
            node.Lease = 1;
            node.Expire = TInstant::Max();
            node.Version = Self->Dirty.Epoch.Version + 1;
            node.State = ENodeState::Active;
            node.AuthorizedByCertificate = false;

            Self->Dirty.DbAddNode(node, txc);

            Self->LoadGenerator->ExecutedOps++;
        }

        return true;
    }

    void Complete(const TActorContext &ctx) override
    {
        LOG_DEBUG(ctx, NKikimrServices::NODE_BROKER, "TTxLoadUpdate Complete");
        Y_UNUSED(ctx);
    }
};

ITransaction *TNodeBroker::CreateTxLoadInsert()
{
    return new TTxLoadInsert(this);
}

ITransaction *TNodeBroker::CreateTxLoadUpdate()
{
    return new TTxLoadUpdate(this);
}

} // NNodeBroker
} // NKikimr
