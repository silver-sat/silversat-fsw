# Module-level CMakeLists. Referenced by zephyr/module.yml.
#
# This is NOT the application's CMakeLists -- that lives in app/. This file
# exists so that code under drivers/ and lib/ is compiled into any Zephyr
# application (or test) that uses this module, including the ztest binaries
# built by twister.

add_subdirectory(drivers)

zephyr_include_directories(include)
