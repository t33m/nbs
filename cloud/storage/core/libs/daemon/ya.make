LIBRARY()

SRCS(
    app.cpp
    config_initializer.cpp
    mlock.cpp
    options.cpp
)

PEERDIR(
    cloud/storage/core/libs/common
    cloud/storage/core/libs/diagnostics

    contrib/ydb/library/actors/util
    library/cpp/deprecated/atomic
    library/cpp/logger
    library/cpp/sighandler
)

END()
