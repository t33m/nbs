UNITTEST()

SRCDIR(cloud/storage/core/libs/common)

PEERDIR(
    cloud/storage/core/libs/common
)

# this test sometimes times out under tsan
# in fact, there is no need to run it under tsan - the logic is single-threaded
IF (SANITIZER_TYPE != "thread")
    SRCS(
        compressed_bitmap_ut.cpp
    )
ENDIF()

SRCS(
    backoff_delay_provider_ut.cpp
    block_buffer_ut.cpp
    block_data_ref_ut.cpp
    concurrent_queue_ut.cpp
    context_ut.cpp
    error_ut.cpp
    file_io_service_ut.cpp
    guarded_sglist_ut.cpp
    ring_buffer_ut.cpp
    scheduler_ut.cpp
    scoped_handle_ut.cpp
    sglist_iter_ut.cpp
    sglist_ut.cpp
    thread_pool_ut.cpp
)

END()
