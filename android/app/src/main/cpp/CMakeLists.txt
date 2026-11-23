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

# BlackBerry OS family.
# libbbcore: RIM's Java VM, natives and the front-end API. All of its memory
# comes from fixed-address regions (bbarena.h, forced in with -include) and
# libbbos loads it at a fixed address, which is what lets save states carry
# raw pointers from one run of the app to the next.
add_library(bbcore SHARED
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
    ${CORE}/bbos/bbarena.c
    ${CORE}/bbos/bbsnap.c
)
target_include_directories(bbcore PRIVATE ${CORE})
target_compile_options(bbcore PRIVATE
    -O2 -fwrapv -fno-strict-aliasing -include ${CORE}/bbos/bbarena.h
    -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-format-truncation)
target_link_options(bbcore PRIVATE -Wl,-Bsymbolic)
target_link_libraries(bbcore log m z)

# libbbos: the JNI bridge (loads libbbcore itself) and the OS importer
add_library(bbos SHARED
    bbjni.c
    ${CORE}/bbos/bbimport.c
    ${CORE}/bbos/cod.c
)
target_include_directories(bbos PRIVATE ${CORE})
target_compile_options(bbos PRIVATE
    -O2 -fwrapv -fno-strict-aliasing
    -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-format-truncation)
target_link_options(bbos PRIVATE -Wl,-Bsymbolic)
target_link_libraries(bbos log m z dl)
add_dependencies(bbos bbcore)
