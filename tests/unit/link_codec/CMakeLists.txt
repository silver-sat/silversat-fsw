cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(link_codec_test LANGUAGES C)

# The vectors, encoded by the Python twin (tools/link_codec.py), so the C
# codec is checked against an independent implementation.
set(TOOL ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/link_codec.py)
set(VECTORS ${CMAKE_CURRENT_BINARY_DIR}/vectors/link_vectors.h)
add_custom_command(
  OUTPUT ${VECTORS}
  COMMAND ${PYTHON_EXECUTABLE} ${TOOL} --vectors ${CMAKE_CURRENT_SOURCE_DIR}/vectors.yaml
          --c-vectors ${VECTORS}
  DEPENDS ${TOOL} ${CMAKE_CURRENT_SOURCE_DIR}/vectors.yaml
  COMMENT "Encoding the link codec vectors with tools/link_codec.py"
)
add_custom_target(link_codec_vectors DEPENDS ${VECTORS})
add_dependencies(app link_codec_vectors)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vectors)
