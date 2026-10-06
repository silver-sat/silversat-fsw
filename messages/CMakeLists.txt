# Generated message definitions (DS-60, DS-61, DS-68).
#
# At build time, generator/msggen.py reads common.yaml, apps/*.yaml and
# nvm_map.yaml and writes into the build directory (the docstring at the top
# of msggen.py says what each file holds):
#
#   <build>/silversat_msg/include/msg/common.h
#   <build>/silversat_msg/include/msg/<app>.h
#   <build>/silversat_msg/include/nvm/<app>.h   FRAM records (DS-74)
#   <build>/silversat_msg/include/nvm/map.h      every region, read-only
#   <build>/silversat_msg/src/msg_<app>.c
#   <build>/silversat_msg/src/cmd_routes.c       only with CONFIG_SS_CMD_ROUTES
#   <build>/silversat_msg/src/tlm_encode.c       only with CONFIG_SS_TLM_ENCODE
#
# Nothing generated is committed. Editing a YAML file or a template reruns
# the generator on the next build; adding an app's YAML file reruns CMake.
#
# A test can add apps of its own, generated like the flight apps, by setting
# SS_MESSAGES_EXTRA_APPS to a list of directories of <app>.yaml files before
# find_package(Zephyr):
#
#   set(SS_MESSAGES_EXTRA_APPS ${CMAKE_CURRENT_SOURCE_DIR}/apps)

if(NOT CONFIG_SS_MESSAGES)
  return()
endif()

set(MSG_DEFS_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(MSG_GEN_DIR ${CMAKE_BINARY_DIR}/silversat_msg)
set(MSG_GENERATOR ${CMAKE_CURRENT_SOURCE_DIR}/generator/msggen.py)

# One header and one source per app, named after its YAML file. The
# generator checks that each file's name matches the app inside it.
file(GLOB msg_app_defs CONFIGURE_DEPENDS ${MSG_DEFS_DIR}/apps/*.yaml)
set(msg_extra_args)
foreach(dir ${SS_MESSAGES_EXTRA_APPS})
  file(GLOB extra_defs CONFIGURE_DEPENDS ${dir}/*.yaml)
  list(APPEND msg_app_defs ${extra_defs})
  list(APPEND msg_extra_args --extra-apps ${dir})
endforeach()
file(GLOB msg_templates CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/generator/templates/*.j2)

set(msg_headers ${MSG_GEN_DIR}/include/msg/common.h ${MSG_GEN_DIR}/include/nvm/map.h)
set(msg_sources)
# Routing for every command; compiled only into builds with command ingest.
set(msg_routes ${MSG_GEN_DIR}/src/cmd_routes.c)
# Housekeeping encoders; compiled only into builds with telemetry output.
set(msg_tlm ${MSG_GEN_DIR}/src/tlm_encode.c)
foreach(def ${msg_app_defs})
  get_filename_component(app ${def} NAME_WE)
  list(APPEND msg_headers ${MSG_GEN_DIR}/include/msg/${app}.h)
  # Each app's FRAM records (DS-74), from nvm_map.yaml; empty if it has none.
  list(APPEND msg_headers ${MSG_GEN_DIR}/include/nvm/${app}.h)
  list(APPEND msg_sources ${MSG_GEN_DIR}/src/msg_${app}.c)
endforeach()

add_custom_command(
  OUTPUT ${msg_headers} ${msg_sources} ${msg_routes} ${msg_tlm}
  COMMAND ${PYTHON_EXECUTABLE} ${MSG_GENERATOR} --defs ${MSG_DEFS_DIR} ${msg_extra_args}
          --out ${MSG_GEN_DIR}
  DEPENDS ${MSG_GENERATOR} ${msg_templates} ${MSG_DEFS_DIR}/common.yaml ${msg_app_defs}
          ${MSG_DEFS_DIR}/nvm_map.yaml
  COMMENT "Generating message headers and channels from messages/*.yaml"
)

# Every Zephyr library depends on zephyr_interface, so this makes the
# headers exist before any code that includes them is compiled.
add_custom_target(silversat_msg_gen
  DEPENDS ${msg_headers} ${msg_sources} ${msg_routes} ${msg_tlm})
add_dependencies(zephyr_interface silversat_msg_gen)

zephyr_include_directories(${MSG_GEN_DIR}/include)

zephyr_library_named(silversat_messages)
zephyr_library_sources(${msg_sources})
zephyr_library_sources_ifdef(CONFIG_SS_CMD_ROUTES ${msg_routes})
zephyr_library_sources_ifdef(CONFIG_SS_TLM_ENCODE ${msg_tlm})
