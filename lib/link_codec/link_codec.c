/*
 * Link codec. See include/silversat/link_codec.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/sys/crc.h>

#include "silversat/link_codec.h"

/* CRC-32C with the parameters in DS-65, over the whole buffer. */
static uint32_t frame_crc(const uint8_t *data, size_t len)
{
	return crc32_c(0, data, len, true, true);
}

/* Append one byte to out, escaping FEND and FESC. Returns the new length or -ENOSPC. */
static int put_escaped(uint8_t byte, uint8_t *out, size_t out_size, size_t at)
{
	if (byte == KISS_FEND || byte == KISS_FESC) {
		if (at + 2 > out_size) {
			return -ENOSPC;
		}
		out[at] = KISS_FESC;
		out[at + 1] = byte == KISS_FEND ? KISS_TFEND : KISS_TFESC;
		return (int)(at + 2);
	}
	if (at + 1 > out_size) {
		return -ENOSPC;
	}
	out[at] = byte;
	return (int)(at + 1);
}

int link_encode(const struct link_packet *packet, uint8_t *out, size_t out_size)
{
	uint8_t raw[LINK_RAW_MAX];
	size_t raw_len = 0;
	uint32_t crc;
	int at;

	raw[raw_len++] = packet->type;
	raw[raw_len++] = packet->seq;
	raw[raw_len++] = packet->len;
	memcpy(&raw[raw_len], packet->payload, packet->len);
	raw_len += packet->len;

	crc = frame_crc(raw, raw_len);
	for (int i = 0; i < LINK_CRC_LEN; i++) {
		raw[raw_len++] = (uint8_t)(crc >> (8 * i)); /* least significant first */
	}

	if (out_size < 1) {
		return -ENOSPC;
	}
	out[0] = KISS_FEND;
	at = 1;
	for (size_t i = 0; i < raw_len; i++) {
		at = put_escaped(raw[i], out, out_size, (size_t)at);
		if (at < 0) {
			return at;
		}
	}
	if ((size_t)at + 1 > out_size) {
		return -ENOSPC;
	}
	out[at++] = KISS_FEND;
	return at;
}

void link_decoder_init(struct link_decoder *decoder)
{
	decoder->len = 0;
	decoder->in_frame = false;
	decoder->escaped = false;
	decoder->broken = LINK_DECODE_NONE;
}

/* Start collecting a new frame; a FEND both ends one frame and starts the next. */
static void start_frame(struct link_decoder *decoder)
{
	decoder->len = 0;
	decoder->in_frame = true;
	decoder->escaped = false;
	decoder->broken = LINK_DECODE_NONE;
}

/* A frame has ended: check it and, if good, copy it out. */
static enum link_decode_result finish_frame(const struct link_decoder *decoder,
					    struct link_packet *out)
{
	const uint8_t *raw = decoder->raw;
	size_t payload_len;
	uint32_t received_crc = 0;

	if (decoder->broken != LINK_DECODE_NONE) {
		return decoder->broken;
	}
	if (decoder->escaped) {
		return LINK_DECODE_BAD_ESCAPE; /* the frame ended right after a FESC */
	}
	if (decoder->len < LINK_HEADER_LEN + LINK_CRC_LEN) {
		return LINK_DECODE_BAD_LENGTH;
	}
	payload_len = decoder->len - LINK_HEADER_LEN - LINK_CRC_LEN;
	if (raw[2] != payload_len) {
		return LINK_DECODE_BAD_LENGTH;
	}
	for (int i = 0; i < LINK_CRC_LEN; i++) {
		received_crc |= (uint32_t)raw[decoder->len - LINK_CRC_LEN + i] << (8 * i);
	}
	if (received_crc != frame_crc(raw, decoder->len - LINK_CRC_LEN)) {
		return LINK_DECODE_BAD_CRC;
	}
	out->type = raw[0];
	out->seq = raw[1];
	out->len = raw[2];
	memcpy(out->payload, &raw[LINK_HEADER_LEN], payload_len);
	return LINK_DECODE_PACKET;
}

enum link_decode_result link_decode_byte(struct link_decoder *decoder, uint8_t byte,
					 struct link_packet *out)
{
	enum link_decode_result result;

	if (byte == KISS_FEND) {
		if (!decoder->in_frame || (decoder->len == 0 && !decoder->escaped &&
					   decoder->broken == LINK_DECODE_NONE)) {
			/* The first FEND, or FENDs back to back: nothing to report. */
			start_frame(decoder);
			return LINK_DECODE_NONE;
		}
		result = finish_frame(decoder, out);
		start_frame(decoder);
		return result;
	}
	if (!decoder->in_frame || decoder->broken != LINK_DECODE_NONE) {
		return LINK_DECODE_NONE; /* noise, or the rest of a bad frame */
	}
	if (decoder->escaped) {
		decoder->escaped = false;
		if (byte == KISS_TFEND) {
			byte = KISS_FEND;
		} else if (byte == KISS_TFESC) {
			byte = KISS_FESC;
		} else {
			decoder->broken = LINK_DECODE_BAD_ESCAPE;
			return LINK_DECODE_NONE;
		}
	} else if (byte == KISS_FESC) {
		decoder->escaped = true;
		return LINK_DECODE_NONE;
	}
	if (decoder->len == LINK_RAW_MAX) {
		decoder->broken = LINK_DECODE_TOO_LONG;
		return LINK_DECODE_NONE;
	}
	decoder->raw[decoder->len++] = byte;
	return LINK_DECODE_NONE;
}

void link_seq_init(struct link_seq *seq)
{
	seq->next_tx = 0;
	seq->last_rx = 0;
	seq->any_rx = false;
}

uint8_t link_seq_next(struct link_seq *seq)
{
	return seq->next_tx++; /* wraps from 255 to 0 */
}

int link_seq_received(struct link_seq *seq, uint8_t number)
{
	uint8_t lost;

	if (!seq->any_rx) {
		seq->any_rx = true;
		seq->last_rx = number;
		return 0;
	}
	if (number == seq->last_rx) {
		return -1;
	}
	/* 8-bit arithmetic, so 255 followed by 0 loses nothing. */
	lost = (uint8_t)(number - seq->last_rx - 1);
	seq->last_rx = number;
	return lost;
}
