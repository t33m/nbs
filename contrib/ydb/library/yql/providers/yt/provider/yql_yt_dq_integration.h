#pragma once

#include "yql_yt_provider.h"

#include <contrib/ydb/library/yql/dq/integration/yql_dq_integration.h>

#include <util/generic/ptr.h>

namespace NYql {

THolder<IDqIntegration> CreateYtDqIntegration(TYtState* state);

}
