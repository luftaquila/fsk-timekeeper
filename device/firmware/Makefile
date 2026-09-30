# fsk-timekeeper nRF52840 firmware (SuperMini nRF52840 + Ra-01SH).
#
#   make                 build; clones the pinned dependencies into vendor/ on first run
#   make clean           remove build/
#   make distclean       remove build/ and vendor/
#   FW_VERSION=1.2.3 make   version reported on the USB `I` line (CI passes the release tag)
#   NODE_DEBUG=1 make       print chip id / role over USB once per second (testing only)
#   make test              host tests: GPS, sync, queues, MAC simulator (C11 compiler, no ARM deps)
#
# One binary for every board: the role is chosen at runtime (master when a PC
# enumerates the USB device, sensor otherwise; src/main.c). The fleet key is not
# compiled in — it is provisioned per board over the serial `K` command into a
# reserved flash page (src/keystore.c), so this build is safe to publish from CI.

TARGET     := fsk-timekeeper
FW_VERSION ?= 1.0.0
BUILD      := build
VENDOR     := vendor

# Pinned bare-metal dependencies (CI caches vendor/ keyed on this file).
NRFX_REF       := v4.3.0
CMSIS_REF      := 5.9.0
RADIOLIB_REF   := 7.7.1
TINYUSB_REF    := 0.20.0
MONOCYPHER_REF := 4.0.2

NRFX       := $(VENDOR)/nrfx
CMSIS      := $(VENDOR)/CMSIS_5
RADIOLIB   := $(VENDOR)/RadioLib
TINYUSB    := $(VENDOR)/tinyusb
MONOCYPHER := $(VENDOR)/monocypher
MDK        := $(NRFX)/bsp/stable/mdk

PREFIX  ?= arm-none-eabi-
CC      := $(PREFIX)gcc
CXX     := $(PREFIX)g++
OBJCOPY := $(PREFIX)objcopy
SIZE    := $(PREFIX)size

# ---- sources -----------------------------------------------------------------
# Ours: full warnings.
APP_C := \
	src/main.c src/board.c src/meas.c src/capture.c src/crc32.c src/errlog.c \
	src/fault.c src/power.c src/secure.c src/keystore.c src/proto_usb.c src/node_id.c \
	src/usb.c src/usb_descriptors.c src/gps.c src/sync.c src/sensor_queue.c \
	src/master_queue.c src/registry.c src/mac_sensor.c src/mac_master.c
APP_CXX := src/radio_hal.cpp src/radio.cpp src/runtime.cpp

# Vendored: warnings off. RadioLib is built whole (non-Arduino mode, our HAL);
# unused modules are dropped by --gc-sections. TinyUSB: CDC-ACM device on nRF5x.
VEND_C := \
	$(MDK)/system_nrf52840.c \
	$(NRFX)/drivers/src/nrfx_power.c \
	$(MONOCYPHER)/src/monocypher.c \
	$(TINYUSB)/src/tusb.c \
	$(TINYUSB)/src/common/tusb_fifo.c \
	$(TINYUSB)/src/device/usbd.c \
	$(TINYUSB)/src/device/usbd_control.c \
	$(TINYUSB)/src/class/cdc/cdc_device.c \
	$(TINYUSB)/src/portable/nordic/nrf5x/dcd_nrf5x.c
VEND_CXX := $(shell find $(RADIOLIB)/src -name '*.cpp' 2>/dev/null)
STARTUP  := $(MDK)/gcc_startup_nrf52840.S

INC := -Isrc -Iconfig \
	-I$(TINYUSB)/src \
	-I$(NRFX) -I$(NRFX)/drivers/include -I$(NRFX)/hal -I$(NRFX)/haly -I$(NRFX)/bsp/stable -I$(MDK) \
	-I$(CMSIS)/CMSIS/Core/Include \
	-I$(RADIOLIB)/src \
	-I$(MONOCYPHER)/src

