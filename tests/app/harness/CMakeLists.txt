cmake_minimum_required(VERSION 3.20.0)

# Generate test_app's messages and channels along with the flight apps'.
set(SS_MESSAGES_EXTRA_APPS ${CMAKE_CURRENT_SOURCE_DIR}/apps)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(app_harness LANGUAGES C)

target_sources(app PRIVATE src/main.c src/test_app.c ../common/app_test.c)
target_include_directories(app PRIVATE ../common)
