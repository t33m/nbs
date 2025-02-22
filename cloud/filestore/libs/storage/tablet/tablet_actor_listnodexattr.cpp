#include "tablet_actor.h"

namespace NCloud::NFileStore::NStorage {

using namespace NActors;

using namespace NKikimr;
using namespace NKikimr::NTabletFlatExecutor;

namespace {

////////////////////////////////////////////////////////////////////////////////

NProto::TError ValidateRequest(const NProto::TListNodeXAttrRequest& request)
{
    if (request.GetNodeId() == InvalidNodeId) {
        return ErrorInvalidArgument();
    }

    return {};
}

}   // namespace

////////////////////////////////////////////////////////////////////////////////

void TIndexTabletActor::HandleListNodeXAttr(
    const TEvService::TEvListNodeXAttrRequest::TPtr& ev,
    const TActorContext& ctx)
{
    if (!AcceptRequest<TEvService::TListNodeXAttrMethod>(ev, ctx, ValidateRequest)) {
        return;
    }

    auto* msg = ev->Get();
    auto requestInfo = CreateRequestInfo(
        ev->Sender,
        ev->Cookie,
        msg->CallContext);

    ExecuteTx<TListNodeXAttr>(
        ctx,
        std::move(requestInfo),
        msg->Record);
}

////////////////////////////////////////////////////////////////////////////////

bool TIndexTabletActor::PrepareTx_ListNodeXAttr(
    const TActorContext& ctx,
    TTransactionContext& tx,
    TTxIndexTablet::TListNodeXAttr& args)
{
    Y_UNUSED(ctx);

    auto* session = FindSession(
        args.ClientId,
        args.SessionId,
        args.SessionSeqNo);
    if (!session) {
        args.Error = ErrorInvalidSession(
            args.ClientId,
            args.SessionId,
            args.SessionSeqNo);
        return true;
    }

    args.CommitId = GetReadCommitId(session->GetCheckpointId());
    if (args.CommitId == InvalidCommitId) {
        args.Error = ErrorInvalidCheckpoint(session->GetCheckpointId());
        return true;
    }

    TIndexTabletDatabase db(tx.DB);

    // validate target node exists
    if (!ReadNode(db, args.NodeId, args.CommitId, args.Node)) {
        return false;   // not ready
    }

    if (!args.Node) {
        args.Error = ErrorInvalidTarget(args.NodeId);
        return true;
    }

    // TODO: AccessCheck
    TABLET_VERIFY(args.Node);
    if (!ReadNodeAttrs(db, args.NodeId, args.CommitId, args.Attrs)) {
        return false;   // not ready
    }

    return true;
}

void TIndexTabletActor::ExecuteTx_ListNodeXAttr(
    const TActorContext& ctx,
    TTransactionContext& tx,
    TTxIndexTablet::TListNodeXAttr& args)
{
    Y_UNUSED(ctx);
    Y_UNUSED(tx);
    Y_UNUSED(args);
}

void TIndexTabletActor::CompleteTx_ListNodeXAttr(
    const TActorContext& ctx,
    TTxIndexTablet::TListNodeXAttr& args)
{
    auto response = std::make_unique<TEvService::TEvListNodeXAttrResponse>(args.Error);
    if (SUCCEEDED(args.Error.GetCode())) {
        response->Record.MutableNames()->Reserve(args.Attrs.size());
        for (const auto& attr: args.Attrs) {
            response->Record.AddNames(attr.Name);
        }
    }

    CompleteResponse<TEvService::TListNodeXAttrMethod>(
        response->Record,
        args.RequestInfo->CallContext,
        ctx);

    NCloud::Reply(ctx, *args.RequestInfo, std::move(response));
}

}   // namespace NCloud::NFileStore::NStorage
