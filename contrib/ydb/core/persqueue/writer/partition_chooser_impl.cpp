#include "partition_chooser_impl.h"
#include "contrib/ydb/public/sdk/cpp/client/ydb_proto/accessor.h"

#include <library/cpp/digest/md5/md5.h>
#include <contrib/ydb/core/persqueue/partition_key_range/partition_key_range.h>
#include <contrib/ydb/core/persqueue/utils.h>
#include <contrib/ydb/services/lib/sharding/sharding.h>

namespace NKikimr::NPQ {
namespace NPartitionChooser {

NYql::NDecimal::TUint128 Hash(const TString& sourceId) {
    return NKikimr::NDataStreams::V1::HexBytesToDecimal(MD5::Calc(sourceId));
}

ui32 TAsIsSharder::operator()(const TString& sourceId, ui32 totalShards) const {
    if (!sourceId) {
        return 0;
    }
    return (sourceId.at(0) - 'A') % totalShards;
}

ui32 TMd5Sharder::operator()(const TString& sourceId, ui32 totalShards) const {
    return NKikimr::NDataStreams::V1::ShardFromDecimal(Hash(sourceId), totalShards);
}

TString TAsIsConverter::operator()(const TString& sourceId) const {
    return sourceId;
}

TString TMd5Converter::operator()(const TString& sourceId) const {
    return AsKeyBound(Hash(sourceId));
}

} // namespace NPartitionChooser

IActor* CreatePartitionChooserActor(TActorId parentId,
                                    const NKikimrSchemeOp::TPersQueueGroupDescription& config,
                                    NPersQueue::TTopicConverterPtr& fullConverter,
                                    const TString& sourceId,
                                    std::optional<ui32> preferedPartition,
                                    bool withoutHash) {
    if (SplitMergeEnabled(config.GetPQTabletConfig())) {
        if (withoutHash) {
            return new NPartitionChooser::TPartitionChooserActor<NPartitionChooser::TBoundaryChooser<NPartitionChooser::TAsIsConverter>>
                (parentId, config, fullConverter, sourceId, preferedPartition);
        } else {
            return new NPartitionChooser::TPartitionChooserActor<NPartitionChooser::TBoundaryChooser<NPartitionChooser::TMd5Converter>>
                (parentId, config, fullConverter, sourceId, preferedPartition);
        }
    } else {
        if (withoutHash) {
            return new NPartitionChooser::TPartitionChooserActor<NPartitionChooser::THashChooser<NPartitionChooser::TAsIsSharder>>
                (parentId, config, fullConverter, sourceId, preferedPartition);
        } else {
            return new NPartitionChooser::TPartitionChooserActor<NPartitionChooser::THashChooser<NPartitionChooser::TMd5Sharder>>
                (parentId, config, fullConverter, sourceId, preferedPartition);
        }
    }
}


} // namespace NKikimr::NPQ
