#include "model.h"

#include <cloud/filestore/libs/storage/model/channel_data_kind.h>

#include <cloud/storage/core/protos/media.pb.h>

#include <contrib/ydb/core/protos/filestore_config.pb.h>

namespace NCloud::NFileStore::NStorage {

namespace {

////////////////////////////////////////////////////////////////////////////////

#define THROTTLING_PARAM(paramName, returnType)                                \
    returnType paramName(                                                      \
        const TStorageConfig& config,                                          \
        const ui32 mediaKind)                                                  \
    {                                                                          \
        switch (mediaKind) {                                                   \
            case NCloud::NProto::STORAGE_MEDIA_SSD:                            \
                return config.GetSSD ## paramName();                           \
            default:                                                           \
                return config.GetHDD ## paramName();                           \
        }                                                                      \
    }                                                                          \
// THROTTLING_PARAM

THROTTLING_PARAM(ThrottlingEnabled, bool);
THROTTLING_PARAM(UnitReadBandwidth, ui64);
THROTTLING_PARAM(UnitWriteBandwidth, ui64);
THROTTLING_PARAM(UnitReadIops, ui64);
THROTTLING_PARAM(UnitWriteIops, ui64);
THROTTLING_PARAM(MaxReadBandwidth, ui64);
THROTTLING_PARAM(MaxWriteBandwidth, ui64);
THROTTLING_PARAM(MaxReadIops, ui64);
THROTTLING_PARAM(MaxWriteIops, ui64);
THROTTLING_PARAM(BoostTime, TDuration);
THROTTLING_PARAM(BoostRefillTime, TDuration);
THROTTLING_PARAM(UnitBoost, ui64);
THROTTLING_PARAM(BurstPercentage, ui64);
THROTTLING_PARAM(DefaultPostponedRequestWeight, ui64);
THROTTLING_PARAM(MaxPostponedWeight, ui64);
THROTTLING_PARAM(MaxWriteCostMultiplier, ui64);
THROTTLING_PARAM(MaxPostponedTime, TDuration);
THROTTLING_PARAM(MaxPostponedCount, ui64);

#undef THROTTLING_PARAM

ui32 MaxReadBandwidth(
    const TStorageConfig& config,
    const NKikimrFileStore::TConfig& fileStore,
    const ui32 unitCount)
{
    const auto unitBandwidth = UnitReadBandwidth(
        config,
        fileStore.GetStorageMediaKind());
    const auto maxBandwidth = MaxReadBandwidth(
        config,
        fileStore.GetStorageMediaKind());

    return Max(
        static_cast<ui64>(fileStore.GetPerformanceProfileMaxReadBandwidth()),
        Min(maxBandwidth, unitCount * unitBandwidth) * 1_MB
    );
}

ui32 MaxWriteBandwidth(
    const TStorageConfig& config,
    const NKikimrFileStore::TConfig& fileStore,
    const ui32 unitCount)
{
    const auto unitBandwidth = UnitWriteBandwidth(
        config,
        fileStore.GetStorageMediaKind());
    const auto maxBandwidth = MaxWriteBandwidth(
        config,
        fileStore.GetStorageMediaKind());

    auto fileStoreMaxWriteBandwidth =
        fileStore.GetPerformanceProfileMaxWriteBandwidth();
    if (!fileStoreMaxWriteBandwidth) {
        fileStoreMaxWriteBandwidth =
            fileStore.GetPerformanceProfileMaxReadBandwidth();
    }

   return Max(
        static_cast<ui64>(fileStoreMaxWriteBandwidth),
        Min(maxBandwidth, unitCount * unitBandwidth) * 1_MB
    );
}

ui32 MaxReadIops(
    const TStorageConfig& config,
    const NKikimrFileStore::TConfig& fileStore,
    const ui32 unitCount)
{
    const auto unitIops = UnitReadIops(
        config,
        fileStore.GetStorageMediaKind());
    const auto maxIops = MaxReadIops(
        config,
        fileStore.GetStorageMediaKind());

    return Max(
        static_cast<ui64>(fileStore.GetPerformanceProfileMaxReadIops()),
        Min(maxIops, unitCount * unitIops)
    );
}

ui32 MaxWriteIops(
    const TStorageConfig& config,
    const NKikimrFileStore::TConfig& fileStore,
    const ui32 unitCount)
{
    const auto unitIops = UnitWriteIops(
        config,
        fileStore.GetStorageMediaKind());
    const auto maxIops = MaxWriteIops(
        config,
        fileStore.GetStorageMediaKind());

    auto fileStoreMaxWriteIops = fileStore.GetPerformanceProfileMaxWriteIops();
    if (!fileStoreMaxWriteIops) {
        fileStoreMaxWriteIops = fileStore.GetPerformanceProfileMaxReadIops();
    }

    return Max(
        static_cast<ui64>(fileStoreMaxWriteIops),
        Min(maxIops, unitCount * unitIops)
    );
}

bool ThrottlingEnabled(
    const TStorageConfig& config,
    const NKikimrFileStore::TConfig& fileStore)
{
    return ThrottlingEnabled(config, fileStore.GetStorageMediaKind());
}

ui32 BoostTime(
    const TStorageConfig& config,
    const NKikimrFileStore::TConfig& fileStore)
{
    return BoostTime(config, fileStore.GetStorageMediaKind()).MilliSeconds();
}

ui32 BoostRefillTime(
    const TStorageConfig& config,
    const NKikimrFileStore::TConfig& fileStore)
{
    return BoostRefillTime(
        config,
        fileStore.GetStorageMediaKind()).MilliSeconds();
}

ui32 BoostPercentage(
    const TStorageConfig& config,
    const NKikimrFileStore::TConfig& fileStore,
    const ui32 unitCount)
{
    const auto unitBoost = UnitBoost(config, fileStore.GetStorageMediaKind());
    return static_cast<ui32>(100.0 * static_cast<double>(unitBoost) /
        static_cast<double>(unitCount));
}

ui32 BurstPercentage(
    const TStorageConfig& config,
    const NKikimrFileStore::TConfig& fileStore)
{
    return BurstPercentage(config, fileStore.GetStorageMediaKind());
}

ui32 DefaultPostponedRequestWeight(
    const TStorageConfig& config,
    const NKikimrFileStore::TConfig& fileStore)
{
    return DefaultPostponedRequestWeight(
        config,
        fileStore.GetStorageMediaKind());
}

ui32 MaxPostponedWeight(
    const TStorageConfig& config,
    const NKikimrFileStore::TConfig& fileStore)
{
    return MaxPostponedWeight(config, fileStore.GetStorageMediaKind());
}

ui32 MaxWriteCostMultiplier(
    const TStorageConfig& config,
    const NKikimrFileStore::TConfig& fileStore)
{
    return MaxWriteCostMultiplier(config, fileStore.GetStorageMediaKind());
}

ui32 MaxPostponedTime(
    const TStorageConfig& config,
    const NKikimrFileStore::TConfig& fileStore)
{
    return MaxPostponedTime(
        config,
        fileStore.GetStorageMediaKind()).MilliSeconds();
}

ui32 MaxPostponedCount(
    const TStorageConfig& config,
    const NKikimrFileStore::TConfig& fileStore)
{
    return MaxPostponedCount(
        config,
        fileStore.GetStorageMediaKind());
}

auto GetAllocationUnit(
    const TStorageConfig& config,
    ui32 mediaKind)
{
    ui64 unit = 0;
    switch (mediaKind) {
        case NCloud::NProto::STORAGE_MEDIA_SSD:
            unit = config.GetAllocationUnitSSD();
            break;

        default:
            unit = config.GetAllocationUnitHDD();
            break;
    }

    Y_ABORT_UNLESS(unit != 0);
    return unit;
}

////////////////////////////////////////////////////////////////////////////////

struct TPoolKinds
{
    TString System;
    TString Log;
    TString Index;
    TString Fresh;
    TString Mixed;
};

TPoolKinds GetPoolKinds(
    const TStorageConfig& config,
    ui32 mediaKind)
{
    switch (mediaKind) {
        case NCloud::NProto::STORAGE_MEDIA_SSD:
            return {
                config.GetSSDSystemChannelPoolKind(),
                config.GetSSDLogChannelPoolKind(),
                config.GetSSDIndexChannelPoolKind(),
                config.GetSSDFreshChannelPoolKind(),
                config.GetSSDMixedChannelPoolKind(),
            };
        case NCloud::NProto::STORAGE_MEDIA_HYBRID:
            return {
                config.GetHybridSystemChannelPoolKind(),
                config.GetHybridLogChannelPoolKind(),
                config.GetHybridIndexChannelPoolKind(),
                config.GetHybridFreshChannelPoolKind(),
                config.GetHybridMixedChannelPoolKind(),
            };
        case NCloud::NProto::STORAGE_MEDIA_HDD:
        default:
            return {
                config.GetHDDSystemChannelPoolKind(),
                config.GetHDDLogChannelPoolKind(),
                config.GetHDDIndexChannelPoolKind(),
                config.GetHDDFreshChannelPoolKind(),
                config.GetHDDMixedChannelPoolKind(),
            };
    }
}

////////////////////////////////////////////////////////////////////////////////

ui32 ComputeAllocationUnitCount(
    const TStorageConfig& config,
    const NKikimrFileStore::TConfig& fileStore)
{
    if (!fileStore.GetBlocksCount()) {
        return 1;
    }

    double fileStoreSize =
        fileStore.GetBlocksCount() * fileStore.GetBlockSize() / double(1_GB);

    const auto unit = GetAllocationUnit(
        config,
        fileStore.GetStorageMediaKind());

    ui32 unitCount = std::ceil(fileStoreSize / unit);
    Y_DEBUG_ABORT_UNLESS(unitCount >= 1, "size %f unit %lu", fileStoreSize, unit);

    return unitCount;
}

ui32 ComputeMixedChannelCount(
    const TStorageConfig& config,
    const ui32 allocationUnitCount,
    const NKikimrFileStore::TConfig& fileStore)
{
    ui32 mixed = 0;
    for (const auto& channel: fileStore.GetExplicitChannelProfiles()) {
        if (channel.GetDataKind() == static_cast<ui32>(EChannelDataKind::Mixed)) {
            ++mixed;
        }
    }

    return Min(
        Max(
            allocationUnitCount,
            mixed,
            config.GetMinChannelCount()
        ),
        MaxChannelsCount
    );

}

void AddOrModifyChannel(
    const TString& poolKind,
    const ui32 channelId,
    const ui64 size,
    const EChannelDataKind dataKind,
    NKikimrFileStore::TConfig& config)
{
    while (channelId >= config.ExplicitChannelProfilesSize()) {
        config.AddExplicitChannelProfiles();
    }

    auto* profile = config.MutableExplicitChannelProfiles(channelId);
    if (profile->GetPoolKind().Empty()) {
        profile->SetPoolKind(poolKind);
    }

    profile->SetDataKind(static_cast<ui32>(dataKind));
    profile->SetSize(size);
    profile->SetReadIops(config.GetPerformanceProfileMaxReadIops());
    profile->SetWriteIops(config.GetPerformanceProfileMaxWriteIops());
    profile->SetReadBandwidth(config.GetPerformanceProfileMaxReadBandwidth());
    profile->SetWriteBandwidth(config.GetPerformanceProfileMaxWriteBandwidth());
}

void SetupChannels(
    ui32 unitCount,
    bool allocateMixed0Channel,
    const TStorageConfig& config,
    NKikimrFileStore::TConfig& fileStore)
{
    const auto unit = GetAllocationUnit(
        config,
        fileStore.GetStorageMediaKind());
    const auto poolKinds = GetPoolKinds(
        config,
        fileStore.GetStorageMediaKind());

    AddOrModifyChannel(
        poolKinds.System,
        0,
        128_MB,
        EChannelDataKind::System,
        fileStore
    );

    AddOrModifyChannel(
        poolKinds.Index,
        1,
        16_MB,
        EChannelDataKind::Index,
        fileStore
    );
    AddOrModifyChannel(
        poolKinds.Fresh,
        2,
        128_MB,
        EChannelDataKind::Fresh,
        fileStore
    );

    const ui32 mixed = ComputeMixedChannelCount(config, unitCount, fileStore);
    ui32 mixedChannelStart = 3;

    if (allocateMixed0Channel) {
        AddOrModifyChannel(
            poolKinds.Mixed,
            mixedChannelStart,
            unit * 1_GB,
            EChannelDataKind::Mixed0,
            fileStore
        );

        ++mixedChannelStart;
    }

    for (ui32 i = 0; i < mixed; ++i) {
        AddOrModifyChannel(
            poolKinds.Mixed,
            mixedChannelStart + i,
            unit * 1_GB,
            EChannelDataKind::Mixed,
            fileStore);
    }
}

void OverrideStorageMediaKind(
    const TStorageConfig& config,
    NKikimrFileStore::TConfig& fileStore)
{
    using namespace ::NCloud::NProto;
    if (fileStore.GetStorageMediaKind() == STORAGE_MEDIA_HDD) {
        switch(static_cast<EStorageMediaKind>(config.GetHDDMediaKindOverride())) {
            case STORAGE_MEDIA_HYBRID:
                fileStore.SetStorageMediaKind(STORAGE_MEDIA_HYBRID);
                break;
            case STORAGE_MEDIA_SSD:
                fileStore.SetStorageMediaKind(STORAGE_MEDIA_SSD);
                break;
            default:
                break; // pass
        }
    }
}

ui32 NodesLimit(
    const TStorageConfig& config,
    NKikimrFileStore::TConfig& fileStore)
{
    ui64 size = fileStore.GetBlocksCount() * fileStore.GetBlockSize();
    ui64 limit = Min(
        static_cast<ui64>(Max<ui32>()),
        size / config.GetSizeToNodesRatio());

    return Max(limit, static_cast<ui64>(config.GetDefaultNodesLimit()));
}

}   // namespace

////////////////////////////////////////////////////////////////////////////////

void SetupFileStorePerformanceAndChannels(
    bool allocateMixed0Channel,
    const TStorageConfig& config,
    NKikimrFileStore::TConfig& fileStore,
    const NProto::TFileStorePerformanceProfile& clientProfile)
{
    const auto allocationUnitCount =
        ComputeAllocationUnitCount(config, fileStore);

    OverrideStorageMediaKind(config, fileStore);

#define SETUP_PARAMETER(name, ...)                                             \
    fileStore.SetPerformanceProfile##name(                                     \
        clientProfile.Get##name()                                              \
            ? clientProfile.Get##name()                                        \
            : name(config, fileStore, ## __VA_ARGS__));                        \
// SETUP_PARAMENTS

    SETUP_PARAMETER(ThrottlingEnabled);
    SETUP_PARAMETER(MaxReadIops, allocationUnitCount);
    SETUP_PARAMETER(MaxReadBandwidth, allocationUnitCount);
    SETUP_PARAMETER(MaxWriteIops, allocationUnitCount);
    SETUP_PARAMETER(MaxWriteBandwidth, allocationUnitCount);
    SETUP_PARAMETER(BoostTime);
    SETUP_PARAMETER(BoostRefillTime);
    SETUP_PARAMETER(BoostPercentage, allocationUnitCount);
    SETUP_PARAMETER(BurstPercentage);
    SETUP_PARAMETER(DefaultPostponedRequestWeight);
    SETUP_PARAMETER(MaxPostponedWeight);
    SETUP_PARAMETER(MaxWriteCostMultiplier);
    SETUP_PARAMETER(MaxPostponedTime);
    SETUP_PARAMETER(MaxPostponedCount);

#undef SETUP_PARAMENTER

    fileStore.SetNodesCount(NodesLimit(config, fileStore));

    SetupChannels(
        allocationUnitCount,
        allocateMixed0Channel,
        config,
        fileStore);
}

void SetupFileStorePerformanceAndChannels(
    bool allocateMixed0Channel,
    const TStorageConfig& config,
    NKikimrFileStore::TConfig& fileStore)
{
    SetupFileStorePerformanceAndChannels(
        allocateMixed0Channel,
        config,
        fileStore,
        NProto::TFileStorePerformanceProfile());
}

}   // namespace NCloud::NFileStore::NStorage
