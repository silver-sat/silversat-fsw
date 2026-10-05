cmake_minimum_required(VERSION 3.20.0)

# dts/bindings/ holds the fake watchdog's binding.
set(DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(health_test LANGUAGES C)

target_sources(app PRIVATE src/main.c src/fake_watchdog.c ../common/app_test.c)
target_include_directories(app PRIVATE ../common src)
