#include "mkql_null.h"
#include <contrib/ydb/library/yql/minikql/computation/mkql_computation_node_holders.h>
#include <contrib/ydb/library/yql/minikql/mkql_node_cast.h>

namespace NKikimr {
namespace NMiniKQL {

IComputationNode* WrapNull(TCallable& callable, const TComputationNodeFactoryContext& ctx) {
    MKQL_ENSURE(callable.GetInputsCount() == 0, "Expected 0 arg");
    return ctx.NodeFactory.CreateImmutableNode(NUdf::TUnboxedValuePod());
}

}
}
