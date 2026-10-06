# Command counter: replay protection (DS-50 stage 3, DS-53).

zephyr_library_named(silversat_cmd_counter)
zephyr_library_sources(cmd_counter.c)
# Floors in RAM, for builds without command ingest (the library tests).
# Command ingest keeps them in FRAM itself (apps/command_ingest/persist.c).
zephyr_library_sources_ifdef(CONFIG_SS_CMD_COUNTER_RAM_FLOORS floor_store_ram.c)
