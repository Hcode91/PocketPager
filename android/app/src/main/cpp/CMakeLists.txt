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
