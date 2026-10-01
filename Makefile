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

# Where the flatsat lives. Overridable:  make flash FLATSAT=other-host
FLATSAT    ?= flatsat
GDB_PORT   ?= 2331

.DEFAULT_GOAL := help
.PHONY: help test test-quick test-messages coverage run build flash clean

help:  ## Show this list
	@echo ''
	@echo 'SilverSat flight software'
	@echo ''
	@awk 'BEGIN {FS = ":.*## "} /^[a-z][a-z-]*:.*## / \
	  { printf "  make %-14s %s\n", $$1, $$2 }' $(MAKEFILE_LIST)
	@echo ''

test: test-messages  ## Run the emulated test suite
	west twister -p $(SIM) -p $(SIM)/native/64 -T $(TESTS) --inline-logs \
	  $(VERBOSITY) $(SANITIZERS)

test-quick: test-messages  ## Run only the 64-bit tests (faster while iterating)
	west twister -p $(SIM)/native/64 -T $(TESTS) --inline-logs \
	  $(VERBOSITY) $(SANITIZERS)

test-messages:  ## Run the message generator's own tests (a second or two)
	python3 -m pytest messages/tests -q

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

build:  ## Cross-compile the application for the flatsat board
	west build -p -b $(BOARD) $(APP) -d $(BUILD_DIR)
	@echo ''
	@arm-zephyr-eabi-size $(BUILD_DIR)/zephyr/zephyr.elf 2>/dev/null \
	  || size $(BUILD_DIR)/zephyr/zephyr.elf

flash: build  ## Load the firmware onto the flatsat (NOT YET WIRED UP)
	@echo ''
	@echo '  make flash is not connected to hardware yet.'
	@echo ''
	@echo '  When the flatsat is running, this target will load'
	@echo '  $(BUILD_DIR)/zephyr/zephyr.elf onto the board over the'
	@echo '  network, roughly like this:'
	@echo ''
	@echo '    arm-zephyr-eabi-gdb -batch \'
	@echo '      -ex "target extended-remote $(FLATSAT):$(GDB_PORT)" \'
	@echo '      -ex "monitor reset" -ex "load" -ex "monitor go" \'
	@echo '      $(BUILD_DIR)/zephyr/zephyr.elf'
	@echo ''
	@echo '  Your firmware built fine, so the code is ready.'
	@echo '  Ask Lee when the rig is up.'
	@echo ''

clean:  ## Delete build output
	rm -rf $(BUILD_DIR) twister-out twister-out.*
