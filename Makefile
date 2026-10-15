# Convenience wrapper around idf.py, see docs/source/development.md
#
# Run `. $IDF_PATH/export.sh` (or the activation script of your ESP-IDF
# installation) in the shell first.
#
# VARIANT   : ospi (default), qspi or lilygo
# PORT      : serial port used by flash and monitor
#
# Local options (see `idf.py menuconfig`) can be kept in an optional
# sdkconfig.local file, it is applied on top of the variant settings.

VARIANT ?= ospi
BUILD   := build/$(VARIANT)

DEFAULTS := sdkconfig.defaults;sdkconfig.$(VARIANT)
ifneq ($(wildcard sdkconfig.local),)
        DEFAULTS := $(DEFAULTS);sdkconfig.local
endif

IDF := idf.py -B $(BUILD) \
	-D SDKCONFIG=$(BUILD)/sdkconfig \
	-D "SDKCONFIG_DEFAULTS=$(DEFAULTS)" \
	-D IDF_TARGET=esp32s3
ifdef PORT
        IDF += -p $(PORT)
endif

all: build

help:
	@echo 'Build the firmware:'
	@echo
	@echo '  make build [VARIANT=ospi|qspi|lilygo]'
	@echo
	@echo 'Upload the firmware to your radio:'
	@echo
	@echo '  make upload PORT=/dev/cu.usbmodem1101'
	@echo

build:
	$(IDF) build

upload:
	$(IDF) flash

monitor:
	$(IDF) monitor

menuconfig:
	$(IDF) menuconfig

clean:
	rm -Rf $(BUILD)

.PHONY: all help build upload monitor menuconfig clean
