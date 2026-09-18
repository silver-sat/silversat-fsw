/*
 * SilverSat magnetometer public interface.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SILVERSAT_MAG_H_
#define SILVERSAT_MAG_H_

#include <stdint.h>

struct silversat_mag_sample {
	int16_t x;
	int16_t y;
	int16_t z;
};

#endif /* SILVERSAT_MAG_H_ */