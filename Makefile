# SilverSat flight software -- everyday commands.
#
# This file is the ONE definition of how the project is built and tested.
# CI calls the same targets (see .github/workflows/ci.yml), so what runs on
# your machine is what runs on the server. If you change a flag here, it
# changes everywhere.
#
# Nothing here is magic. Every recipe below is one west command. Read them.
# Type `make` on its own for the list.

BOARD      ?= nucleo_f446re
SIM        ?= native_sim
APP        ?= app
TESTS      ?= tests
BUILD_DIR  ?= build
REPO_ROOT  := $(shell pwd)
VERBOSITY  ?= -v
SANITIZERS ?= --enable-asan --enable-ubsan

# The platforms `make twister` tests, and which part of the tests. CI runs
# each platform in two halves, four jobs in parallel:
#   make twister PLATFORMS=native_sim SUBSET=1/2
PLATFORMS  ?= $(SIM) $(SIM)/native/64
SUBSET     ?=

# The flatsat: a Nucleo on an Ubuntu box on the tailnet (docs/flatsat.md).
# Its GDB server is OpenOCD's; ser2net puts the console, the radio UART, and
# the reset controller on TCP ports.
# Overridable:  make flash FLATSAT_HOST=other-host
FLATSAT_HOST ?= flatsat
GDB_PORT     ?= 3333
CONSOLE_PORT ?= 4000
RESET_PORT   ?= 4002

# The Zephyr SDK's GDB for the board, which isn't on the PATH.
GDB          ?= $(firstword $(wildcard /opt/toolchains/zephyr-sdk-*/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb) arm-zephyr-eabi-gdb)
ELF          := $(BUILD_DIR)/zephyr/zephyr.elf

# The Circuit Playground Bluefruit, the student tier (docs/cpb.md). Its own
# build directory, so it doesn't replace the flatsat's firmware.
CPB_BOARD    := adafruit_circuitplayground_bluefruit/nrf52840/uf2
CPB_BUILD_DIR ?= build-cpb

# The Metro RP2350, also the student tier (docs/metro-rp2350.md).
METRO_BOARD  := adafruit_metro_rp2350/rp2350b/m33
METRO_BUILD_DIR ?= build-metro

# Turn on a west group, keeping any already on: `west config` replaces the
# whole list, and a board whose group is off can't find its HAL.
define ENABLE_WEST_GROUP
filter="$$(west config manifest.group-filter || true)"; \
case ",$$filter," in *,+$(1),*) ;; \
*) west config manifest.group-filter -- "$${filter:+$$filter,}+$(1)" ;; esac
endef

# `make run` builds native_sim into the same directory, so check the build is
# the flatsat's before loading it.
CHECK_FIRMWARE = grep -qs '^CONFIG_BOARD="$(BOARD)"' $(BUILD_DIR)/zephyr/.config || \
	{ echo 'No $(BOARD) firmware in $(BUILD_DIR): run `make build` first.'; exit 1; }

.DEFAULT_GOAL := help
.PHONY: help test twister test-quick test-python coverage run run-fresh build flash console reset bootlog debug clean cpb cpb-setup metro metro-setup deploy-controller

help:  ## Show this list
	@echo ''
	@echo 'SilverSat flight software'
	@echo ''
	@awk 'BEGIN {FS = ":.*## "} /^[a-z][a-z-]*:.*## / \
	  { printf "  make %-14s %s\n", $$1, $$2 }' $(MAKEFILE_LIST)
	@echo ''

test: test-python twister  ## Run every test: Python, then emulated

twister:  ## Run the emulated tests on PLATFORMS (both native_sim widths)
	west twister $(foreach p,$(PLATFORMS),-p $(p)) -T $(TESTS) --inline-logs \
	  $(VERBOSITY) $(SANITIZERS) $(if $(SUBSET),--subset $(SUBSET))

