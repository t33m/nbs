#pragma once
#include "yql_pg_provider.h"

#include <contrib/ydb/library/yql/core/yql_data_provider.h>
#include <contrib/ydb/library/yql/providers/common/transform/yql_visit.h>
#include <contrib/ydb/library/yql/providers/common/transform/yql_exec.h>

#include <util/generic/ptr.h>

namespace NYql {

TIntrusivePtr<IDataProvider> CreatePgDataSource(TPgState::TPtr state);
TIntrusivePtr<IDataProvider> CreatePgDataSink(TPgState::TPtr state);

THolder<TVisitorTransformerBase> CreatePgDataSourceTypeAnnotationTransformer(TPgState::TPtr state);
THolder<TVisitorTransformerBase> CreatePgDataSinkTypeAnnotationTransformer(TPgState::TPtr state);

THolder<TExecTransformerBase> CreatePgDataSinkExecTransformer(TPgState::TPtr state);


}
