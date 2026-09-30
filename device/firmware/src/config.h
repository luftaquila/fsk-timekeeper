/* Board pin map (DESIGN §4) and firmware parameters.
 *
 * SuperMini nRF52840 numbering: P0.n == n, P1.n == 32 + n. Capture pins on
 * port 1 need the PORT bit in GPIOTE PSEL (capture.c).
 */
#ifndef CONFIG_H
#define CONFIG_H

#define PIN(port, n) ((port) * 32 + (n))

/* Onboard status LED. Polarity unverified (DESIGN §10); blinking works either way. */
#define PIN_LED_STATUS PIN(0, 15)

/* LoRa radio (Ra-01SH / SX1262) — SPIM + control */
#define PIN_LORA_SCK   PIN(0, 22)
#define PIN_LORA_MISO  PIN(0, 20)
#define PIN_LORA_MOSI  PIN(0, 17)
#define PIN_LORA_NSS   PIN(0, 8)
#define PIN_LORA_BUSY  PIN(1, 0)
#define PIN_LORA_DIO1  PIN(1, 6)  /* GPIOTE capture (Tx/RxDone) — port 1 */
#define PIN_LORA_NRST  PIN(0, 11) /* radio reset (NOT the board RST pin) */

/* Sensor input (BA2M NPN open-collector, falling edge) — sensor role */
#define PIN_SENSOR_IN  PIN(1, 13) /* GPIOTE capture (falling) — port 1 */

/* GPS (ATGM336H breakout, master only; sensor boards leave U4 unpopulated).
 * P0.09/P0.10 are the NFC antenna pins until board_init() clears UICR.NFCPINS. */
#define PIN_GPS_PPS    PIN(0, 9)  /* 1PPS in — GPIOTE capture (rising) */
#define PIN_GPS_RXD    PIN(1, 11) /* UARTE0 RXD <- GPS TXD (NMEA, 9600 8N1) */
#define PIN_GPS_TXD    PIN(0, 10) /* UARTE0 TXD -> GPS RXD (CASIC config commands) */

/* VCC enable gate — driven HIGH at boot (DESIGN §8) */
#define PIN_EXT_POWER  PIN(0, 13)

/* LoRa radio parameters (DESIGN §2.1/§2.2, KR920): one channel shared by the
 * master and every sensor. */
#define LORA_FREQ_MHZ   921.3f
#define LORA_BW_KHZ     250.0f  /* SF7/BW250, symbol 512 us */
#define LORA_SF         7
#define LORA_CR         5       /* coding rate 4/5 */
#define LORA_SYNCWORD   0x12
#define LORA_POWER_DBM  12      /* conducted ~+12 dBm -> EIRP <= +14 dBm with ~2 dBi */
#define LORA_PREAMBLE   8
#define LORA_TCXO_V     1.6f    /* Ra-01SH TCXO via DIO3 */

#define TICKS_PER_MS 16000u /* TIMER1 runs at 16 MHz */

/* Fixed TxDone->RxDone air delay in ticks (DESIGN §2.9). Cancels in
 * sensor-to-sensor intervals; 0 until measured. */
#define T_AIR_REF_TICKS 0u

/* Frame timing (DESIGN §2.8): a beacon every BEACON_PERIOD_MS on a TIMER1 grid; slot k starts
 * SLOT_OFFSET_MS + k * SLOT_LEN_MS after its RxDone and holds a ~77 ms uplink. A TX let through
 * at SLOT_LATE_MS is on air <= ~7 ms later (SPI + TCXO start after a CAD) and ends by ~99 ms. */
#define BEACON_PERIOD_MS   1000u
#define SLOT_OFFSET_MS     50u
#define SLOT_LEN_MS        100u
#define SLOT_LATE_MS       15u  /* latest TX start within a slot; later -> skip this cycle */
#define BEACON_LBT_MAX_MS  50u  /* re-sense a busy channel this long, then send the beacon anyway */
#define RX_DRAIN_MAX_MS    100u /* longest wait for a reception in progress before a CAD */
#define RX_HEADER_WAIT_MS  12u  /* a preamble without a header after this long is noise */
#define CAD_TIMEOUT_MS     10u  /* a CAD takes ~2.5 ms (4 symbols at SF7/BW250), ~7.5 ms after a TCXO start */

/* Periodic checkpoint + diagnostics: in the beacon whose seq % this == slot % this. */
#define CHECKPOINT_PERIOD_BEACONS 5u
/* A checkpoint request (host `CP`) rides this many consecutive beacons. */
#define CP_REQ_BEACONS 2u

