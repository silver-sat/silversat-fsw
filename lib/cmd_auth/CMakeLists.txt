# Command authentication: DS-50 stages 1 and 2 (shape check, signature).

zephyr_library_named(silversat_cmd_auth)
zephyr_library_sources(cmd_auth.c)
# blake2.h stays private to this library: its names are generic.
zephyr_library_include_directories(../blake2s)
