SRCDIR(contrib/ydb/library/yql/minikql/invoke_builtins)

SRCS(
    mkql_builtins.cpp
    mkql_builtins.h
    mkql_builtins_abs.cpp
    mkql_builtins_add.cpp
    mkql_builtins_bitand.cpp
    mkql_builtins_bitnot.cpp
    mkql_builtins_bitor.cpp
    mkql_builtins_bitxor.cpp
    mkql_builtins_byteat.cpp
    mkql_builtins_codegen.cpp
    mkql_builtins_concat.cpp
    mkql_builtins_convert.cpp
    mkql_builtins_countbits.cpp
    mkql_builtins_decimal.cpp
    mkql_builtins_dec.cpp
    mkql_builtins_div.cpp
    mkql_builtins_find.cpp
    mkql_builtins_impl.h
    mkql_builtins_inc.cpp
    mkql_builtins_invprestr.cpp
    mkql_builtins_max.cpp
    mkql_builtins_min.cpp
    mkql_builtins_minus.cpp
    mkql_builtins_mod.cpp
    mkql_builtins_mul.cpp
    mkql_builtins_nanvl.cpp
    mkql_builtins_plus.cpp
    mkql_builtins_rotleft.cpp
    mkql_builtins_rotright.cpp
    mkql_builtins_shiftleft.cpp
    mkql_builtins_shiftright.cpp
    mkql_builtins_string_kernels.cpp
    mkql_builtins_sub.cpp
    mkql_builtins_substring.cpp
    mkql_builtins_with.cpp
    mkql_builtins_equals.cpp
    mkql_builtins_not_equals.cpp
    mkql_builtins_less.cpp
    mkql_builtins_less_or_equal.cpp
    mkql_builtins_greater.cpp
    mkql_builtins_greater_or_equal.cpp
)

PEERDIR(
    library/cpp/pop_count
    contrib/ydb/library/binary_json
    contrib/ydb/library/yql/minikql
    contrib/ydb/library/yql/minikql/arrow
    contrib/ydb/library/yql/public/udf
    contrib/libs/apache/arrow
)

YQL_LAST_ABI_VERSION()

PROVIDES(mkql_invoke_builtins)
