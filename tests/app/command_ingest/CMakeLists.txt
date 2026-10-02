cmake_minimum_required(VERSION 3.20.0)

# target_app receives the routed commands.
set(SS_MESSAGES_EXTRA_APPS ${CMAKE_CURRENT_SOURCE_DIR}/apps)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(command_ingest_test LANGUAGES C)

# Packets signed by the ground's signer with the published test keys.
set(SIGNER ${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/sign_command.py)
set(PACKETS ${CMAKE_CURRENT_BINARY_DIR}/packets/packets.h)
add_custom_command(
  OUTPUT ${PACKETS}
  COMMAND ${PYTHON_EXECUTABLE} ${SIGNER} --c-packets ${CMAKE_CURRENT_SOURCE_DIR}/packets.yaml
          ${PACKETS}
  DEPENDS ${SIGNER} ${CMAKE_CURRENT_SOURCE_DIR}/packets.yaml
  COMMENT "Signing the command ingest test packets"
)
add_custom_target(command_ingest_packets DEPENDS ${PACKETS})
add_dependencies(app command_ingest_packets)

target_sources(app PRIVATE src/main.c ../common/app_test.c)
target_include_directories(app PRIVATE ../common ${CMAKE_CURRENT_BINARY_DIR}/packets)
