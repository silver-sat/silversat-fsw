# Command counter: replay protection (DS-50 stage 3, DS-53).

zephyr_library_named(silversat_cmd_counter)
zephyr_library_sources(cmd_counter.c)
# Replaced by an FRAM-backed store once the FRAM service exists (DS-70).
zephyr_library_sources(floor_store_ram.c)
