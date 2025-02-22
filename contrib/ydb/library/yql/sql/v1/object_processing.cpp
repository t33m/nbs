#include "object_processing.h"

#include <contrib/ydb/library/yql/core/yql_callable_names.h>

namespace NSQLTranslationV1 {
using namespace NYql;

INode::TPtr TObjectProcessorImpl::BuildKeys() const {
    auto keys = Y("Key");
    keys = L(keys, Q(Y(Q("objectId"), Y("String", BuildQuotedAtom(Pos, ObjectId)))));
    keys = L(keys, Q(Y(Q("typeId"), Y("String", BuildQuotedAtom(Pos, TypeId)))));
    return keys;
}

TObjectProcessorImpl::TObjectProcessorImpl(TPosition pos, const TString& objectId, const TString& typeId, const TObjectOperatorContext& context)
    : TBase(pos)
    , TObjectOperatorContext(context)
    , ObjectId(objectId)
    , TypeId(typeId)
{

}

bool TObjectProcessorImpl::DoInit(TContext& ctx, ISource* src) {
    Y_UNUSED(src);
    Scoped->UseCluster(ServiceId, Cluster);
    auto options = FillFeatures(BuildOptions());
    auto keys = BuildKeys();

    Add("block", Q(Y(
        Y("let", "sink", Y("DataSink", BuildQuotedAtom(Pos, ServiceId), Scoped->WrapCluster(Cluster, ctx))),
        Y("let", "world", Y(TString(WriteName), "world", "sink", keys, Y("Void"), Q(options))),
        Y("return", ctx.PragmaAutoCommit ? Y(TString(CommitName), "world", "sink") : AstNode("world"))
    )));
    return TAstListNode::DoInit(ctx, src);
}

INode::TPtr TCreateObject::FillFeatures(INode::TPtr options) const {
    if (Features.size()) {
        auto features = Y();
        for (auto&& i : Features) {
            if (i.second.HasNode()) {
                features = L(features, Q(Y(BuildQuotedAtom(Pos, i.first), i.second.Build())));
            } else {
                features = L(features, Q(Y(BuildQuotedAtom(Pos, i.first))));
            }
        }
        return L(options, Q(Y(Q("features"), Q(features))));
    } else {
        return options;
    }
}

TObjectOperatorContext::TObjectOperatorContext(TScopedStatePtr scoped)
    : Scoped(scoped)
    , ServiceId(Scoped->CurrService)
    , Cluster(Scoped->CurrCluster)
{

}

}
