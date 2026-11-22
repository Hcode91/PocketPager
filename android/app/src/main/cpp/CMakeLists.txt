cmake_minimum_required(VERSION 3.22)
project(pocketpager C)

set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../core)

add_library(pocketpager SHARED
    jni.c
    ${CORE}/cpu/i386.c
    ${CORE}/cpu/i8259.c
    ${CORE}/cpu/i8254.c
    ${CORE}/rim/board.c
    ${CORE}/rim/scramble.c
)

target_include_directories(pocketpager PRIVATE ${CORE})
target_compile_options(pocketpager PRIVATE
    -O2 -fwrapv -fno-strict-aliasing
    -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable)
target_link_libraries(pocketpager log m)

# BlackBerry OS family: RIM's Java VM, natives and the installer importer
add_library(bbos SHARED
    bbjni.c
    ${CORE}/bbos/cod.c
    ${CORE}/bbos/vm.c
    ${CORE}/bbos/interp.c
    ${CORE}/bbos/disasm.c
    ${CORE}/bbos/natives.c
    ${CORE}/bbos/natives_rim.c
    ${CORE}/bbos/natives_vm.c
    ${CORE}/bbos/natives_sys.c
    ${CORE}/bbos/natives_crypto.c
    ${CORE}/bbos/natives_gfx.c
    ${CORE}/bbos/natives_misc.c
    ${CORE}/bbos/natives_font.c
    ${CORE}/bbos/blockcipher.c
    ${CORE}/bbos/bbhost.c
    ${CORE}/bbos/bbimport.c
)
target_include_directories(bbos PRIVATE ${CORE})
target_compile_options(bbos PRIVATE
    -O2 -fwrapv -fno-strict-aliasing
    -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-format-truncation)
target_link_libraries(bbos log m z)
