# Frame manager (DS-20 to DS-25).

zephyr_library_named(silversat_frame_manager)
zephyr_library_sources(frame_manager.c)
zephyr_library_sources_ifdef(CONFIG_SS_FRAME_MANAGER_FLIGHT_TABLES frame_tables.c)
