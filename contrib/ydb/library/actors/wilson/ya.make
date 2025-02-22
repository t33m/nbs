LIBRARY()

SRCS(
    wilson_event.cpp
    wilson_span.cpp
    wilson_profile_span.cpp
    wilson_trace.cpp
    wilson_uploader.cpp
)

PEERDIR(
    contrib/ydb/library/actors/core
    contrib/ydb/library/actors/protos
    contrib/ydb/library/actors/wilson/protos
)

END()

RECURSE(
    protos
)
