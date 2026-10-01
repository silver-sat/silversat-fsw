# Generated message definitions (DS-60, DS-61, DS-68).
#
# At build time, generator/msggen.py reads common.yaml and apps/*.yaml and
# writes C headers and channel definitions into the build directory:
#
#   <build>/silversat_msg/include/msg/common.h
#   <build>/silversat_msg/include/msg/<app>.h
#   <build>/silversat_msg/src/msg_<app>.c
#
# Nothing generated is committed. Editing a YAML file or a template reruns
# the generator on the next build; adding an app's YAML file reruns CMake.

if(NOT CONFIG_SS_MESSAGES)
  return()
endif()

set(MSG_DEFS_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(MSG_GEN_DIR ${CMAKE_BINARY_DIR}/silversat_msg)
set(MSG_GENERATOR ${CMAKE_CURRENT_SOURCE_DIR}/generator/msggen.py)

# One header and one source per app, named after its YAML file. The
# generator checks that each file's name matches the app inside it.
file(GLOB msg_app_defs CONFIGURE_DEPENDS ${MSG_DEFS_DIR}/apps/*.yaml)
file(GLOB msg_templates CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/generator/templates/*.j2)

set(msg_headers ${MSG_GEN_DIR}/include/msg/common.h)
set(msg_sources)
foreach(def ${msg_app_defs})
  get_filename_component(app ${def} NAME_WE)
  list(APPEND msg_headers ${MSG_GEN_DIR}/include/msg/${app}.h)
  list(APPEND msg_sources ${MSG_GEN_DIR}/src/msg_${app}.c)
endforeach()

add_custom_command(
  OUTPUT ${msg_headers} ${msg_sources}
  COMMAND ${PYTHON_EXECUTABLE} ${MSG_GENERATOR} --defs ${MSG_DEFS_DIR} --out ${MSG_GEN_DIR}
  DEPENDS ${MSG_GENERATOR} ${msg_templates} ${MSG_DEFS_DIR}/common.yaml ${msg_app_defs}
  COMMENT "Generating message headers and channels from messages/*.yaml"
)

# Every Zephyr library depends on zephyr_interface, so this makes the
# headers exist before any code that includes them is compiled.
add_custom_target(silversat_msg_gen DEPENDS ${msg_headers} ${msg_sources})
add_dependencies(zephyr_interface silversat_msg_gen)

zephyr_include_directories(${MSG_GEN_DIR}/include)

zephyr_library_named(silversat_messages)
zephyr_library_sources(${msg_sources})
