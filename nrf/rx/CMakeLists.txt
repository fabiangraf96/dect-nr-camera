cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(dect_cam_rx)

target_sources(app PRIVATE
    src/main.c
    src/radio.c
    src/uart_out.c
    src/ctrl.c
)

target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
