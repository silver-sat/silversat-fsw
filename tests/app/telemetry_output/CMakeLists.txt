cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(telemetry_output_test LANGUAGES C)

target_sources(app PRIVATE src/main.c ../common/app_test.c)
target_include_directories(app PRIVATE ../common)
