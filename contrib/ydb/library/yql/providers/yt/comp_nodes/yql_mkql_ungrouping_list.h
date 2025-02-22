#pragma once

#include <contrib/ydb/library/yql/minikql/computation/mkql_computation_node.h>
#include <contrib/ydb/library/yql/minikql/mkql_node.h>

namespace NYql {

NKikimr::NMiniKQL::IComputationNode* WrapYtUngroupingList(NKikimr::NMiniKQL::TCallable& callable, const NKikimr::NMiniKQL::TComputationNodeFactoryContext& ctx);

} // NYql
