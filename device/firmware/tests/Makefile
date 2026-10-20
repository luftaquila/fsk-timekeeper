CC ?= cc
CXX ?= c++
OUT := ../build/host
CFLAGS := -std=c11 -g -O1 -Wall -Wextra -Werror -Wno-pointer-to-int-cast \
	-fsanitize=undefined -fno-sanitize-recover=all
CXXFLAGS := -std=gnu++17 -g -O1 -Wall -Wextra -Werror -fno-exceptions -fno-rtti \
	-fsanitize=undefined -fno-sanitize-recover=all
SRC := ../src
RADIOLIB := ../vendor/RadioLib/src

GPS_CASES := no_pps healthy buffered_rmc buffered_rmc_wrap rx_ring_recovery frequency_scale no_fix_observation stale_rmc resume_after_silence void_transition malformed_active \
	bad_date bad_time bad_checksum hfxo_recovery missed_pps report_expiry \
	status_not_exact_a millis_wrap segments report_timeout silent_expiry glitch hfxo_stop_between_edges
SIM_CASES := sync_reports_1 sync_reports_3 sync_reports_5 sync_reports_loss sync_reports_data sync_reports_retry radio_delay_3 radio_delay_5 radio_asymmetry roundtrip_binding reauth_health join_uptime join_backoff join_window_end steady status_reports init_recovery latency latency_lbt_off lbt_jam overflow sync_gap long_gap reboot reboot_new_tag reboot_full faults \
	ev_wrap hfxo_stop session_replay late_seal late_slot late_lbt cad_failure load_2 load_5 load_noise $(addprefix admission_,0 1 2 3 4 5 6 7 8 9 10 11)
RADIO_CASES := fault_recovery rx_fault_recovery xosc_stuck rx_stuck device_errors errors_unreadable probe_frame probe_silent probe_busy_stuck begin init_recovery config_range lbt_listen begin_dead receive receive_zero_length cad_timeout cad_wrong_modem tx_missing_irq tx_busy_deadline rx_access_deadline rx_access_boundary rx_busy_deadline lbt_access_deadline lbt_busy_deadline cad_access_deadline cad_busy_deadline cad_hung_deadline

# The SX126x driver of the vendor RadioLib, built for the host without its warnings.
RADIOLIB_SRC := $(RADIOLIB)/Module.cpp $(RADIOLIB)/Hal.cpp $(RADIOLIB)/protocols/PhysicalLayer/PhysicalLayer.cpp \
	$(wildcard $(RADIOLIB)/modules/SX126x/*.cpp) $(wildcard $(RADIOLIB)/utils/*.cpp)
RADIOLIB_OBJ := $(patsubst $(RADIOLIB)/%.cpp,$(OUT)/radiolib/%.o,$(RADIOLIB_SRC))

QUEUE_SRC := $(SRC)/sensor_queue.c $(SRC)/sync.c $(SRC)/master_queue.c $(SRC)/registry.c \
	$(SRC)/proto_usb.c $(SRC)/errlog.c $(SRC)/crc32.c
MAC_SRC := $(SRC)/mac_sensor.c $(SRC)/mac_master.c $(SRC)/master_admission.c $(SRC)/protocol.c $(QUEUE_SRC)
LINES_SRC := $(SRC)/proto_usb.c $(SRC)/master_queue.c $(SRC)/errlog.c $(SRC)/crc32.c
HEADERS := $(wildcard $(SRC)/*.h)

.PHONY: test
# usb_lines.txt is what the console's firmware-lines test parses. After a line format
# change, regenerate it here: ../build/host/usb-lines > usb_lines.txt
test: $(OUT)/gps-test $(OUT)/sync-test $(OUT)/queue-test $(OUT)/fault-test $(OUT)/mac-sim $(OUT)/radio-test \
    $(OUT)/secure-test $(OUT)/protocol-test $(OUT)/capture-test $(OUT)/main-test $(OUT)/hal-test \
	$(OUT)/usb-lines
	@ulimit -c 0; failed=0; \
	for case in $(GPS_CASES); do "$(OUT)/gps-test" "$$case" || failed=1; done; \
	"$(OUT)/sync-test" || failed=1; \
	"$(OUT)/queue-test" || failed=1; \
	"$(OUT)/fault-test" || failed=1; \
	"$(OUT)/secure-test" || failed=1; \
	"$(OUT)/protocol-test" || failed=1; \
	"$(OUT)/capture-test" || failed=1; \
	"$(OUT)/main-test" || failed=1; \
	"$(OUT)/hal-test" || failed=1; \
	for case in $(SIM_CASES); do "$(OUT)/mac-sim" "$$case" || failed=1; done; \
	for case in $(RADIO_CASES); do "$(OUT)/radio-test" "$$case" || failed=1; done; \
	"$(OUT)/usb-lines" > "$(OUT)/usb_lines.txt" && diff -u usb_lines.txt "$(OUT)/usb_lines.txt" && \
		echo "PASS usb_lines" || failed=1; \
	python3 uf2_test.py || failed=1; \
	python3 metadata_test.py || failed=1; \
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

$(OUT)/secure-test: secure_test.c $(SRC)/secure.c $(SRC)/protocol.c $(HEADERS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -I$(SRC) -I../vendor/monocypher/src secure_test.c $(SRC)/protocol.c ../vendor/monocypher/src/monocypher.c -o $@

$(OUT)/protocol-test: protocol_test.c $(SRC)/protocol.c $(HEADERS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -I$(SRC) protocol_test.c $(SRC)/protocol.c -o $@

$(OUT)/capture-test: capture_test.c $(SRC)/capture.c fakes/nrf.h $(HEADERS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -Ifakes -I$(SRC) capture_test.c -o $@

# Keep the real command dispatch and clock-session handling; discard the infinite
# hardware entry loops when linking this host harness.
comma := ,
HOST_GC := $(if $(filter Darwin,$(shell uname -s)),-Wl$(comma)-dead_strip,-Wl$(comma)--gc-sections)
$(OUT)/main-test: main_test.c $(SRC)/main.c $(QUEUE_SRC) fakes/nrf.h $(HEADERS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -ffunction-sections -fdata-sections -Ifakes -I$(SRC) main_test.c $(QUEUE_SRC) $(HOST_GC) -o $@

$(OUT)/mac-sim: mac_sim.c $(SRC)/secure.c $(MAC_SRC) $(HEADERS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -O2 -I$(SRC) -I../vendor/monocypher/src mac_sim.c $(MAC_SRC) ../vendor/monocypher/src/monocypher.c -o $@

$(OUT)/usb-lines: usb_lines.c $(LINES_SRC) $(HEADERS)
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -I$(SRC) usb_lines.c $(LINES_SRC) -o $@

$(OUT)/radiolib/%.o: $(RADIOLIB)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=gnu++17 -O1 -w -fno-exceptions -fno-rtti -I$(RADIOLIB) -c $< -o $@

$(OUT)/radio-test: radio_test.cpp $(SRC)/radio.cpp fakes/nrf.h $(HEADERS) $(RADIOLIB_OBJ)
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -Wno-unused-private-field -Ifakes -I$(SRC) -I$(RADIOLIB) radio_test.cpp $(RADIOLIB_OBJ) -o $@

$(OUT)/hal-test: hal_test.cpp $(SRC)/radio_hal.cpp fakes/nrf.h $(HEADERS) $(OUT)/radiolib/Hal.o
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -Wno-unused-private-field -Ifakes -I$(SRC) -I$(RADIOLIB) hal_test.cpp $(OUT)/radiolib/Hal.o -o $@
