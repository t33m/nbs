#include "tablet_actor.h"

#include <cloud/filestore/libs/diagnostics/metrics/label.h>
#include <cloud/filestore/libs/diagnostics/metrics/operations.h>
#include <cloud/filestore/libs/diagnostics/metrics/registry.h>
#include <cloud/filestore/libs/service/request.h>

namespace NCloud::NFileStore::NStorage {

using namespace NActors;
using namespace NKikimr;
using namespace NMetrics;

namespace {

////////////////////////////////////////////////////////////////////////////////

void RegisterSensor(
    IMetricsRegistryPtr registry,
    TString name,
    const std::atomic<i64>& source,
    EAggregationType aggrType,
    EMetricType metrType)
{
    registry->Register(
        {CreateSensor(std::move(name))},
        source,
        aggrType,
        metrType);
}

}   // namespace

////////////////////////////////////////////////////////////////////////////////

TIndexTabletActor::TMetrics::TMetrics(IMetricsRegistryPtr metricsRegistry)
    : StorageRegistry(CreateScopedMetricsRegistry(
        {CreateLabel("component", "storage")},
        metricsRegistry))
    , StorageFsRegistry(CreateScopedMetricsRegistry(
        {CreateLabel("component", "storage_fs"), CreateLabel("host", "cluster")},
        metricsRegistry))
    , FsRegistry(CreateMetricsRegistryStub())
    , AggregatableFsRegistry(CreateMetricsRegistryStub())
{}

void TIndexTabletActor::TMetrics::Register(
    const TString& fsId,
    const TString& mediaKind)
{
    if (Initialized) {
        return;
    }

    auto totalKindRegistry = CreateScopedMetricsRegistry(
        {CreateLabel("type", mediaKind)},
        StorageRegistry);

    FsRegistry = CreateScopedMetricsRegistry(
        {CreateLabel("filesystem", fsId)},
        StorageFsRegistry);
    AggregatableFsRegistry = CreateScopedMetricsRegistry(
        {},
        {std::move(totalKindRegistry), FsRegistry});

#define REGISTER(registry, name, aggrType, metrType)                           \
    RegisterSensor(registry, #name, name, aggrType, metrType)                  \
// REGISTER

#define REGISTER_AGGREGATABLE_SUM(name, metrType)                              \
    REGISTER(                                                                  \
        AggregatableFsRegistry,                                                \
        name,                                                                  \
        EAggregationType::AT_SUM,                                              \
        metrType)                                                              \
// REGISTER_AGGREGATABLE_SUM

#define REGISTER_LOCAL(name, metrType)                                         \
    REGISTER(                                                                  \
        FsRegistry,                                                            \
        name,                                                                  \
        EAggregationType::AT_SUM,                                              \
        metrType)                                                              \
// REGISTER_LOCAL

    REGISTER_AGGREGATABLE_SUM(TotalBytesCount, EMetricType::MT_ABSOLUTE);
    REGISTER_AGGREGATABLE_SUM(UsedBytesCount, EMetricType::MT_ABSOLUTE);

    REGISTER_AGGREGATABLE_SUM(TotalNodesCount, EMetricType::MT_ABSOLUTE);
    REGISTER_AGGREGATABLE_SUM(UsedNodesCount, EMetricType::MT_ABSOLUTE);

    REGISTER_AGGREGATABLE_SUM(UsedSessionsCount, EMetricType::MT_ABSOLUTE);
    REGISTER_AGGREGATABLE_SUM(UsedHandlesCount, EMetricType::MT_ABSOLUTE);
    REGISTER_AGGREGATABLE_SUM(UsedLocksCount, EMetricType::MT_ABSOLUTE);

    REGISTER_AGGREGATABLE_SUM(AllocatedCompactionRangesCount, EMetricType::MT_ABSOLUTE);
    REGISTER_AGGREGATABLE_SUM(UsedCompactionRangesCount, EMetricType::MT_ABSOLUTE);

    REGISTER_AGGREGATABLE_SUM(MixedBytesCount, EMetricType::MT_ABSOLUTE);
    REGISTER_AGGREGATABLE_SUM(FreshBytesCount, EMetricType::MT_ABSOLUTE);
    REGISTER_AGGREGATABLE_SUM(GarbageQueueSize, EMetricType::MT_ABSOLUTE);

    REGISTER_AGGREGATABLE_SUM(IdleTime, EMetricType::MT_DERIVATIVE);
    REGISTER_AGGREGATABLE_SUM(BusyTime, EMetricType::MT_DERIVATIVE);

    REGISTER_AGGREGATABLE_SUM(AllocatedCompactionRangesCount, EMetricType::MT_ABSOLUTE);
    REGISTER_AGGREGATABLE_SUM(UsedCompactionRangesCount, EMetricType::MT_ABSOLUTE);

    // Throttling
    REGISTER_LOCAL(MaxReadBandwidth, EMetricType::MT_ABSOLUTE);
    REGISTER_LOCAL(MaxWriteBandwidth, EMetricType::MT_ABSOLUTE);
    REGISTER_LOCAL(MaxReadIops, EMetricType::MT_ABSOLUTE);
    REGISTER_LOCAL(MaxWriteIops, EMetricType::MT_ABSOLUTE);
    REGISTER_LOCAL(RejectedRequests, EMetricType::MT_DERIVATIVE);
    REGISTER_LOCAL(PostponedRequests, EMetricType::MT_DERIVATIVE);
    REGISTER_LOCAL(UsedQuota, EMetricType::MT_DERIVATIVE);
    MaxUsedQuota.Register(
        FsRegistry,
        {CreateSensor("MaxUsedQuota")},
        EAggregationType::AT_MAX);
    ReadDataPostponed.Register(
        FsRegistry,
        {CreateLabel("request", "ReadData"), CreateLabel("histogram", "ThrottlerDelay")});
    WriteDataPostponed.Register(
        FsRegistry,
        {CreateLabel("request", "WriteData"), CreateLabel("histogram", "ThrottlerDelay")});

#undef REGISTER_LOCAL
#undef REGISTER_AGGREGATABLE_SUM
#undef REGISTER

    BusyIdleCalc.Register(&BusyTime, &IdleTime);

    Initialized = true;
}

void TIndexTabletActor::TMetrics::Update(
    const NProto::TFileSystem& fileSystem,
    const NProto::TFileSystemStats& stats,
    const NProto::TFileStorePerformanceProfile& performanceProfile,
    const TCompactionMapStats& compactionStats)
{
    const ui32 blockSize = fileSystem.GetBlockSize();

    Store(TotalBytesCount, fileSystem.GetBlocksCount() * blockSize);
    Store(UsedBytesCount, stats.GetUsedBlocksCount() * blockSize);

    Store(TotalNodesCount, fileSystem.GetNodesCount());
    Store(UsedNodesCount, stats.GetUsedNodesCount());

    Store(UsedSessionsCount, stats.GetUsedSessionsCount());
    Store(UsedHandlesCount, stats.GetUsedHandlesCount());
    Store(UsedLocksCount, stats.GetUsedLocksCount());

    Store(MixedBytesCount, stats.GetMixedBlocksCount() * blockSize);
    Store(FreshBytesCount, stats.GetFreshBytesCount());
    Store(GarbageQueueSize, stats.GetGarbageQueueSize());

    Store(MaxReadIops, performanceProfile.GetMaxReadIops());
    Store(MaxWriteIops, performanceProfile.GetMaxWriteIops());
    Store(MaxReadBandwidth, performanceProfile.GetMaxReadBandwidth());
    Store(MaxWriteBandwidth, performanceProfile.GetMaxWriteBandwidth());

    Store(AllocatedCompactionRangesCount, compactionStats.AllocatedRangesCount);
    Store(UsedCompactionRangesCount, compactionStats.UsedRangesCount);

    BusyIdleCalc.OnUpdateStats();
}

////////////////////////////////////////////////////////////////////////////////

void TIndexTabletActor::RegisterCounters(const TActorContext& ctx)
{
    if (!Counters) {
        auto counters = CreateIndexTabletCounters();

        // LAME: ownership transferred to executor
        Counters = counters.release();
        Executor()->RegisterExternalTabletCounters(Counters);

        // only aggregated statistics will be reported by default
        // (you can always turn on per-tablet statistics on monitoring page)
        // TabletCountersAddTablet(TabletID(), ctx);

        ScheduleUpdateCounters(ctx);
    }
}

void TIndexTabletActor::RegisterStatCounters()
{
    const auto& fsId = GetFileSystemId();
    if (!fsId) {
        // it's possible to have empty id for newly created volume
        // just wait for the config update
        return;
    }

    const auto& fs = GetFileSystem();
    const auto storageMediaKind = GetStorageMediaKind(fs);
    TABLET_VERIFY(!storageMediaKind.empty());

    // Update should be called before Register, because we want to write
    // correct values to solomon. If we reorder this two actions, we can
    // aggregate zero values, in the middle of the registration (or right after
    // registration, before update).
    Metrics.Update(
        fs,
        GetFileSystemStats(),
        GetPerformanceProfile(),
        GetCompactionMapStats(0));

    Metrics.Register(fsId, storageMediaKind);
}

void TIndexTabletActor::ScheduleUpdateCounters(const TActorContext& ctx)
{
    if (!UpdateCountersScheduled) {
        ctx.Schedule(UpdateCountersInterval,
            new TEvIndexTabletPrivate::TEvUpdateCounters());
        UpdateCountersScheduled = true;
    }

    if (!UpdateLeakyBucketCountersScheduled) {
        ctx.Schedule(UpdateLeakyBucketCountersInterval,
            new TEvIndexTabletPrivate::TEvUpdateLeakyBucketCounters());
        UpdateLeakyBucketCountersScheduled = true;
    }
}

void TIndexTabletActor::HandleUpdateCounters(
    const TEvIndexTabletPrivate::TEvUpdateCounters::TPtr& ev,
    const TActorContext& ctx)
{
    Y_UNUSED(ev);

    UpdateCounters();
    Metrics.Update(
        GetFileSystem(),
        GetFileSystemStats(),
        GetPerformanceProfile(),
        GetCompactionMapStats(0));

    UpdateCountersScheduled = false;
    ScheduleUpdateCounters(ctx);
}

void TIndexTabletActor::UpdateCounters()
{
#define FILESTORE_TABLET_UPDATE_COUNTER(name, ...)                             \
    {                                                                          \
        auto& counter = Counters->Simple()[                                    \
            TIndexTabletCounters::SIMPLE_COUNTER_Stats_##name];                \
        counter.Set(Get##name());                                              \
    }                                                                          \
// FILESTORE_TABLET_UPDATE_COUNTER

    FILESTORE_TABLET_STATS(FILESTORE_TABLET_UPDATE_COUNTER)

#undef FILESTORE_TABLET_UPDATE_COUNTER
}

////////////////////////////////////////////////////////////////////////////////

void TIndexTabletActor::HandleGetStorageStats(
    const TEvIndexTablet::TEvGetStorageStatsRequest::TPtr& ev,
    const TActorContext& ctx)
{
    auto response = std::make_unique<TEvIndexTablet::TEvGetStorageStatsResponse>();

    auto* stats = response->Record.MutableStats();

#define FILESTORE_TABLET_UPDATE_COUNTER(name, ...)                             \
    stats->Set##name(Get##name());                                             \
// FILESTORE_TABLET_UPDATE_COUNTER

    FILESTORE_TABLET_STATS(FILESTORE_TABLET_UPDATE_COUNTER)

#undef FILESTORE_TABLET_UPDATE_COUNTER

    stats->SetTabletChannelCount(GetTabletChannelCount());
    stats->SetConfigChannelCount(GetConfigChannelCount());

    const auto txDeleteGarbageRwCompleted = Counters->TxCumulative(
        TIndexTabletCounters::ETransactionType::TX_DeleteGarbage,
        NKikimr::COUNTER_TT_RW_COMPLETED
    ).Get();
    stats->SetTxDeleteGarbageRwCompleted(txDeleteGarbageRwCompleted);

    response->Record.SetMediaKind(GetFileSystem().GetStorageMediaKind());

    auto cmStats = GetCompactionMapStats(0);
    stats->SetUsedCompactionRanges(cmStats.UsedRangesCount);
    stats->SetAllocatedCompactionRanges(cmStats.AllocatedRangesCount);

    NCloud::Reply(ctx, *ev, std::move(response));
}

}   // namespace NCloud::NFileStore::NStorage
