LIBRARY()

SRCS(
    client.cpp
)

PEERDIR(
    contrib/ydb/public/sdk/cpp/client/impl/ydb_internal/grpc_connections
)

END()
