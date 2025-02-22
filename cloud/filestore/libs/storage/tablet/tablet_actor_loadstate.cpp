#include "tablet_actor.h"

#include "helpers.h"

#include <cloud/filestore/libs/diagnostics/critical_events.h>

namespace NCloud::NFileStore::NStorage {

using namespace NActors;

using namespace NKikimr;
using namespace NKikimr::NTabletFlatExecutor;

namespace {

////////////////////////////////////////////////////////////////////////////////

NProto::TTabletStorageInfo SerializeTabletStorageInfo(const auto& info)
{
    NProto::TTabletStorageInfo proto;

    proto.SetTabletId(info.TabletID);
    proto.SetVersion(info.Version);

    for (const auto& srcChannel: info.Channels) {
        auto& dstChannel = *proto.MutableChannels()->Add();
        dstChannel.SetStoragePool(srcChannel.StoragePool);

        for (const auto& srcEntry: srcChannel.History) {
            auto& dstEntry = *dstChannel.MutableHistory()->Add();
            dstEntry.SetFromGeneration(srcEntry.FromGeneration);
            dstEntry.SetGroupId(srcEntry.GroupID);
        }
    }

    return proto;
}

NProto::TError ValidateTabletStorageInfoUpdate(
    const TString& LogTag,
    const NProto::TTabletStorageInfo& oldInfo,
    const NProto::TTabletStorageInfo& newInfo)
{
    const ui32 oldInfoVersion = oldInfo.GetVersion();
    const ui32 newInfoVersion = newInfo.GetVersion();

    if (oldInfoVersion > newInfoVersion) {
        return MakeError(E_FAIL, TStringBuilder()
            << "version mismatch (old: " << oldInfoVersion
            << ", new: " << newInfoVersion << ")");
    }

    if (oldInfoVersion == newInfoVersion) {
        google::protobuf::util::MessageDifferencer differencer;

        TString diff;
        differencer.ReportDifferencesToString(&diff);
        if (differencer.Compare(oldInfo, newInfo)) {
            return MakeError(S_ALREADY, "nothing to update");
        }

        return MakeError(E_FAIL, TStringBuilder()
            << "content has changed without version increment, diff: " << diff);
    }

    TABLET_VERIFY_C(oldInfoVersion < newInfoVersion,
        TStringBuilder() << "config version mismatch: old "
            << oldInfoVersion << " , new: " << newInfoVersion);

    const ui32 oldChannelCount = oldInfo.ChannelsSize();
    const ui32 newChannelCount = newInfo.ChannelsSize();;

    if (oldChannelCount > newChannelCount) {
        return MakeError(E_FAIL, TStringBuilder()
            << "channel count has been decreased (old: " << oldChannelCount
            << ", new: " << newChannelCount << ")");
    }

    return {};
}

}   // namespace

////////////////////////////////////////////////////////////////////////////////

bool TIndexTabletActor::PrepareTx_LoadState(
    const TActorContext& ctx,
    TTransactionContext& tx,
    TTxIndexTablet::TLoadState& args)
{
    LOG_INFO_S(ctx, TFileStoreComponents::TABLET,
        LogTag << " Loading tablet state data");

    TIndexTabletDatabase db(tx.DB);

    bool ready = true;

    if (!db.ReadFileSystem(args.FileSystem)) {
        ready = false;
    }

    if (!db.ReadFileSystemStats(args.FileSystemStats)) {
        ready = false;
    }

    if (!db.ReadTabletStorageInfo(args.TabletStorageInfo)) {
        ready = false;
    }

    if (!db.ReadNode(RootNodeId, 0, args.RootNode)) {
        ready = false;
    }

    if (!db.ReadSessions(args.Sessions)) {
        ready = false;
    }

    if (!db.ReadSessionHandles(args.Handles)) {
        ready = false;
    }

    if (!db.ReadSessionLocks(args.Locks)) {
        ready = false;
    }

    if (!db.ReadSessionDupCacheEntries(args.DupCache)) {
        ready = false;
    }

    if (!db.ReadFreshBytes(args.FreshBytes)) {
        ready = false;
    }

    if (!db.ReadFreshBlocks(args.FreshBlocks)) {
        ready = false;
    }

    if (!db.ReadNewBlobs(args.NewBlobs)) {
        ready = false;
    }

    if (!db.ReadGarbageBlobs(args.GarbageBlobs)) {
        ready = false;
    }

    if (!db.ReadCheckpoints(args.Checkpoints)) {
        ready = false;
    }

    if (!db.ReadCompactionMap(args.CompactionMap)) {
        ready = false;
    }

    if (!db.ReadTruncateQueue(args.TruncateQueue)) {
        ready = false;
    }

    if (!db.ReadStorageConfig(args.StorageConfig)) {
        ready = false;
    }

    if (!db.ReadSessionHistoryEntries(args.SessionsHistory)) {
        ready = false;
    }

    LOG_INFO_S(ctx, TFileStoreComponents::TABLET,
        LogTag << " Loading tablet state data "
            << (ready ? "finished" : "restarted"));

    return ready;
}

void TIndexTabletActor::ExecuteTx_LoadState(
    const TActorContext& ctx,
    TTransactionContext& tx,
    TTxIndexTablet::TLoadState& args)
{
    LOG_INFO_S(ctx, TFileStoreComponents::TABLET,
        LogTag << " Preparing tablet state");

    TIndexTabletDatabase db(tx.DB);

    if (!args.RootNode) {
        args.RootNode.ConstructInPlace();
        args.RootNode->Attrs = CreateDirectoryAttrs(0777, 0, 0);
        db.WriteNode(RootNodeId, 0, args.RootNode->Attrs);
    }

    const auto& oldTabletStorageInfo = args.TabletStorageInfo;
    const auto newTabletStorageInfo = SerializeTabletStorageInfo(*Info());

    if (!oldTabletStorageInfo.GetTabletId()) {
        // First TxLoadState on tablet creation
        LOG_INFO_S(ctx, TFileStoreComponents::TABLET,
            LogTag << " Initializing tablet storage info");

        TABLET_VERIFY(newTabletStorageInfo.GetTabletId());
        args.TabletStorageInfo.CopyFrom(newTabletStorageInfo);
        db.WriteTabletStorageInfo(newTabletStorageInfo);
        return;
    }

    const auto error = ValidateTabletStorageInfoUpdate(
        LogTag,
        oldTabletStorageInfo,
        newTabletStorageInfo);

    if (HasError(error)) {
        ReportInvalidTabletStorageInfo();
        args.Error = error;
        return;
    }

    if (error.GetCode() != S_ALREADY) {
        LOG_INFO_S(ctx, TFileStoreComponents::TABLET,
            LogTag << " Updating tablet storage info");

        args.TabletStorageInfo.CopyFrom(newTabletStorageInfo);
        db.WriteTabletStorageInfo(newTabletStorageInfo);
    }

    LOG_INFO_S(ctx, TFileStoreComponents::TABLET,
        LogTag << " Completed preparing tablet state");
}

void TIndexTabletActor::CompleteTx_LoadState(
    const TActorContext& ctx,
    TTxIndexTablet::TLoadState& args)
{
    if (args.StorageConfig.Defined()) {
        Config = std::make_shared<TStorageConfig>(*Config);
        Config->Merge(*args.StorageConfig.Get());
        LOG_INFO_S(ctx, TFileStoreComponents::TABLET,
            LogTag << " Merge StorageConfig with config from tablet database");
    }

    if (HasError(args.Error)) {
        LOG_ERROR_S(ctx, TFileStoreComponents::TABLET,
            LogTag
            << "Switching tablet to BROKEN state due to the failed TxLoadState: "
            << FormatError(args.Error));

        BecomeAux(ctx, STATE_BROKEN);

        // allow pipes to connect
        SignalTabletActive(ctx);

        // resend pending WaitReady requests
        while (WaitReadyRequests) {
            ctx.Send(WaitReadyRequests.front().release());
            WaitReadyRequests.pop_front();
        }

        return;
    }

    BecomeAux(ctx, STATE_WORK);
    LOG_INFO_S(ctx, TFileStoreComponents::TABLET,
        LogTag << " Activating tablet");

    // allow pipes to connect
    SignalTabletActive(ctx);

    // resend pending WaitReady requests
    while (WaitReadyRequests) {
        ctx.Send(WaitReadyRequests.front().release());
        WaitReadyRequests.pop_front();
    }

    TThrottlerConfig config;
    Convert(args.FileSystem.GetPerformanceProfile(), config);

    LOG_INFO_S(ctx, TFileStoreComponents::TABLET,
        LogTag << " Initializing tablet state");

    LoadState(
        Executor()->Generation(),
        *Config,
        args.FileSystem,
        args.FileSystemStats,
        args.TabletStorageInfo,
        config);
    UpdateLogTag();

    LOG_INFO_S(ctx, TFileStoreComponents::TABLET,
        LogTag << " Loading tablet sessions");
    auto idleSessionDeadline = ctx.Now() + Config->GetIdleSessionTimeout();
    LoadSessions(
        idleSessionDeadline,
        args.Sessions,
        args.Handles,
        args.Locks,
        args.DupCache,
        args.SessionsHistory);

    if (!Config->GetEnableCollectGarbageAtStart()) {
        SetStartupGcExecuted();
    }

    LOG_INFO_S(ctx, TFileStoreComponents::TABLET,
        LogTag << " Enqueueing truncate operations: "
            << args.TruncateQueue.size());
    for (const auto& entry: args.TruncateQueue) {
        EnqueueTruncateOp(
            entry.GetNodeId(),
            TByteRange(entry.GetOffset(), entry.GetLength(), GetBlockSize()));
    }

    // checkpoints should be loaded before data
    LOG_INFO_S(ctx, TFileStoreComponents::TABLET,
        LogTag << " Loading tablet checkpoints: "
            << args.Checkpoints.size());
    LoadCheckpoints(args.Checkpoints);

    LOG_INFO_S(ctx, TFileStoreComponents::TABLET,
        LogTag << " Loading fresh bytes: "
            << args.FreshBytes.size());
    LoadFreshBytes(args.FreshBytes);

    LOG_INFO_S(ctx, TFileStoreComponents::TABLET,
        LogTag << " Loading fresh blocks: "
            << args.FreshBlocks.size());
    LoadFreshBlocks(args.FreshBlocks);

    LOG_INFO_S(ctx, TFileStoreComponents::TABLET,
        LogTag << " Loading garbage blobs: "
            << args.GarbageBlobs.size());
    LoadGarbage(args.NewBlobs, args.GarbageBlobs);

    LOG_INFO_S(ctx, TFileStoreComponents::TABLET,
        LogTag << " Initializing compaction map: "
            << args.CompactionMap.size());
    LoadCompactionMap(args.CompactionMap);

    LOG_INFO_S(ctx, TFileStoreComponents::TABLET,
        LogTag << " Scheduling startup events");
    ScheduleCleanupSessions(ctx);
    RestartCheckpointDestruction(ctx);
    EnqueueFlushIfNeeded(ctx);
    EnqueueBlobIndexOpIfNeeded(ctx);
    EnqueueCollectGarbageIfNeeded(ctx);
    EnqueueTruncateIfNeeded(ctx);

    RegisterFileStore(ctx);
    RegisterStatCounters();
    ResetThrottlingPolicy();

    CompleteStateLoad();

    LOG_INFO_S(ctx, TFileStoreComponents::TABLET,
        LogTag << " Load state completed");
}

}   // namespace NCloud::NFileStore::NStorage
