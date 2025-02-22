#pragma once

#include <contrib/ydb/library/actors/core/actor.h>
#include <contrib/ydb/core/base/events.h>
#include <contrib/ydb/core/persqueue/writer/source_id_encoding.h>
#include <contrib/ydb/core/protos/flat_scheme_op.pb.h>
#include <contrib/ydb/library/persqueue/topic_parser/topic_parser.h>
#include <contrib/ydb/public/api/protos/persqueue_error_codes_v1.pb.h>


namespace NKikimr::NPQ {

struct TEvPartitionChooser {
    enum EEv {
        EvInitResult = EventSpaceBegin(TKikimrEvents::ES_PQ_PARTITION_CHOOSER),

        EvChooseResult,
        EvChooseError,
        EvRefreshRequest,

        EvEnd,
    };

    static_assert(EvEnd < EventSpaceEnd(TKikimrEvents::ES_PQ_PARTITION_CHOOSER), "expect EvEnd < EventSpaceEnd(TKikimrEvents::ES_PQ_PARTITION_CHOOSER)");

    struct TEvChooseResult: public TEventLocal<TEvChooseResult, EvChooseResult> {
        TEvChooseResult(ui32 partitionId, ui64 tabletId)
            : PartitionId(partitionId)
            , TabletId(tabletId) {
        }

        ui32 PartitionId;
        ui64 TabletId;
    };

    struct TEvChooseError: public TEventLocal<TEvChooseError, EvChooseError>  {
        TEvChooseError(Ydb::PersQueue::ErrorCode::ErrorCode code, TString&& errorMessage)
            : Code(code)
            , ErrorMessage(std::move(errorMessage)) {
        }

        Ydb::PersQueue::ErrorCode::ErrorCode Code;
        TString ErrorMessage;
    };

    struct TEvRefreshRequest: public TEventLocal<TEvRefreshRequest, EvRefreshRequest> {
    };

};

NActors::IActor* CreatePartitionChooserActor(TActorId parentId,
                                             const NKikimrSchemeOp::TPersQueueGroupDescription& config,
                                             NPersQueue::TTopicConverterPtr& fullConverter,
                                             const TString& sourceId,
                                             std::optional<ui32> preferedPartition,
                                             bool withoutHash = false);

} // namespace NKikimr::NPQ
