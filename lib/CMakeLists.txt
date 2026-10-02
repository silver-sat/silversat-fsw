# Libraries shared by apps. Each has its own subdirectory and Kconfig symbol.

add_subdirectory_ifdef(CONFIG_SS_BLAKE2S blake2s)
add_subdirectory_ifdef(CONFIG_SS_CMD_AUTH cmd_auth)
add_subdirectory_ifdef(CONFIG_SS_CMD_COUNTER cmd_counter)
add_subdirectory_ifdef(CONFIG_SS_CMD_TEXT cmd_text)
