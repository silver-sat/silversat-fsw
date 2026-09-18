#include <zephyr/ztest.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

ZTEST_SUITE(mag, NULL, NULL, NULL, NULL, NULL);

ZTEST(mag, test_emul_bus_ready)
{
	const struct device *bus = DEVICE_DT_GET(DT_NODELABEL(test_spi));

	zassert_true(device_is_ready(bus),
		     "emulated SPI controller not ready -- is app.overlay applied?");
}