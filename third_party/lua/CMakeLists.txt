# Lua 5.4.8 (lua.org, MIT) as an ESP-IDF component — the scripting runtime of
# the panel's voice agent (app_agent), the way ESP-Claw's cap_lua carries its
# skills. The interpreter mains (lua.c), the test scaffold (ltests.c) and the
# amalgamation (onelua.c) are not compiled; everything else is stock.
set(LUA_SRCS
    lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldblib.c ldebug.c
    ldo.c ldump.c lfunc.c lgc.c linit.c liolib.c llex.c lmathlib.c lmem.c
    loadlib.c lobject.c lopcodes.c loslib.c lparser.c lstate.c lstring.c
    lstrlib.c ltable.c ltablib.c ltm.c lundump.c lutf8lib.c lvm.c lzio.c)

idf_component_register(
    SRCS ${LUA_SRCS}
    INCLUDE_DIRS "."
)

# Lua assumes a hosted C library; ESP-IDF/newlib provides enough of one. LUA_32BITS
# keeps numbers at 32-bit on this 32-bit target (half the NaN-boxing weight).
# PUBLIC: luaconf.h sizes Lua's number/integer types from this, and every
# includer of lua.h must see the same layout the library was built with.
target_compile_definitions(${COMPONENT_LIB} PUBLIC LUA_32BITS=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -w)
