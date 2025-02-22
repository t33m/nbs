LIBRARY()

SRCS(
    abstract.cpp
    GLOBAL hash_by_columns.cpp
    container.cpp
)

PEERDIR(
    contrib/libs/apache/arrow
    contrib/ydb/core/protos
    contrib/ydb/core/tx/sharding
)

YQL_LAST_ABI_VERSION()
GENERATE_ENUM_SERIALIZATION(hash_by_columns.h)

END()
