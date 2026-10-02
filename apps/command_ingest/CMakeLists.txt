# Command ingest (DS-50 to DS-54).

zephyr_library_named(silversat_command_ingest)
zephyr_library_sources(command_ingest.c)

# The keys (DS-54) become a header in the build directory, never in Git.
# A flight build refuses the published test keys.
set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
get_filename_component(KEY_FILE_0 ${CONFIG_SS_CMD_KEY_FILE_SLOT0} ABSOLUTE BASE_DIR ${REPO_ROOT})
get_filename_component(KEY_FILE_1 ${CONFIG_SS_CMD_KEY_FILE_SLOT1} ABSOLUTE BASE_DIR ${REPO_ROOT})
set(KEYS_HEADER ${CMAKE_CURRENT_BINARY_DIR}/keys/cmd_keys.h)
set(FLIGHT_ARG)
if(CONFIG_SS_FLIGHT_BUILD)
  set(FLIGHT_ARG --flight)
endif()

add_custom_command(
  OUTPUT ${KEYS_HEADER}
  COMMAND ${PYTHON_EXECUTABLE} ${REPO_ROOT}/tools/key_header.py
          --slot0 ${KEY_FILE_0} --slot1 ${KEY_FILE_1} --out ${KEYS_HEADER} ${FLIGHT_ARG}
  DEPENDS ${REPO_ROOT}/tools/key_header.py ${REPO_ROOT}/tools/sign_command.py
          ${KEY_FILE_0} ${KEY_FILE_1}
  COMMENT "Writing the command keys header"
)
add_custom_target(silversat_cmd_keys DEPENDS ${KEYS_HEADER})
zephyr_library_add_dependencies(silversat_cmd_keys)
zephyr_library_include_directories(${CMAKE_CURRENT_BINARY_DIR}/keys)
