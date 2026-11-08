cmake_minimum_required(VERSION 3.20.0)

# tests/fakes holds the fake sensor's binding.
set(FAKES ${CMAKE_CURRENT_SOURCE_DIR}/../../fakes)
list(APPEND DTS_ROOT ${FAKES})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(temperature_app_test LANGUAGES C)

target_sources(app PRIVATE src/main.c ../common/app_test.c ${FAKES}/fake_temperature.c)
target_include_directories(app PRIVATE ../common ${FAKES})
