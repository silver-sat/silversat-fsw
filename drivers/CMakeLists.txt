# Out-of-tree drivers.
#
# Each driver gets its own subdirectory and its own Kconfig symbol, so that
# an application only compiles what it actually enables. Add a line here when
# you add a driver, e.g.:
#
#   add_subdirectory_ifdef(CONFIG_SILVERSAT_MAG magnetometer)
#
# (No drivers yet -- the first one arrives with the magnetometer PR.)