test-quick: test-python  ## Run only the 64-bit tests (faster while iterating)
	west twister -p $(SIM)/native/64 -T $(TESTS) --inline-logs \
	  $(VERBOSITY) $(SANITIZERS)

test-python:  ## Run the Python tests: generator, tools, simulators
	python3 -m pytest messages/tests tools/tests sim/tests -q

coverage:  ## Run the tests and write a coverage report
	west twister -p $(SIM)/native/64 -T $(TESTS) --inline-logs \
	  $(VERBOSITY) --coverage --coverage-basedir $(REPO_ROOT)
	@echo ''
	@echo 'Report written. To view it:'
	@echo '  python3 -m http.server -d twister-out/coverage 8000'
	@echo ''

run:  ## Build and run the application under emulation
	west build -p -b $(SIM) $(APP) -d $(BUILD_DIR)
	./$(BUILD_DIR)/zephyr/zephyr.exe

run-fresh:  ## Like run, but with blank FRAM, as a new spacecraft
	west build -p -b $(SIM) $(APP) -d $(BUILD_DIR)
	./$(BUILD_DIR)/zephyr/zephyr.exe --eeprom_erase

build:  ## Cross-compile the application for the flatsat board
	west build -p -b $(BOARD) $(APP) -d $(BUILD_DIR)
	@echo ''
	@arm-zephyr-eabi-size $(BUILD_DIR)/zephyr/zephyr.elf 2>/dev/null \
	  || size $(BUILD_DIR)/zephyr/zephyr.elf

flash:  ## Load the last `make build` onto the flatsat, and start it
	@$(CHECK_FIRMWARE)
	python3 tools/flatsat.py --host $(FLATSAT_HOST) flash --gdb $(GDB) --port $(GDB_PORT) --elf $(ELF)

console:  ## Show the flatsat's console (Ctrl-C to stop)
	python3 tools/flatsat.py --host $(FLATSAT_HOST) console --port $(CONSOLE_PORT)

reset:  ## Reset the flatsat's board (for when it stops answering)
	python3 tools/flatsat.py --host $(FLATSAT_HOST) reset --port $(RESET_PORT)

bootlog:  ## Show the flatsat's last 16 boots and why each happened
	python3 tools/flatsat.py --host $(FLATSAT_HOST) bootlog --gdb $(GDB) --port $(GDB_PORT)

debug:  ## Debug the flatsat's firmware in GDB (the last `make build`)
	@$(CHECK_FIRMWARE)
	$(GDB) -ex "target extended-remote $(FLATSAT_HOST):$(GDB_PORT)" $(ELF)

clean:  ## Delete build output
	rm -rf $(BUILD_DIR) twister-out twister-out.*

cpb-setup:  ## Fetch the Nordic HAL the Circuit Playground needs (once)
	@$(call ENABLE_WEST_GROUP,cpb)
	west update hal_nordic

cpb:  ## Build the UF2 for the Circuit Playground Bluefruit (docs/cpb.md)
	@test -d ../modules/hal/nordic || $(MAKE) cpb-setup
	west build -p -b $(CPB_BOARD) $(APP) -d $(CPB_BUILD_DIR)
	@echo ''
	@echo 'Firmware: $(CPB_BUILD_DIR)/zephyr/zephyr.uf2'

metro-setup:  ## Fetch the Raspberry Pi HAL the Metro RP2350 needs (once)
	@$(call ENABLE_WEST_GROUP,metro)
	west update hal_rpi_pico

metro:  ## Build the UF2 for the Metro RP2350 (docs/metro-rp2350.md)
	@test -d ../modules/hal/rpi_pico || $(MAKE) metro-setup
	west build -p -S cdc-acm-console -b $(METRO_BOARD) $(APP) -d $(METRO_BUILD_DIR)
	@echo ''
	@echo 'Firmware: $(METRO_BUILD_DIR)/zephyr/zephyr.uf2'

deploy-controller:  ## Copy the flatsat controller firmware to its board (the Pico)
	flatsat/deploy.sh