#pragma once

#include "public.h"

#include <cloud/filestore/config/storage.pb.h>

#include <util/datetime/base.h>
#include <util/generic/string.h>

namespace NCloud::NFileStore::NStorage {

////////////////////////////////////////////////////////////////////////////////

class TStorageConfig
{
private:
    NProto::TStorageConfig ProtoConfig;

public:
    struct TValueByName
    {
        enum class ENameStatus
        {
            NotFound,
            FoundInDefaults,
            FoundInProto
        };

        ENameStatus Status;
        TString Value;

        explicit TValueByName(ENameStatus status)
            : Status(status)
        {}

        explicit TValueByName(const TString& value)
            : Status(ENameStatus::FoundInProto)
            , Value(value)
        {}
    };

    TStorageConfig(const NProto::TStorageConfig& config = {})
        : ProtoConfig(config)
    {}

    TStorageConfig(const TStorageConfig&) = default;

    void Merge(const NProto::TStorageConfig& storageServiceConfig);

    TValueByName GetValueByName(const TString& name) const;

    TString GetSchemeShardDir() const;

    ui32 GetPipeClientRetryCount() const;
    TDuration GetPipeClientMinRetryTime() const;
    TDuration GetPipeClientMaxRetryTime() const;

    TDuration GetEstablishSessionTimeout() const;
    TDuration GetIdleSessionTimeout() const;

    bool GetWriteBatchEnabled() const;
    TDuration GetWriteBatchTimeout() const;
    ui32 GetWriteBlobThreshold() const;

    ui32 GetMaxBlobSize() const;

    ui32 GetFlushThreshold() const;
    ui32 GetCleanupThreshold() const;
    ui32 GetCompactionThreshold() const;
    ui32 GetCollectGarbageThreshold() const;
    ui32 GetFlushBytesThreshold() const;
    ui32 GetMaxDeleteGarbageBlobsPerTx() const;

    ui32 GetFlushThresholdForBackpressure() const;
    ui32 GetCleanupThresholdForBackpressure() const;
    ui32 GetCompactionThresholdForBackpressure() const;
    ui32 GetFlushBytesThresholdForBackpressure() const;

    TString GetHDDSystemChannelPoolKind() const;
    TString GetHDDLogChannelPoolKind() const;
    TString GetHDDIndexChannelPoolKind() const;
    TString GetHDDFreshChannelPoolKind() const;
    TString GetHDDMixedChannelPoolKind() const;

    TString GetSSDSystemChannelPoolKind() const;
    TString GetSSDLogChannelPoolKind() const;
    TString GetSSDIndexChannelPoolKind() const;
    TString GetSSDFreshChannelPoolKind() const;
    TString GetSSDMixedChannelPoolKind() const;

    TString GetHybridSystemChannelPoolKind() const;
    TString GetHybridLogChannelPoolKind() const;
    TString GetHybridIndexChannelPoolKind() const;
    TString GetHybridFreshChannelPoolKind() const;
    TString GetHybridMixedChannelPoolKind() const;

    ui32 GetAllocationUnitSSD() const;
    ui32 GetSSDUnitReadBandwidth() const;
    ui32 GetSSDUnitWriteBandwidth() const;
    ui32 GetSSDMaxReadBandwidth() const;
    ui32 GetSSDMaxWriteBandwidth() const;
    ui32 GetSSDUnitReadIops() const;
    ui32 GetSSDUnitWriteIops() const;
    ui32 GetSSDMaxReadIops() const;
    ui32 GetSSDMaxWriteIops() const;
    bool GetSSDThrottlingEnabled() const;
    TDuration GetSSDBoostTime() const;
    TDuration GetSSDBoostRefillTime() const;
    ui32 GetSSDUnitBoost() const;
    ui32 GetSSDBurstPercentage() const;
    ui32 GetSSDDefaultPostponedRequestWeight() const;
    ui32 GetSSDMaxPostponedWeight() const;
    ui32 GetSSDMaxWriteCostMultiplier() const;
    TDuration GetSSDMaxPostponedTime() const;
    ui32 GetSSDMaxPostponedCount() const;

    ui32 GetSSDMaxBlobsPerRange() const;
    ui32 GetSSDV2MaxBlobsPerRange() const;

    ui32 GetAllocationUnitHDD() const;
    ui32 GetHDDUnitReadBandwidth() const;
    ui32 GetHDDUnitWriteBandwidth() const;
    ui32 GetHDDMaxReadBandwidth() const;
    ui32 GetHDDMaxWriteBandwidth() const;
    ui32 GetHDDUnitReadIops() const;
    ui32 GetHDDUnitWriteIops() const;
    ui32 GetHDDMaxReadIops() const;
    ui32 GetHDDMaxWriteIops() const;
    bool GetHDDThrottlingEnabled() const;
    TDuration GetHDDBoostTime() const;
    TDuration GetHDDBoostRefillTime() const;
    ui32 GetHDDUnitBoost() const;
    ui32 GetHDDBurstPercentage() const;
    ui32 GetHDDDefaultPostponedRequestWeight() const;
    ui32 GetHDDMaxPostponedWeight() const;
    ui32 GetHDDMaxWriteCostMultiplier() const;
    TDuration GetHDDMaxPostponedTime() const;
    ui32 GetHDDMaxPostponedCount() const;

    ui32 GetHDDMediaKindOverride() const;
    ui32 GetMinChannelCount() const;

    ui32 GetMaxResponseBytes() const;
    ui32 GetMaxResponseEntries() const;

    ui32 GetDefaultNodesLimit() const;
    ui32 GetSizeToNodesRatio() const;

    bool GetDisableLocalService() const;

    ui32 GetDupCacheEntryCount() const;

    bool GetEnableCollectGarbageAtStart() const;

    bool GetThrottlingEnabled() const;

    TString GetTabletBootInfoBackupFilePath() const;
    bool GetHiveProxyFallbackMode() const;

    ui32 GetMaxBlocksPerTruncateTx() const;
    ui32 GetMaxTruncateTxInflight() const;

    TDuration GetCompactionRetryTimeout() const;

    ui32 GetReassignChannelsPercentageThreshold() const;

    ui32 GetCpuLackThreshold() const;

    ui32 GetSessionHistoryEntryCount() const;

    ui64 GetTenantHiveTabletId() const;

    TString GetFolderId() const;
    NCloud::NProto::EAuthorizationMode GetAuthorizationMode() const;

    void Dump(IOutputStream& out) const;
    void DumpHtml(IOutputStream& out) const;
};

}   // namespace NCloud::NFileStore::NStorage
