#pragma once

#include <contrib/ydb/library/yql/minikql/computation/mkql_computation_node.h>
#include <contrib/ydb/library/yql/minikql/mkql_watermark.h>

namespace NKikimr {
namespace NMiniKQL {

IComputationNode* WrapMultiHoppingCore(TCallable& callable, const TComputationNodeFactoryContext& ctx, TWatermark& watermark);

}
}
