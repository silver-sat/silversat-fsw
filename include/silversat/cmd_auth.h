/*
 * Command authentication: the first two stages of command ingest (DS-50).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A signed ground command is printable ASCII (DS-52):
 *
 *   <tag: 64 hex><salt: 16 hex><counter: 16 hex><command text>
 *
 * The tag is keyed BLAKE2s over everything after it, exactly as received.
 * cmd_auth_parse() is the shape check; cmd_auth_verify() checks the tag.
 * Nothing in the packet is trusted until cmd_auth_verify() succeeds; in
 * particular, the counter is checked only afterwards (DS-50, DS-53).
 */

#ifndef SILVERSAT_CMD_AUTH_H_
#define SILVERSAT_CMD_AUTH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CMD_AUTH_KEY_LEN     32 /* bytes */
#define CMD_AUTH_TAG_LEN     32 /* bytes */
#define CMD_AUTH_SALT_LEN    8  /* bytes */
#define CMD_AUTH_COUNTER_LEN 8  /* bytes: a 64-bit counter */

/* Characters in the fixed part of a packet, before the command text. */
#define CMD_AUTH_TAG_CHARS     (2 * CMD_AUTH_TAG_LEN)
#define CMD_AUTH_SALT_CHARS    (2 * CMD_AUTH_SALT_LEN)
#define CMD_AUTH_COUNTER_CHARS (2 * CMD_AUTH_COUNTER_LEN)
#define CMD_AUTH_HEADER_CHARS  (CMD_AUTH_TAG_CHARS + CMD_AUTH_SALT_CHARS + CMD_AUTH_COUNTER_CHARS)

/* Whole packets stay under 256 bytes (DS-66). */
#define CMD_AUTH_PACKET_MAX 255
#define CMD_AUTH_TEXT_MAX   (CMD_AUTH_PACKET_MAX - CMD_AUTH_HEADER_CHARS)

/* The fields of a packet that passed the shape check. */
struct cmd_auth_packet {
	uint8_t tag[CMD_AUTH_TAG_LEN];
	uint8_t salt[CMD_AUTH_SALT_LEN];
	uint64_t counter;
	/* The command text, inside the caller's buffer. Not NUL-terminated. */
	const char *text;
	size_t text_len;
};

/*
 * Stage 1, the shape check. Accepts a packet only if its length is in
 * range, the tag, salt, and counter are lowercase hex, and the command text
 * is 1 to CMD_AUTH_TEXT_MAX printable ASCII characters. Returns 0 and fills
 * *out, or returns -EINVAL.
 */
int cmd_auth_parse(const char *packet, size_t len, struct cmd_auth_packet *out);

/*
 * Stage 2, the signature check. Computes keyed BLAKE2s over everything
 * after the tag and compares it with the packet's tag in constant time.
 * packet and len are the same as given to cmd_auth_parse().
 */
bool cmd_auth_verify(const char *packet, size_t len, const struct cmd_auth_packet *parsed,
		     const uint8_t key[CMD_AUTH_KEY_LEN]);

#endif /* SILVERSAT_CMD_AUTH_H_ */
