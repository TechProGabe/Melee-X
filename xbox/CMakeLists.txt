# Melee-X: default.xbe. Configure with nxdk's toolchain file (xbox/build.sh).
#
# Three kinds of code, three compile setups (docs/toolchain.md):
#   game  src/melee, src/sysdolphin (+ a few src/pc units): lowered and compiled
#         by tools/xbox/compile_game.py, linked here as objects
#   sdk   xbox/src/sdk, the Dolphin SDK: the game triple, so its structs match
#         the game's (i686-pc-windows-gnu -mno-ms-bitfields)
#   hw    xbox/src/hw: nxdk's own triple, talks to the kernel, pbkit and USB
cmake_minimum_required(VERSION 3.16)
project(melee_x C)

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
# __FILE__ relative to the checkout: no builder's home directory in the XBE
get_filename_component(ROOT_ABS "${ROOT}" ABSOLUTE)
add_compile_options("-ffile-prefix-map=${ROOT_ABS}/=")
set(NXDK_DIR $ENV{NXDK_DIR})

# ---- pbkit, patched so a GPU fault can't freeze the console ----
set(PBKIT_SRC ${NXDK_DIR}/lib/pbkit)
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/pbkit/pbkit.c
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/pbkit
    COMMAND python3 ${ROOT}/tools/xbox/patch_pbkit.py ${PBKIT_SRC}/pbkit.c ${CMAKE_BINARY_DIR}/pbkit/pbkit.c
    DEPENDS ${PBKIT_SRC}/pbkit.c ${ROOT}/tools/xbox/patch_pbkit.py
    COMMENT "patch pbkit.c")
add_library(mx_pbkit STATIC
    ${CMAKE_BINARY_DIR}/pbkit/pbkit.c
    ${PBKIT_SRC}/pbkit_gamma.c ${PBKIT_SRC}/pbkit_dma.c ${PBKIT_SRC}/pbkit_draw.c
    ${PBKIT_SRC}/pbkit_print.c ${PBKIT_SRC}/pbkit_pushbuffer.c)
target_include_directories(mx_pbkit PRIVATE ${PBKIT_SRC})
target_compile_options(mx_pbkit PRIVATE -w)

# ---- game objects (tools/xbox/compile_game.py, incremental via depfiles) ----
# XBOX_LTO (xbox/build.sh: XBOX_LTO=1): game and sdk code as bitcode, ThinLTO
# at the link (tools/xbox/thinlto_link.py); its own object folder
option(XBOX_LTO "ThinLTO over the game and sdk code" OFF)
# XBOX_PGO (xbox/build.sh): "gen" instruments the game and sdk code
# (counters out through xhw_pgo.c), a .profdata path optimizes with it
set(XBOX_PGO "" CACHE STRING "PGO: empty, gen, or a .profdata path")
if(XBOX_PGO AND NOT XBOX_PGO STREQUAL "gen")
    # relative to the checkout (docs/pgo.md): the sdk objects compile in the build folder
    get_filename_component(XBOX_PGO "${XBOX_PGO}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}/..")
endif()
set(GAME_OUT ${CMAKE_BINARY_DIR}/game)
set(GAME_ENV XBOX_LTO= XBOX_PGO=${XBOX_PGO})
if(XBOX_LTO)
    set(GAME_OUT ${GAME_OUT}-lto)
    set(GAME_ENV XBOX_LTO=1 XBOX_PGO=${XBOX_PGO})
endif()
if(XBOX_PGO STREQUAL "gen")
    set(GAME_OUT ${GAME_OUT}-pgogen)
elseif(XBOX_PGO)
    set(GAME_OUT ${GAME_OUT}-pgo)
endif()
add_custom_target(game_objs
    COMMAND ${CMAKE_COMMAND} -E env XBOX_GAME_OUT=${GAME_OUT} ${GAME_ENV} python3 ${ROOT}/tools/xbox/compile_game.py
    WORKING_DIRECTORY ${ROOT}
    BYPRODUCTS ${GAME_OUT}/objects.txt ${GAME_OUT}/objects.stamp
    COMMENT "game code: lower + compile"
    USES_TERMINAL)

