# Adafruit Circuit Playground Bluefruit

A copy of this board's Zephyr definition, because upstream Zephyr doesn't
have it yet. The files other than this README are unchanged from
[adafruit/zephyr](https://github.com/adafruit/zephyr) at commit
`3567aee7fc2caa322fd534fbf4ed0c705d6c5a62`, the revision CircuitPython's
`zephyr-cp` port pins (`doc/` without its images, since nothing here builds
the Zephyr docs). `zephyr/module.yml` adds them to the board search path.

Don't edit them here: fix a bug in `adafruit/zephyr`, then copy the files
again and update the commit above. When the board reaches upstream Zephyr,
and our `west.yml` pins a release that has it, delete this directory.

What the flight software changes for this board is in
`app/boards/adafruit_circuitplayground_bluefruit_nrf52840_uf2.*`.
docs/cpb.md is the guide.
