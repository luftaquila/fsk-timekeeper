CC ?= cc
CXX ?= c++
OUT := ../build/host
CFLAGS := -std=c11 -g -O1 -Wall -Wextra -Werror -Wno-pointer-to-int-cast \
	-fsanitize=undefined -fno-sanitize-recover=all
CXXFLAGS := -std=gnu++17 -g -O1 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
	-fsanitize=undefined -fno-sanitize-recover=all
SRC := ../src
RADIOLIB := ../vendor/RadioLib/src

GPS_CASES := healthy frequency_scale stale_rmc resume_after_silence void_transition malformed_active \
	bad_date bad_time bad_checksum hfxo_recovery missed_pps report_expiry \
	status_not_exact_a millis_wrap segments report_timeout glitch hfxo_stop_between_edges
SIM_CASES := steady overflow sync_gap long_gap reboot reboot_new_tag reboot_full faults \
	id_collision ev_wrap hfxo_stop
RADIO_CASES := probe_frame probe_silent probe_busy_stuck begin begin_dead

# The SX126x driver of the vendor RadioLib, built for the host without its warnings.
RADIOLIB_SRC := $(RADIOLIB)/Module.cpp $(RADIOLIB)/Hal.cpp $(RADIOLIB)/protocols/PhysicalLayer/PhysicalLayer.cpp \
	$(wildcard $(RADIOLIB)/modules/SX126x/*.cpp) $(wildcard $(RADIOLIB)/utils/*.cpp)
RADIOLIB_OBJ := $(patsubst $(RADIOLIB)/%.cpp,$(OUT)/radiolib/%.o,$(RADIOLIB_SRC))

QUEUE_SRC := $(SRC)/sensor_queue.c $(SRC)/sync.c $(SRC)/master_queue.c $(SRC)/registry.c \
	$(SRC)/proto_usb.c $(SRC)/errlog.c $(SRC)/crc32.c
MAC_SRC := $(SRC)/mac_sensor.c $(SRC)/mac_master.c $(QUEUE_SRC)
HEADERS := $(wildcard $(SRC)/*.h)

.PHONY: test
test: $(OUT)/gps-test $(OUT)/sync-test $(OUT)/queue-test $(OUT)/fault-test $(OUT)/mac-sim $(OUT)/radio-test
	@ulimit -c 0; failed=0; \
	for case in $(GPS_CASES); do "$(OUT)/gps-test" "$$case" || failed=1; done; \
	"$(OUT)/sync-test" || failed=1; \
	"$(OUT)/queue-test" || failed=1; \
	"$(OUT)/fault-test" || failed=1; \
	for case in $(SIM_CASES); do "$(OUT)/mac-sim" "$$case" || failed=1; done; \
	for case in $(RADIO_CASES); do "$(OUT)/radio-test" "$$case" || failed=1; done; \
	exit $$failed

$(OUT)/gps-test: gps_test.c fakes/nrf.h $(SRC)/gps.c $(HEADERS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -Ifakes gps_test.c -o $@

$(OUT)/sync-test: sync_test.c $(SRC)/sync.c $(HEADERS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -I$(SRC) sync_test.c $(SRC)/sync.c -o $@

$(OUT)/queue-test: queue_test.c $(QUEUE_SRC) $(HEADERS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -I$(SRC) queue_test.c $(QUEUE_SRC) -o $@

$(OUT)/fault-test: fault_test.c $(SRC)/fault.c $(SRC)/power.c $(HEADERS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -DFAULT_HOST_TEST -I$(SRC) fault_test.c $(SRC)/fault.c $(SRC)/power.c -o $@

$(OUT)/mac-sim: mac_sim.c $(MAC_SRC) $(HEADERS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -O2 -I$(SRC) mac_sim.c $(MAC_SRC) -o $@

$(OUT)/radiolib/%.o: $(RADIOLIB)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=gnu++17 -O1 -w -fno-exceptions -fno-rtti -I$(RADIOLIB) -c $< -o $@

$(OUT)/radio-test: radio_test.cpp $(SRC)/radio.cpp fakes/nrf.h $(HEADERS) $(RADIOLIB_OBJ)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -Wno-unused-private-field -Ifakes -I$(SRC) -I$(RADIOLIB) radio_test.cpp $(RADIOLIB_OBJ) -o $@