# ---- sdk: the Dolphin SDK on the Xbox (game triple) ----
file(GLOB SDK_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/sdk/*.c ${CMAKE_CURRENT_SOURCE_DIR}/src/sdk/gx/*.c)
list(APPEND SDK_SOURCES
    ${ROOT}/src/pc/audio.c
    ${ROOT}/src/pc/misc.c
    ${ROOT}/extern/aurora/lib/dolphin/mtx/mtx.c
    ${ROOT}/extern/aurora/lib/dolphin/mtx/mtx44.c
    ${ROOT}/extern/aurora/lib/dolphin/mtx/mtxstack.c
    ${ROOT}/extern/aurora/lib/dolphin/mtx/mtxvec.c
    ${ROOT}/extern/aurora/lib/dolphin/mtx/quat.c
    ${ROOT}/extern/aurora/lib/dolphin/mtx/vec.c)

set(SDK_FLAGS
    --target=i686-pc-windows-gnu -mno-ms-bitfields -march=pentium3 -msse -mfpmath=sse
    -ffreestanding -nostdlibinc -fsigned-char -fno-strict-aliasing -fwrapv -ffp-contract=off
    -mno-stack-arg-probe -fno-asynchronous-unwind-tables -fno-auto-import -ffunction-sections
    -U_WIN32 -U__MINGW32__ -U__MINGW64__ -UWIN32 -U__WIN32 -U__WIN32__
    -DTARGET_PC=1 -DMELEE_PC=1 -DTARGET_XBOX=1 -DNXDK -D__STDC__=1
    -Wno-everything -Werror=implicit-function-declaration -Werror=implicit-int
    "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/include/game/xbox_game_prelude.h"
    "SHELL:-include ${ROOT}/src/pc/compat.h")
set(SDK_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/sdk
    ${CMAKE_CURRENT_SOURCE_DIR}/include/game
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${ROOT}/extern/aurora/include
    ${ROOT}/src
    ${ROOT}/src/sdk_include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/sdk)
if(XBOX_LTO)
    list(APPEND SDK_FLAGS -flto=thin)
endif()
if(XBOX_PGO STREQUAL "gen")
    list(APPEND SDK_FLAGS -fprofile-generate "SHELL:-mllvm -disable-vp" "SHELL:-mllvm -static-func-full-module-prefix=false")
    add_compile_definitions(XHW_PGO=1)
elseif(XBOX_PGO)
    list(APPEND SDK_FLAGS -fprofile-use=${XBOX_PGO} "SHELL:-mllvm -static-func-full-module-prefix=false")
endif()
add_library(mx_sdk OBJECT ${SDK_SOURCES})
target_include_directories(mx_sdk BEFORE PRIVATE ${SDK_INCLUDES})
target_compile_options(mx_sdk PRIVATE -O2 ${SDK_FLAGS}
    "SHELL:-isystem ${NXDK_DIR}/lib/pdclib/include"
    "SHELL:-isystem ${NXDK_DIR}/lib/pdclib/platform/xbox/include"
    "SHELL:-isystem ${NXDK_DIR}/lib/xboxrt/libc_extensions")
# log.c defines the printf-family redirections the prelude points at
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/sdk/log.c PROPERTIES COMPILE_DEFINITIONS XSDK_NO_STDIO_RENAME)

# ---- hw: kernel, pbkit, AC97, USB (nxdk triple) ----
file(GLOB HW_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/hw/*.c)
add_library(mx_hw OBJECT ${HW_SOURCES})
target_include_directories(mx_hw PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src/hw
    ${NXDK_DIR}/lib/sdl/SDL2/include
    ${NXDK_DIR}/lib/usb/libusbohci/inc
    ${NXDK_DIR}/lib/usb/libusbohci_xbox
    ${NXDK_DIR}/lib/zlib/zlib)
target_compile_definitions(mx_hw PRIVATE XBOX "USBH_USE_EXTERNAL_CONFIG=\"usbh_config_xbox.h\"")
target_compile_options(mx_hw PRIVATE -O2 -march=pentium3 -ffunction-sections -Wall -Wno-unused-function
    "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/include/xbuiltin.h")
# the string routines must not be recognised as themselves (xhw_string.c)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/hw/xhw_string.c PROPERTIES
    COMPILE_OPTIONS "-fno-builtin;-ffreestanding" COMPILE_DEFINITIONS XBUILTIN_OFF)

# ---- link ----
add_executable(melee_x $<TARGET_OBJECTS:mx_sdk> $<TARGET_OBJECTS:mx_hw>)
add_dependencies(melee_x game_objs)
if(XBOX_LTO)
    set_property(TARGET melee_x PROPERTY RULE_LAUNCH_LINK "python3 ${ROOT}/tools/xbox/thinlto_link.py")
endif()
set_property(TARGET melee_x APPEND PROPERTY LINK_DEPENDS ${GAME_OUT}/objects.stamp)
target_link_libraries(melee_x
    mx_pbkit
    ${NXDK_DIR}/lib/libSDL2.lib
    ${NXDK_DIR}/lib/libzlib.lib
    ${NXDK_DIR}/lib/libnxdk_automount_d.lib)
# The game objects come from outside CMake; the response file lists them.
target_link_options(melee_x PRIVATE
    "@${GAME_OUT}/objects.txt"
    -stack:1048576
    -include:_automount_d_drive
    -map:${CMAKE_BINARY_DIR}/melee_x.map
    # functions are COMDAT sections (-ffunction-sections): never fold two
    # identical ones into one address, the game compares function pointers
    -opt:noicf)
# The code's layout from whole-match profiles (tools/xbox/make_order.py):
# hot functions first, the rest in link order. A name the link doesn't have
# (renamed since, or in a library) is skipped without a warning.
set(ORDER_FILE ${CMAKE_CURRENT_SOURCE_DIR}/order.txt)
if(XBOX_PGO STREQUAL "gen")
    # nothing refers to the version word; tools/xbox/pgo_raw.py reads it
    target_link_options(melee_x PRIVATE -include:___llvm_profile_raw_version)
endif()
if(EXISTS ${ORDER_FILE})
    target_link_options(melee_x PRIVATE "-order:@${ORDER_FILE}" -ignore:4037)
    set_property(TARGET melee_x APPEND PROPERTY LINK_DEPENDS ${ORDER_FILE})
endif()
# -ffunction-sections in the game triple names each function's section
# .text$<name>, which /order can't move: plain .text, like nxdk's triple
# (tools/xbox/coff_text_plain.py; idempotent)
add_custom_command(TARGET melee_x PRE_LINK
    COMMAND python3 ${ROOT}/tools/xbox/coff_text_plain.py "@${GAME_OUT}/objects.txt" $<TARGET_OBJECTS:mx_sdk>
    COMMAND_EXPAND_LISTS
    COMMENT "game and sdk objects: .text$* -> .text")
add_custom_command(TARGET melee_x POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/xbe
    COMMAND ${NXDK_DIR}/tools/cxbe/cxbe "-OUT:${CMAKE_BINARY_DIR}/xbe/default.xbe" "-TITLE:Melee-X" $<TARGET_FILE:melee_x>
    COMMENT "cxbe -> xbe/default.xbe")
