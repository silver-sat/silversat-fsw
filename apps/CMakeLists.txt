# Flight software apps (DS-10).
#
# Each app gets its own subdirectory and its own Kconfig symbol, so that a
# build compiles only the apps it enables. Add one line per app as they are
# written, guarded by that app's Kconfig symbol:
#
#   add_subdirectory_ifdef(CONFIG_SS_* <app>)   (the app's own symbol)

if(CONFIG_SS_MESSAGES)
  # Compiles the resource map's build checks in every build that has
  # messages, even one with no app enabled.
  zephyr_library_named(silversat_apps)
  zephyr_library_sources(resource_map.c)
endif()
