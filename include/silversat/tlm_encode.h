/*
 * Telemetry packets for the downlink (DS-61, DS-64, DS-69).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Every downlink packet starts with a printable kind byte, so the ground
 * can tell them apart (decided 2026-10-04):
 *
 *   'A', 'N'  command replies, as text: "ACK <counter> ok" (command ingest)
 *   'H'       housekeeping, binary:
 *
 *             'H'  app id  MET (8 bytes)  the app's housekeeping fields
 *
 *   'E'       an event (DS-10), binary: 'E', then struct event's fields
 *             (common.yaml): MET (8 bytes), app id, severity, event id
 *             (2 bytes), arg0, arg1 (4 bytes each, signed)
 *
 * All multi-byte values are little-endian (DS-64), and fields are packed in
 * YAML order with no padding. The implementation is generated from the YAML
 * (messages/generator/templates/tlm_encode.c.j2). The ground decodes with
 * the same definitions (tools/telemetry.py).
 */

#ifndef SILVERSAT_TLM_ENCODE_H_
#define SILVERSAT_TLM_ENCODE_H_

#include <stddef.h>
#include <stdint.h>

#include "msg/common.h"

#define TLM_KIND_HK       'H'
#define TLM_HK_HEADER_LEN 10 /* kind, app id, MET */
#define TLM_KIND_EVENT    'E'

/* Every app with housekeeping, by enum app_id, in definition order. */
extern const uint8_t tlm_hk_apps[];
extern const size_t tlm_hk_app_count;

/*
 * Encode app's latest housekeeping as an 'H' packet in out. Returns the
 * packet's length, or -ENOENT (no such app), -ENOSPC (out too small), or
 * -EBUSY (the channel couldn't be read without waiting).
 */
int tlm_encode_hk(uint8_t app, int64_t met_ms, uint8_t *out, size_t out_size);

/*
 * Encode an event as an 'E' packet in out. Returns the packet's length, or
 * -ENOSPC (out too small).
 */
int tlm_encode_event(const struct event *event, uint8_t *out, size_t out_size);

#endif /* SILVERSAT_TLM_ENCODE_H_ */