/* Sensor link state at the master from the time since its last uplink
 * (reported on D lines; the console uses only this state). */
#define LINK_OK_MS    12000u
#define LINK_STALE_MS 17000u

/* A capture may be stamped normally while its offset anchor is this fresh. At the
 * worst-case 80 ppm relative drift this contributes <= 0.56 ms. */
#define SYNC_TTL_MS 7000u
/* Captures taken while the anchor is staler are held in local ticks and
 * interpolated between the anchors around them when those are at most this far
 * apart; otherwise they become losses of unknown time. */
#define SYNC_HOLD_MAX_MS 60000u

/* Skew correction (DESIGN §2.5): only for a plausible estimate from enough samples over a long
 * enough span, at most SKEW_MAX_EXTRAP_MS past the anchor. The raw estimate is still reported
 * so an RC fallback (~10000 ppm) stays visible. */
#define SKEW_CLAMP_PPM      100      /* max plausible XO drift; a real XO is < +-40 ppm */
#define SKEW_MIN_SAMPLES    4u       /* offset samples required before trusting the slope */
#define SKEW_MIN_DL_TICKS   8000000u /* min local-tick span of a skew sample (~0.5 s) */
#define SKEW_MAX_EXTRAP_MS  SYNC_TTL_MS

/* Evidence buffers. The sensor keeps every record until the master has taken it
 * (cumulative ACK in the beacon); when the FIFO is full, new captures accumulate
 * into one loss range. The master keeps a record until the host acks its line. */
#define SENSOR_FIFO_LEN        256u
#define MASTER_EVENT_QUEUE_LEN 16u
#define MASTER_USB_RETRY_MS    100u

/* Radio recovery: reset after this many consecutive SPI no-responses or failed CADs (a radio
 * that reset itself is back in GFSK and fails the CAD start), or when a sensor hears no beacon
 * for BEACON_LOSS_RESET_MS; resets back off up to RADIO_RESET_BACKOFF_MAX_MS. */
#define RADIO_NORESP_RESET         3u
#define BEACON_LOSS_RESET_MS       10000u
#define RADIO_RESET_BACKOFF_MAX_MS 60000u
/* A radio that fails to come up at boot is retried after this, doubling up to
 * RADIO_RESET_BACKOFF_MAX_MS: one bad start must not silence a board. */
#define RADIO_BOOT_RETRY_MS        1000u
/* BUSY wait per SX1262 command. Legitimate waits are a few ms (TCXO start 5 ms,
 * calibration); RadioLib's 1 s default stalls the main loop on a dead radio. */
#define RADIO_SPI_TIMEOUT_MS       50u
/* After NRST the chip must answer its version string within this before a
 * begin() is tried; begin() on a silent radio would block ~10 s. */
#define RADIO_PROBE_MS             100u
/* One radio call spinning on BUSY this long with no SPI transfer is a hung radio
 * (two RadioLib waits have no timeout): record it as a fault and reboot. */
#define RADIO_HANG_MS              1000u

/* Fault handlers reboot; after more than FAULT_REBOOT_MAX consecutive fault
 * reboots the board halts with a fast LED blink. The count clears after
 * FAULT_STABLE_MS of uptime or on a reset that is not a software reset. */
#define FAULT_REBOOT_MAX 3u
#define FAULT_STABLE_MS  60000u

/* PPS qualification (master): consecutive edges one second apart within PPS_MAX_DEV_PPM, fresh
 * RMC status A (an expiring prerequisite, not an accuracy certificate), HFXO running. The ppb
 * window is capped at PPS_MAX_SPAN_S; wire ticks stay raw. */
#define PPS_MAX_DEV_PPM     200u
#define PPS_MIN_SPAN_S      8u
#define PPS_MAX_SPAN_S      64u
#define PPS_STALE_MS        2500u  /* no PPS edge for this long -> estimate invalid */
#define GPS_RMC_STALE_MS    2500u  /* no well-formed RMC A for this long -> qualification lost */
#define GPS_RMC_LAG_MAX_MS  900u   /* an RMC completed within this after a PPS edge carries its UTC */
#define GPS_CFG_RESEND_MS   10000u /* $PCAS03 (volatile in the module) re-sent at most this often */

/* Role is decided once at boot: master when a USB host enumerates the board
 * within ROLE_SETTLE_MS, else sensor (DESIGN §8). */
#define ROLE_SETTLE_MS   1500u

/* Diode drop between the cell and VDDH, added back to the sensor's VDDH reading
 * to estimate the cell voltage. */
#define BATT_DIODE_DROP_MV 240u

#endif /* CONFIG_H */
