LIBRARY()

SRCS(
    blob_markers.cpp
    fresh_blob.cpp
)

PEERDIR(
    cloud/blockstore/public/api/protos
    cloud/blockstore/libs/storage/protos
    cloud/blockstore/libs/storage/protos_ydb

    contrib/ydb/library/actors/protos

    contrib/ydb/core/protos
)

END()
