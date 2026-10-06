cmake_minimum_required(VERSION 3.20.0)

# One build for the shared libraries' tests: signing (cmd_auth), replay
# protection (cmd_counter), command text decoding and routing (cmd_route),
# the link codec (link_codec), the housekeeping encoders (tlm_encode), and
# the FRAM service (nvm). Each keeps its own source file and test suite;
# they share a build only to save CI time, since most of a test's time is
# building Zephyr for it.

# typed_app has a command for every argument kind.
set(SS_MESSAGES_EXTRA_APPS ${CMAKE_CURRENT_SOURCE_DIR}/apps)

# The fake FRAM's and mirror's bindings (app.overlay puts them behind the
# fram and nvm-mirror aliases).
set(FAKES ${CMAKE_CURRENT_SOURCE_DIR}/../../fakes)
list(APPEND DTS_ROOT ${FAKES})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(libs_test LANGUAGES C)

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
          --vectors ${CMAKE_CURRENT_SOURCE_DIR}/command_vectors.yaml
          --c-vectors ${VECTOR_DIR}/route_vectors.h
  DEPENDS ${TOOLS}/command_text.py ${CMAKE_CURRENT_SOURCE_DIR}/command_vectors.yaml
  COMMENT "Converting the shared command text vectors"
)

# Link frames, encoded by the Python twin of the link codec.
add_custom_command(
  OUTPUT ${VECTOR_DIR}/link_vectors.h
  COMMAND ${PYTHON_EXECUTABLE} ${TOOLS}/link_codec.py
          --vectors ${CMAKE_CURRENT_SOURCE_DIR}/link_vectors.yaml
          --c-vectors ${VECTOR_DIR}/link_vectors.h
  DEPENDS ${TOOLS}/link_codec.py ${CMAKE_CURRENT_SOURCE_DIR}/link_vectors.yaml
  COMMENT "Encoding the link codec vectors with tools/link_codec.py"
)

# Housekeeping packets, encoded by the ground decoder's Python.
add_custom_command(
  OUTPUT ${VECTOR_DIR}/tlm_vectors.h
  COMMAND ${PYTHON_EXECUTABLE} ${TOOLS}/telemetry.py
          --extra-apps ${CMAKE_CURRENT_SOURCE_DIR}/apps
          --vectors ${CMAKE_CURRENT_SOURCE_DIR}/tlm_vectors.yaml
          --c-vectors ${VECTOR_DIR}/tlm_vectors.h
  DEPENDS ${TOOLS}/telemetry.py ${TOOLS}/command_text.py
          ${CMAKE_CURRENT_SOURCE_DIR}/tlm_vectors.yaml ${CMAKE_CURRENT_SOURCE_DIR}/apps/typed_app.yaml
  COMMENT "Encoding the housekeeping vectors with tools/telemetry.py"
)

add_custom_target(libs_vectors
  DEPENDS ${VECTOR_DIR}/signed_vectors.h ${VECTOR_DIR}/route_vectors.h
          ${VECTOR_DIR}/link_vectors.h ${VECTOR_DIR}/tlm_vectors.h)
add_dependencies(app libs_vectors)

target_sources(app PRIVATE src/auth.c src/counter.c src/route.c src/link_codec.c
  src/tlm_encode.c src/nvm.c ${FAKES}/fake_fram.c ${FAKES}/fake_retained_mem.c)
target_include_directories(app PRIVATE ${VECTOR_DIR} ${FAKES})
