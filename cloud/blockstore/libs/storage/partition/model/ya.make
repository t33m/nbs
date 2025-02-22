LIBRARY()

GENERATE_ENUM_SERIALIZATION(mixed_index_cache.h)
GENERATE_ENUM_SERIALIZATION(operation_status.h)

SRCS(
    barrier.cpp
    blob_index.cpp
    block.cpp
    block_index.cpp
    block_mask.cpp
    checkpoint.cpp
    cleanup_queue.cpp
    commit_queue.cpp
    fresh_blob.cpp
    garbage_queue.cpp
    mixed_index_cache.cpp
    operation_status.cpp
    unconfirmed_blob.cpp
)

PEERDIR(
    cloud/blockstore/libs/common
    cloud/blockstore/libs/storage/core
    cloud/blockstore/libs/storage/protos
    
    cloud/storage/core/libs/common
    cloud/storage/core/libs/tablet
    
    library/cpp/protobuf/json
)

END()

RECURSE_FOR_TESTS(ut)
