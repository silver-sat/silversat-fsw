cmake_minimum_required(VERSION 3.20.0)

# typed_app has a command for every argument kind.
set(SS_MESSAGES_EXTRA_APPS ${CMAKE_CURRENT_SOURCE_DIR}/apps)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cmd_route_test LANGUAGES C)

# The vectors the ground's tests use too (tools/tests/test_command_text.py).
set(TOOL ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/command_text.py)
set(VECTORS ${CMAKE_CURRENT_BINARY_DIR}/vectors/route_vectors.h)
add_custom_command(
  OUTPUT ${VECTORS}
  COMMAND ${PYTHON_EXECUTABLE} ${TOOL} --vectors ${CMAKE_CURRENT_SOURCE_DIR}/vectors.yaml
          --c-vectors ${VECTORS}
  DEPENDS ${TOOL} ${CMAKE_CURRENT_SOURCE_DIR}/vectors.yaml
  COMMENT "Converting the shared command text vectors"
)
add_custom_target(cmd_route_vectors DEPENDS ${VECTORS})
add_dependencies(app cmd_route_vectors)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vectors)
