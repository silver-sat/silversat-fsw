/*
 * Command text. See include/silversat/cmd_text.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include "silversat/cmd_text.h"

/* 18446744073709551615, the largest 64-bit value, has 20 digits. */
#define DIGITS_MAX 20

int cmd_text_split(const char *text, size_t len, struct cmd_text *out)
{
	size_t start = 0;

	out->count = 0;
	if (len == 0) {
		return -EINVAL;
	}
	for (size_t i = 0; i <= len; i++) {
		if (i < len && text[i] != ' ') {
			continue;
		}
		/* A word ends at i: a space, or the end of the text. */
		if (i == start || out->count == CMD_TEXT_WORDS_MAX || i - start > UINT8_MAX) {
			return -EINVAL; /* an empty word means extra spaces */
		}
		out->word[out->count] = &text[start];
		out->len[out->count] = (uint8_t)(i - start);
		out->count++;
		start = i + 1;
	}
	return 0;
}

bool cmd_text_word_is(const struct cmd_text *t, uint8_t i, const char *s)
{
	return i < t->count && strlen(s) == t->len[i] && memcmp(t->word[i], s, t->len[i]) == 0;
}

/*
 * Read decimal digits, with no leading zeros, as a 64-bit magnitude.
 * Returns 0 or -EINVAL, including when the value would overflow.
 */
static int read_digits(const char *s, size_t len, uint64_t *out)
{
	uint64_t value = 0;

	if (len == 0 || len > DIGITS_MAX || (len > 1 && s[0] == '0')) {
		return -EINVAL;
	}
	for (size_t i = 0; i < len; i++) {
		uint64_t digit;

		if (s[i] < '0' || s[i] > '9') {
			return -EINVAL;
		}
		digit = (uint64_t)(s[i] - '0');
		if (value > (UINT64_MAX - digit) / 10) {
			return -EINVAL;
		}
		value = value * 10 + digit;
	}
	*out = value;
	return 0;
}

int cmd_text_unsigned(const struct cmd_text *t, uint8_t i, uint64_t max, uint64_t *out)
{
	uint64_t value;

	if (i >= t->count || read_digits(t->word[i], t->len[i], &value) != 0 || value > max) {
		return -EINVAL;
	}
	*out = value;
	return 0;
}

int cmd_text_signed(const struct cmd_text *t, uint8_t i, int64_t min, int64_t max, int64_t *out)
{
	bool negative;
	uint64_t magnitude;
	int64_t value;

	if (i >= t->count) {
		return -EINVAL;
	}
	negative = t->len[i] > 0 && t->word[i][0] == '-';
	if (read_digits(t->word[i] + negative, t->len[i] - negative, &magnitude) != 0) {
		return -EINVAL;
	}
	if (negative) {
		/* -0 is not allowed; INT64_MIN's magnitude is one more than INT64_MAX. */
		if (magnitude == 0 || magnitude > (uint64_t)INT64_MAX + 1) {
			return -EINVAL;
		}
		value = magnitude == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)magnitude;
	} else {
		if (magnitude > (uint64_t)INT64_MAX) {
			return -EINVAL;
		}
		value = (int64_t)magnitude;
	}
	if (value < min || value > max) {
		return -EINVAL;
	}
	*out = value;
	return 0;
}

int cmd_text_bool(const struct cmd_text *t, uint8_t i, bool *out)
{
	if (cmd_text_word_is(t, i, "true")) {
		*out = true;
		return 0;
	}
	if (cmd_text_word_is(t, i, "false")) {
		*out = false;
		return 0;
	}
	return -EINVAL;
}

int cmd_text_name(const struct cmd_text *t, uint8_t i, const struct cmd_text_name *names,
		  size_t count, uint32_t *out)
{
	for (size_t n = 0; n < count; n++) {
		if (cmd_text_word_is(t, i, names[n].name)) {
			*out = names[n].value;
			return 0;
		}
	}
	return -EINVAL;
}
