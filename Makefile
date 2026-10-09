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
# Its GDB server is OpenOCD's; ser2net puts the console and the radio UART
# on TCP ports. Overridable:  make flash FLATSAT=other-host
FLATSAT      ?= flatsat
GDB_PORT     ?= 3333
CONSOLE_PORT ?= 4000
# The Zephyr SDK's GDB for the board, which isn't on the PATH.
GDB          ?= $(firstword $(wildcard /opt/toolchains/zephyr-sdk-*/gnu/arm-zephyr-eabi/bin/arm-zephyr-eabi-gdb) arm-zephyr-eabi-gdb)
ELF          := $(BUILD_DIR)/zephyr/zephyr.elf
# `make run` builds native_sim into the same directory, so check the build is
# the flatsat's before loading it.
CHECK_FIRMWARE = grep -qs '^CONFIG_BOARD="$(BOARD)"' $(BUILD_DIR)/zephyr/.config || \
	{ echo 'No $(BOARD) firmware in $(BUILD_DIR): run `make build` first.'; exit 1; }

.DEFAULT_GOAL := help
.PHONY: help test twister test-quick test-python coverage run run-fresh build flash console debug clean

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
	python3 tools/flatsat.py --host $(FLATSAT) flash --gdb $(GDB) --port $(GDB_PORT) --elf $(ELF)

console:  ## Show the flatsat's console (Ctrl-C to stop)
	python3 tools/flatsat.py --host $(FLATSAT) console --port $(CONSOLE_PORT)

debug:  ## Debug the flatsat's firmware in GDB (the last `make build`)
	@$(CHECK_FIRMWARE)
	$(GDB) -ex "target extended-remote $(FLATSAT):$(GDB_PORT)" $(ELF)

clean:  ## Delete build output
	rm -rf $(BUILD_DIR) twister-out twister-out.*
