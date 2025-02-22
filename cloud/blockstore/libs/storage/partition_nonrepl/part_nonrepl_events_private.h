#pragma once

#include "public.h"

#include <cloud/blockstore/libs/diagnostics/profile_log.h>
#include <cloud/blockstore/libs/kikimr/components.h>
#include <cloud/blockstore/libs/kikimr/events.h>
#include <cloud/blockstore/libs/storage/protos/disk.pb.h>
#include <cloud/blockstore/libs/storage/protos/part.pb.h>

#include <library/cpp/containers/stack_vector/stack_vec.h>

#include <util/datetime/base.h>
#include <util/generic/vector.h>

namespace NCloud::NBlockStore::NProto {

    using TChecksumBlocksRequest = NProto::TChecksumDeviceBlocksRequest;
    using TChecksumBlocksResponse = NProto::TChecksumDeviceBlocksResponse;
}   // namespace NCloud::NBlockStore::NProto

namespace NCloud::NBlockStore::NStorage {

////////////////////////////////////////////////////////////////////////////////

#define BLOCKSTORE_PARTITION_NONREPL_REQUESTS_PRIVATE(xxx, ...)             \
    xxx(ChecksumBlocks, __VA_ARGS__)                                        \
// BLOCKSTORE_PARTITION_NONREPL_REQUESTS_PRIVATE

////////////////////////////////////////////////////////////////////////////////

struct TEvNonreplPartitionPrivate
{
    //
    // RangeMigrated
    //

    struct TRangeMigrated
    {
        TBlockRange64 Range;
        TInstant ReadStartTs;
        TDuration ReadDuration;
        TInstant WriteStartTs;
        TDuration WriteDuration;
        TVector<IProfileLog::TBlockInfo> AffectedBlockInfos;

        TRangeMigrated(
                TBlockRange64 range,
                TInstant readStartTs,
                TDuration readDuration,
                TInstant writeStartTs,
                TDuration writeDuration,
                TVector<IProfileLog::TBlockInfo> affectedBlockInfos)
            : Range(std::move(range))
            , ReadStartTs(readStartTs)
            , ReadDuration(readDuration)
            , WriteStartTs(writeStartTs)
            , WriteDuration(writeDuration)
            , AffectedBlockInfos(std::move(affectedBlockInfos))
        {
        }
    };

    //
    // MigrateNextRange
    //

    struct TMigrateNextRange
    {
    };

    //
    // WriteOrZeroCompleted
    //

    struct TWriteOrZeroCompleted
    {
        ui64 RequestCounter;

        TWriteOrZeroCompleted(ui64 requestCounter)
            : RequestCounter(requestCounter)
        {
        }
    };

    //
    // ResyncNextRange
    //

    struct TResyncNextRange
    {
    };

    //
    // RangeResynced
    //

    struct TRangeResynced
    {
        TBlockRange64 Range;
        TInstant ChecksumStartTs;
        TDuration ChecksumDuration;
        TInstant ReadStartTs;
        TDuration ReadDuration;
        TInstant WriteStartTs;
        TDuration WriteDuration;
        TVector<IProfileLog::TBlockInfo> AffectedBlockInfos;

        TRangeResynced(
                TBlockRange64 range,
                TInstant checksumStartTs,
                TDuration checksumDuration,
                TInstant readStartTs,
                TDuration readDuration,
                TInstant writeStartTs,
                TDuration writeDuration,
                TVector<IProfileLog::TBlockInfo> affectedBlockInfos)
            : Range(range)
            , ChecksumStartTs(checksumStartTs)
            , ChecksumDuration(checksumDuration)
            , ReadStartTs(readStartTs)
            , ReadDuration(readDuration)
            , WriteStartTs(writeStartTs)
            , WriteDuration(writeDuration)
            , AffectedBlockInfos(std::move(affectedBlockInfos))
        {
        }
    };

    //
    // OperationCompleted
    //

    struct TOperationCompleted
    {
        NProto::TPartitionStats Stats;

        ui64 TotalCycles = 0;
        ui64 ExecCycles = 0;

        TStackVec<int, 2> DeviceIndices;
        TDuration ActorSystemTime;

        bool Failed = false;
    };

    //
    // Events declaration
    //

    enum EEvents
    {
        EvBegin = TBlockStorePrivateEvents::PARTITION_NONREPL_START,

        EvUpdateCounters,
        EvReadBlocksCompleted,
        EvWriteBlocksCompleted,
        EvZeroBlocksCompleted,
        EvRangeMigrated,
        EvMigrateNextRange,
        EvWriteOrZeroCompleted,
        EvChecksumBlocksCompleted,
        EvResyncNextRange,
        EvRangeResynced,

        BLOCKSTORE_PARTITION_NONREPL_REQUESTS_PRIVATE(BLOCKSTORE_DECLARE_EVENT_IDS)

        EvEnd
    };

    static_assert(EvEnd < (int)TBlockStorePrivateEvents::PARTITION_NONREPL_END,
        "EvEnd expected to be < TBlockStorePrivateEvents::PARTITION_NONREPL_END");

    using TEvUpdateCounters = TResponseEvent<TEmpty, EvUpdateCounters>;
    using TEvReadBlocksCompleted = TResponseEvent<TOperationCompleted, EvReadBlocksCompleted>;
    using TEvWriteBlocksCompleted = TResponseEvent<TOperationCompleted, EvWriteBlocksCompleted>;
    using TEvZeroBlocksCompleted = TResponseEvent<TOperationCompleted, EvZeroBlocksCompleted>;
    using TEvChecksumBlocksCompleted = TResponseEvent<TOperationCompleted, EvChecksumBlocksCompleted>;

    using TEvRangeMigrated = TResponseEvent<
        TRangeMigrated,
        EvRangeMigrated
    >;

    using TEvMigrateNextRange = TResponseEvent<
        TMigrateNextRange,
        EvMigrateNextRange
    >;

    using TEvWriteOrZeroCompleted = TResponseEvent<
        TWriteOrZeroCompleted,
        EvWriteOrZeroCompleted
    >;

    using TEvResyncNextRange = TResponseEvent<
        TResyncNextRange,
        EvResyncNextRange
    >;

    using TEvRangeResynced = TResponseEvent<
        TRangeResynced,
        EvRangeResynced
    >;

    BLOCKSTORE_PARTITION_NONREPL_REQUESTS_PRIVATE(BLOCKSTORE_DECLARE_PROTO_EVENTS)
};

}   // namespace NCloud::NBlockStore::NStorage
