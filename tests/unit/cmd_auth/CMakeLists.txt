cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(cmd_auth_test LANGUAGES C)

# Test vectors signed by the same Python code the ground station uses, with
# Python's own BLAKE2s, so this checks the satellite against the ground.
set(SIGNER ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/sign_command.py)
set(VECTORS ${CMAKE_CURRENT_BINARY_DIR}/vectors/signed_vectors.h)
add_custom_command(
  OUTPUT ${VECTORS}
  COMMAND ${PYTHON_EXECUTABLE} ${SIGNER} --c-vectors ${VECTORS}
  DEPENDS ${SIGNER} ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/test_key.txt
  COMMENT "Signing test vectors with tools/sign_command.py"
)
add_custom_target(cmd_auth_vectors DEPENDS ${VECTORS})
add_dependencies(app cmd_auth_vectors)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vectors)
