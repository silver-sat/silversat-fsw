# Health (DS-43).

zephyr_library_named(silversat_health)
zephyr_library_sources(health.c boot.c)
zephyr_library_sources_ifdef(CONFIG_SS_HEALTH_STATUS_LOG status.c)
