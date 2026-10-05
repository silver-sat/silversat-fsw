cmake_minimum_required(VERSION 3.20.0)

# dts/bindings/ holds the fake watchdog's binding.
set(DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# watched_app: every flight app health watches is protected, so the tests
# need one that isn't.
set(SS_MESSAGES_EXTRA_APPS ${CMAKE_CURRENT_SOURCE_DIR}/apps)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(health_test LANGUAGES C)

# watched_app's attribute row, for the resource map's app_attrs[] (health
# reads it there).
zephyr_compile_definitions(SS_TEST_APP_ATTRS="test_app_attrs.h")
zephyr_include_directories(src)

target_sources(app PRIVATE src/main.c src/fake_watchdog.c ../common/app_test.c)
target_include_directories(app PRIVATE ../common src)
