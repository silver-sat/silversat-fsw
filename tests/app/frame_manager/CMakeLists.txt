cmake_minimum_required(VERSION 3.20.0)

# Generate the test apps' messages and channels along with the flight apps'.
set(SS_MESSAGES_EXTRA_APPS ${CMAKE_CURRENT_SOURCE_DIR}/apps)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(frame_manager_test LANGUAGES C)

target_sources(app PRIVATE
  src/main.c
  src/test_apps.c
  src/test_tables.c
  ../common/app_test.c
)
target_include_directories(app PRIVATE ../common)
