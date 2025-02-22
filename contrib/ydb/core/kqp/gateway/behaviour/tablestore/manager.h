#pragma once

#include "behaviour.h"

#include <contrib/ydb/services/metadata/manager/generic_manager.h>

namespace NKikimr::NKqp {

class TTableStoreManager: public NMetadata::NModifications::IOperationsManager {
    using TBase = NMetadata::NModifications::IOperationsManager;
    bool IsStandalone = false;
protected:
    NThreading::TFuture<TYqlConclusionStatus> DoModify(const NYql::TObjectSettingsImpl& settings, const ui32 nodeId,
        NMetadata::IClassBehaviour::TPtr manager, TInternalModificationContext& context) const override;
public:
    TTableStoreManager(bool isStandalone)
        : IsStandalone(isStandalone)
    {}
};

}
