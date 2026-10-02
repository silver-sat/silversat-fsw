/*
 * Command routing: the last two stages of command ingest (DS-50).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * cmd_route() decodes the text of an authenticated ground command and
 * publishes it to the target app's command channel. The implementation is
 * generated from the YAML (messages/generator/templates/cmd_routes.c.j2),
 * so every command, argument, and allowed mode is stated once (DS-60,
 * DS-68). Call it only after the signature and counter checks pass: the
 * parser only ever sees authenticated input.
 */

#ifndef SILVERSAT_CMD_ROUTE_H_
#define SILVERSAT_CMD_ROUTE_H_

#include <stddef.h>
#include <stdint.h>

/* Checked in this order; the first that fails is returned. */
enum cmd_route_result {
	/* Decoded and published. */
	CMD_ROUTE_OK,
	/* Fewer than two words, or not split by single spaces. */
	CMD_ROUTE_BAD_SYNTAX,
	/* The first word is not an app. */
	CMD_ROUTE_UNKNOWN_APP,
	/* The app has no command by that name. */
	CMD_ROUTE_UNKNOWN_COMMAND,
	/* Too many or too few arguments. */
	CMD_ROUTE_BAD_ARG_COUNT,
	/* The command is not allowed in the current mode (DS-50). */
	CMD_ROUTE_MODE,
	/* An argument is malformed or out of range; see bad_arg. */
	CMD_ROUTE_BAD_ARG,
	/*
	 * The publish failed: the target channel's validator rejected the
	 * command, or the zbus pool had no buffer for it.
	 */
	CMD_ROUTE_PUBLISH_FAILED,
};

/* What cmd_route() found, for counters, events, and the ACK. */
struct cmd_route_info {
	/* enum app_id, or 0 if the app is unknown. */
	uint8_t app;
	/* The app's command id, or 0 if the command is unknown. */
	uint16_t command;
	/* For CMD_ROUTE_BAD_ARG: which argument, counting from 0. */
	uint8_t bad_arg;
};

/* mode is the current enum mode, from mode_chan. */
enum cmd_route_result cmd_route(const char *text, size_t len, uint8_t mode,
				struct cmd_route_info *info);

#endif /* SILVERSAT_CMD_ROUTE_H_ */
