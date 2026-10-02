cmake_minimum_required(VERSION 3.20.0)

# One build for the command libraries' tests: signing (cmd_auth), replay
# protection (cmd_counter), and text decoding and routing (cmd_route). Each
# keeps its own source file and test suite; they share a build only to save
# CI time, since most of a test's time is building Zephyr for it.

# typed_app has a command for every argument kind.
set(SS_MESSAGES_EXTRA_APPS ${CMAKE_CURRENT_SOURCE_DIR}/apps)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(command_test LANGUAGES C)

set(TOOLS ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools)
set(VECTOR_DIR ${CMAKE_CURRENT_BINARY_DIR}/vectors)

# Signed packets from the ground's signer, with Python's own BLAKE2s.
add_custom_command(
  OUTPUT ${VECTOR_DIR}/signed_vectors.h
  COMMAND ${PYTHON_EXECUTABLE} ${TOOLS}/sign_command.py --c-vectors ${VECTOR_DIR}/signed_vectors.h
  DEPENDS ${TOOLS}/sign_command.py ${TOOLS}/test_key.txt
  COMMENT "Signing test vectors with tools/sign_command.py"
)

# Command text vectors, shared with the ground's tests.
add_custom_command(
  OUTPUT ${VECTOR_DIR}/route_vectors.h
  COMMAND ${PYTHON_EXECUTABLE} ${TOOLS}/command_text.py
          --vectors ${CMAKE_CURRENT_SOURCE_DIR}/vectors.yaml
          --c-vectors ${VECTOR_DIR}/route_vectors.h
  DEPENDS ${TOOLS}/command_text.py ${CMAKE_CURRENT_SOURCE_DIR}/vectors.yaml
  COMMENT "Converting the shared command text vectors"
)

add_custom_target(command_vectors
  DEPENDS ${VECTOR_DIR}/signed_vectors.h ${VECTOR_DIR}/route_vectors.h)
add_dependencies(app command_vectors)

target_sources(app PRIVATE src/auth.c src/counter.c src/route.c)
target_include_directories(app PRIVATE ${VECTOR_DIR})
