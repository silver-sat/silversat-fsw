/*
 * Command text: splitting it into words and reading each argument.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Command text is "<app> <command> <arguments>", words separated by exactly
 * one space, with none at either end. Anything else is rejected rather
 * than tidied, so a command means exactly what was signed (DS-52). The
 * generated routing code (cmd_routes.c) calls these to decode each
 * command's arguments.
 *
 * Arguments are written as:
 *   unsigned integers  decimal digits, no leading zeros: 0, 7, 255
 *   signed integers    the same, with '-' for negatives: -128 (not -0)
 *   bool               true or false
 *   enum               the value's name from the YAML: safe, nominal
 */

#ifndef SILVERSAT_CMD_TEXT_H_
#define SILVERSAT_CMD_TEXT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The app, the command, and up to 14 arguments. */
#define CMD_TEXT_WORDS_MAX 16

/* The words of a command, pointing into the caller's text. */
struct cmd_text {
	const char *word[CMD_TEXT_WORDS_MAX];
	uint8_t len[CMD_TEXT_WORDS_MAX];
	uint8_t count;
};

/* A name an enum argument may take, and its value. */
struct cmd_text_name {
	const char *name;
	uint32_t value;
};

/*
 * Split text into words. Returns -EINVAL for empty text, a space at either
 * end, two spaces in a row, or more than CMD_TEXT_WORDS_MAX words.
 */
int cmd_text_split(const char *text, size_t len, struct cmd_text *out);

/* Whether word i is exactly s. */
bool cmd_text_word_is(const struct cmd_text *t, uint8_t i, const char *s);

/* Read word i as an unsigned integer no greater than max. 0 or -EINVAL. */
int cmd_text_unsigned(const struct cmd_text *t, uint8_t i, uint64_t max, uint64_t *out);

/* Read word i as a signed integer from min to max. 0 or -EINVAL. */
int cmd_text_signed(const struct cmd_text *t, uint8_t i, int64_t min, int64_t max, int64_t *out);

/* Read word i as true or false. 0 or -EINVAL. */
int cmd_text_bool(const struct cmd_text *t, uint8_t i, bool *out);

/* Read word i as one of the names given. 0 or -EINVAL. */
int cmd_text_name(const struct cmd_text *t, uint8_t i, const struct cmd_text_name *names,
		  size_t count, uint32_t *out);

#endif /* SILVERSAT_CMD_TEXT_H_ */
