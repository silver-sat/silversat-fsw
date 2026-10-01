/*
 * Command authentication. See include/silversat/cmd_auth.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include "blake2.h"
#include "silversat/cmd_auth.h"

/* The value of one lowercase hex digit, or -1. Uppercase is rejected (DS-52). */
static int hex_value(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	return -1;
}

/* Decode 2 * len hex characters into len bytes. Returns 0 or -EINVAL. */
static int hex_decode(const char *hex, uint8_t *out, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		int high = hex_value(hex[2 * i]);
		int low = hex_value(hex[2 * i + 1]);

		if (high < 0 || low < 0) {
			return -EINVAL;
		}
		out[i] = (uint8_t)((high << 4) | low);
	}
	return 0;
}

int cmd_auth_parse(const char *packet, size_t len, struct cmd_auth_packet *out)
{
	uint8_t counter[CMD_AUTH_COUNTER_LEN];
	const char *field = packet;

	if (len <= CMD_AUTH_HEADER_CHARS || len > CMD_AUTH_PACKET_MAX) {
		return -EINVAL;
	}
	if (hex_decode(field, out->tag, CMD_AUTH_TAG_LEN) != 0) {
		return -EINVAL;
	}
	field += CMD_AUTH_TAG_CHARS;
	if (hex_decode(field, out->salt, CMD_AUTH_SALT_LEN) != 0) {
		return -EINVAL;
	}
	field += CMD_AUTH_SALT_CHARS;
	if (hex_decode(field, counter, CMD_AUTH_COUNTER_LEN) != 0) {
		return -EINVAL;
	}
	field += CMD_AUTH_COUNTER_CHARS;

	/* The counter is written most significant digit first. */
	out->counter = 0;
	for (size_t i = 0; i < CMD_AUTH_COUNTER_LEN; i++) {
		out->counter = (out->counter << 8) | counter[i];
	}

	out->text = field;
	out->text_len = len - CMD_AUTH_HEADER_CHARS;
	for (size_t i = 0; i < out->text_len; i++) {
		if (out->text[i] < ' ' || out->text[i] > '~') {
			return -EINVAL;
		}
	}
	return 0;
}

/*
 * Compare without stopping at the first difference, so the time taken
 * doesn't reveal how much of a forged tag was right (DS-54).
 */
static bool tags_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
	uint8_t diff = 0;

	for (size_t i = 0; i < len; i++) {
		diff |= a[i] ^ b[i];
	}
	return diff == 0;
}

bool cmd_auth_verify(const char *packet, size_t len, const struct cmd_auth_packet *parsed,
		     const uint8_t key[CMD_AUTH_KEY_LEN])
{
	uint8_t computed[CMD_AUTH_TAG_LEN];

	if (len <= CMD_AUTH_TAG_CHARS) {
		return false;
	}
	/* Everything after the tag, exactly as received (DS-52). */
	if (blake2s(computed, sizeof(computed), packet + CMD_AUTH_TAG_CHARS,
		    len - CMD_AUTH_TAG_CHARS, key, CMD_AUTH_KEY_LEN) != 0) {
		return false;
	}
	return tags_equal(computed, parsed->tag, CMD_AUTH_TAG_LEN);
}
