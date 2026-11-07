# GCC 16 added -Wnon-c-typedef-for-linkage, and esp-dl (pulled in by esp-sr)
# trips it in a managed component, so the diagnostic is demoted rather than fixed.
add_compile_options(-Wno-error=non-c-typedef-for-linkage)

# The framework's default event-loop task ("sys_evt") overflows its stock stack
# on a configured boot on this panel. The value is set in sdkconfig.product (the
# slot that wins over the board definition's own defaults); nothing is forced
# here, so the generated sdkconfig.h stays the one source of truth.
