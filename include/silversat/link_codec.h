/*
 * Link codec: frames on the serial links between avionics and its peer
 * boards (DS-32, DS-33, DS-65, DS-66).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Every frame on every avionics serial link has the same layout, decided
 * 2026-10-02:
 *
 *   type  seq  len  payload (0 to 255 bytes)  CRC-32C (4 bytes)
 *
 *   type     what the frame is: the KISS type byte (DS-66)
 *   seq      the sender's sequence number, 0 to 255, then 0 again
 *   len      the payload length, so a truncated frame is caught
 *   CRC-32C  over type, seq, len, and payload (DS-65), least significant
 *            byte first (DS-64)
 *
 * The whole frame is then KISS-escaped and wrapped in FEND bytes. The CRC
 * is computed before escaping and checked after unescaping (DS-65).
 *
 * tools/link_codec.py is the Python twin, used by the simulators and the
 * ground (DS-91). The vectors in tests/unit/link_codec keep the two in step.
 */

#ifndef SILVERSAT_LINK_CODEC_H_
#define SILVERSAT_LINK_CODEC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* KISS special bytes. */
#define KISS_FEND  0xC0 /* frame end */
#define KISS_FESC  0xDB /* frame escape */
#define KISS_TFEND 0xDC /* escaped FEND */
#define KISS_TFESC 0xDD /* escaped FESC */

#define LINK_PAYLOAD_MAX 255
#define LINK_HEADER_LEN  3 /* type, seq, len */
#define LINK_CRC_LEN     4

/* An unescaped frame: header, payload, CRC. */
#define LINK_RAW_MAX (LINK_HEADER_LEN + LINK_PAYLOAD_MAX + LINK_CRC_LEN)

/* An encoded frame, if every byte needed escaping, plus the two FENDs. */
#define LINK_ENCODED_MAX (2 * LINK_RAW_MAX + 2)

struct link_packet {
	uint8_t type;
	uint8_t seq;
	uint8_t len;
	uint8_t payload[LINK_PAYLOAD_MAX];
};

/*
 * Encode a packet as a complete KISS frame in out. Returns the number of
 * bytes written, or -ENOSPC if out is too small. LINK_ENCODED_MAX is always
 * enough.
 */
int link_encode(const struct link_packet *packet, uint8_t *out, size_t out_size);

enum link_decode_result {
	/* Nothing yet: feed more bytes. */
	LINK_DECODE_NONE,
	/* A good packet is in *out. */
	LINK_DECODE_PACKET,
	/* A frame ended, but its CRC was wrong. */
	LINK_DECODE_BAD_CRC,
	/* A frame ended too short, or its length byte didn't match. */
	LINK_DECODE_BAD_LENGTH,
	/* A FESC was followed by something other than TFEND or TFESC. */
	LINK_DECODE_BAD_ESCAPE,
	/* A frame was longer than LINK_RAW_MAX bytes. */
	LINK_DECODE_TOO_LONG,
};

/*
 * Decoder state for one link. Feed it the received bytes one at a time, in
 * order. Bytes before the first FEND are noise and are ignored. A bad frame
 * is reported when its closing FEND arrives, and decoding carries on with
 * the next frame.
 */
struct link_decoder {
	uint8_t raw[LINK_RAW_MAX];
	uint16_t len;
	bool in_frame;
	bool escaped;
	/* Why the frame in progress is already bad, or LINK_DECODE_NONE. */
	enum link_decode_result broken;
};

void link_decoder_init(struct link_decoder *decoder);

/* Feed one received byte. Fills *out only for LINK_DECODE_PACKET. */
enum link_decode_result link_decode_byte(struct link_decoder *decoder, uint8_t byte,
					 struct link_packet *out);

/*
 * Sequence numbers for one link (DS-33). Each side numbers the frames it
 * sends, 0 to 255 and round again; the receiver uses them to count frames
 * lost on the way and to spot a frame received twice.
 */
struct link_seq {
	uint8_t next_tx;
	uint8_t last_rx;
	bool any_rx;
};

void link_seq_init(struct link_seq *seq);

/* The sequence number for the next frame sent. */
uint8_t link_seq_next(struct link_seq *seq);

/*
 * Note a received sequence number. Returns how many frames were lost just
 * before it (0 if none), or -1 if it repeats the last one received.
 */
int link_seq_received(struct link_seq *seq, uint8_t number);

#endif /* SILVERSAT_LINK_CODEC_H_ */
