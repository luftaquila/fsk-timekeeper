CC ?= cc
OUT := ../build/host/gps-test
CASES := healthy frequency_scale stale_rmc resume_after_silence void_transition malformed_active \
         bad_date bad_time bad_checksum hfxo_recovery missed_pps report_expiry \
         status_not_exact_a millis_wrap

.PHONY: test
test: $(OUT)
	@ulimit -c 0; failed=0; for case in $(CASES); do "$(OUT)" "$$case" || failed=1; done; exit $$failed

$(OUT): gps_test.c fakes/nrf.h ../src/gps.c ../src/gps.h ../src/config.h ../src/gpio.h ../src/board.h ../src/capture.h
	@mkdir -p ../build/host
	$(CC) -std=c11 -g -O1 -Wall -Wextra -Werror -Wno-pointer-to-int-cast \
		-fsanitize=undefined -fno-sanitize-recover=all -Ifakes gps_test.c -o $@