# ---- flags -------------------------------------------------------------------
MCU  := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
OPT  := -O2 -g -DNDEBUG
DEFS := -DNRF52840_XXAA -DFW_VERSION=\"$(FW_VERSION)\"
ifeq ($(NODE_DEBUG),1)
DEFS += -DNODE_DEBUG
endif

COMMON   := $(MCU) $(OPT) -ffunction-sections -fdata-sections $(INC) $(DEFS) -MMD -MP
CFLAGS   := $(COMMON) -std=gnu11
# Bare-metal C++ for RadioLib: drop the parts that need a hosted runtime.
CXXFLAGS := $(COMMON) -std=gnu++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit
# 4 KB heap (RadioLib's short-lived SPI buffers) keeps data + heap below the
# retained fault block at 0x20007E00 (linker/nrf52840_app.ld).
ASFLAGS  := $(MCU) -x assembler-with-cpp -DNRF52840_XXAA -D__HEAP_SIZE=4096

# newlib (nano) + nosys stubs; the linker script places the app at 0x26000
# (S140 user-app base) and INCLUDEs nrf_common.ld from the MDK. C++ objects
# need the C++ driver so libstdc++ (nano) is linked.
LDSCRIPT := linker/nrf52840_app.ld
LDFLAGS  := $(MCU) -T $(LDSCRIPT) -L$(MDK) --specs=nano.specs --specs=nosys.specs \
	-Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map -Wl,--print-memory-usage

# ---- objects -----------------------------------------------------------------
APP_OBJ  := $(patsubst %.c,$(BUILD)/%.o,$(APP_C)) $(patsubst %.cpp,$(BUILD)/%.o,$(APP_CXX))
VEND_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(VEND_C)) $(patsubst %.cpp,$(BUILD)/%.o,$(VEND_CXX)) \
	$(patsubst %.S,$(BUILD)/%.o,$(STARTUP))
OBJS := $(APP_OBJ) $(VEND_OBJ)

$(APP_OBJ):  WARN := -Wall -Wextra
$(VEND_OBJ): WARN := -w

# ---- targets -----------------------------------------------------------------
.PHONY: all build deps test clean distclean

# Dependencies are cloned first, then make re-reads this file so the RadioLib
# source glob sees them.
all: deps
	@$(MAKE) --no-print-directory build

build: $(BUILD)/$(TARGET).uf2 $(BUILD)/$(TARGET).hex $(BUILD)/$(TARGET).bin

test:
	@$(MAKE) --no-print-directory -C tests test

deps: $(NRFX) $(CMSIS) $(RADIOLIB) $(TINYUSB) $(MONOCYPHER)

$(NRFX):
	git clone --depth 1 --branch $(NRFX_REF) https://github.com/NordicSemiconductor/nrfx.git $@
$(CMSIS):
	git clone --depth 1 --branch $(CMSIS_REF) --filter=blob:none --sparse https://github.com/ARM-software/CMSIS_5.git $@
	git -C $@ sparse-checkout set CMSIS/Core/Include
$(RADIOLIB):
	git clone --depth 1 --branch $(RADIOLIB_REF) https://github.com/jgromes/RadioLib.git $@
$(TINYUSB):
	git clone --depth 1 --branch $(TINYUSB_REF) https://github.com/hathach/tinyusb.git $@
$(MONOCYPHER):
	git clone --depth 1 --branch $(MONOCYPHER_REF) https://github.com/LoupVaillant/Monocypher.git $@

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(WARN) -c $< -o $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(WARN) -c $< -o $@

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS) $(LDSCRIPT)
	$(CXX) $(OBJS) $(LDFLAGS) -o $@
	$(SIZE) $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/$(TARGET).hex: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O ihex $< $@

# UF2 for the Adafruit nRF52 bootloader drive (family 0xADA52840, app at 0x26000).
$(BUILD)/$(TARGET).uf2: $(BUILD)/$(TARGET).bin tools/bin2uf2.py
	python3 tools/bin2uf2.py --base 0x26000 --family 0xADA52840 $< $@

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf $(VENDOR)

-include $(OBJS:.o=.d)
