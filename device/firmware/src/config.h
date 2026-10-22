/* Pin map and firmware parameters. Port-1 capture pins need the GPIOTE PORT bit (capture.c). */
#ifndef CONFIG_H
#define CONFIG_H

#define PIN(port, n) ((port) * 32 + (n))

/* Onboard status LED. Polarity unverified; blinking works either way. */
#define PIN_LED_STATUS PIN(0, 15)

/* LoRa radio (Ra-01SH / SX1262) — SPIM + control */
#define PIN_LORA_SCK   PIN(0, 22)
#define PIN_LORA_MISO  PIN(0, 20)
#define PIN_LORA_MOSI  PIN(0, 17)
#define PIN_LORA_NSS   PIN(0, 8)
#define PIN_LORA_BUSY  PIN(1, 0)
#define PIN_LORA_DIO1  PIN(1, 6)  /* GPIOTE capture (Tx/RxDone) — port 1 */
#define PIN_LORA_NRST  PIN(0, 11) /* radio reset (NOT the board RST pin) */

/* Sensor input (BA2M NPN open-collector, falling edge) — sensor role; on the camera master the
 * line-scan camera's Strobe (Line 1, NPN, low while exposing) on the same J1 SENSE pin. */
#define PIN_SENSOR_IN  PIN(1, 13) /* GPIOTE capture (falling) — port 1 */

/* Line-scan camera line trigger (J1 pin 2 -> camera Line 2+, 3.3 V single-ended), master only */
#define PIN_CAMERA_TRIG PIN(0, 31) /* GPIOTE task output, driven by TIMER4 */

/* GPS (ATGM336H, master only); P0.09/P0.10 are NFC pins until board_init() clears UICR.NFCPINS. */
#define PIN_GPS_PPS    PIN(0, 9)  /* 1PPS in — GPIOTE capture (rising) */
#define PIN_GPS_RXD    PIN(1, 11) /* UARTE0 RXD <- GPS TXD (NMEA, 9600 8N1) */
#define PIN_GPS_TXD    PIN(0, 10) /* UARTE0 TXD -> GPS RXD (CASIC config commands) */

/* VCC (3V3 out: radio, GPS, sensor pull-up) enable gate — driven HIGH at boot */
#define PIN_EXT_POWER  PIN(0, 13)

/* LoRa parameters; channel, power and LBT are per-board settings (radio_cfg.h). */
#define LORA_BW_HZ      250000u /* SF7/BW250, symbol 512 us */
#define LORA_BW_KHZ     (LORA_BW_HZ / 1000.0f)
#define LORA_SF         7
#define LORA_CR         5       /* coding rate 4/5 */
#define LORA_SYNCWORD   0x12
#define LORA_PREAMBLE   8
#define LORA_TCXO_V     0.0f    /* Ra-01SH passive XTAL: do not enable DIO3 TCXO power */

/* Defaults when no settings are stored. KR USN rule: ch 26-32 (922.1-923.3 MHz) allow 25 mW
 * EIRP; 922.9 MHz keeps all 250 kHz inside them, off LoRaWAN defaults 922.1/922.3/922.5 MHz. */
#define RADIO_DEF_FREQ_KHZ  922900u
#define RADIO_DEF_POWER_DBM 12      /* conducted ~+12 dBm -> EIRP <= +14 dBm with ~2 dBi */
#define RADIO_DEF_LBT       1u
/* Ra-01SH rating (Ai-Thinker spec V1.1: 803-930 MHz, up to +22 dBm): its RF matching, not the
 * SX1262's 150-960 MHz, sets the band. Power floor: SX1262's (RadioLib SX1262::setOutputPower). */
#define RADIO_FREQ_MIN_KHZ  803000u
#define RADIO_FREQ_MAX_KHZ  930000u
#define RADIO_POWER_MIN_DBM (-9)
#define RADIO_POWER_MAX_DBM 22

/* RSSI LBT: listen >= 5 ms, send only at <= -65 dBm. Module RSSI accuracy is unmeasured. */
#define LBT_LISTEN_MS      5u
#define LBT_THRESHOLD_DBM  (-65)

/* Time on air in us of a len-byte packet: the SX126x datasheet formula RadioLib's getTimeOnAir()
 * uses, for an explicit header, CRC on, SF7..SF12 without LDRO and LORA_CR = 4 + CR. */
#define LORA_SYMBOL_US   ((1000000u << LORA_SF) / LORA_BW_HZ)
#define LORA_TOA_US(len) (LORA_SYMBOL_US * \
    (4u * (LORA_PREAMBLE + 8u) + 17u + 4u * LORA_CR * ((8u * (len) + 43u) / (4u * LORA_SF))) / 4u)

#define TICKS_PER_MS 16000u /* TIMER1 runs at 16 MHz */

/* Frame: a beacon every BEACON_PERIOD_MS on a TIMER1 grid; slot j starts SLOT_OFFSET_MS +
 * j * SLOT_LEN_MS after its RxDone. Normal beacons deal slots 0..SLOT_COUNT-2 to the live entries
 * in turn, the last takes join requests (mac_schedule.h); admission beacons carry a schedule. */
#define BEACON_PERIOD_MS   1000u
#define SLOT_OFFSET_MS     50u
#define SLOT_LEN_MS        120u
#define SLOT_COUNT         6u
#define SLOT_LATE_MS       10u  /* latest TX start in a slot, CAD included; later skips the slot */
#define TX_SETUP_MS        7u   /* SPI + oscillator startup budget after CAD, until preamble */
#define SLOT_GUARD_MS      5u   /* slot-end spare for the unmeasured parts of this budget */
#define BEACON_ACCESS_MS   50u /* absolute frame-start -> TX-call deadline */
#define JOIN_WINDOW_MS     30000u
#define JOIN_LIFETIME_MS   10000u
#define JOIN_MAX_OFFERS    3u
#define RX_HEADER_WAIT_MS  12u  /* a preamble without a header after this long is noise */
#define CAD_TIMEOUT_MS     10u  /* ~2.5 ms CAD (4 symbols, SF7/BW250) + oscillator startup margin */
#define CAD_DONE_MS        8u   /* a healthy CAD has reported by now, oscillator startup included */

/* Idle checkpoint/two-way refresh this many beacon periods after the last confirmed exchange,
 * in the sensor's last slot. Independent of 8-bit seq wrap. */
#define CHECKPOINT_PERIOD_BEACONS 5u
/* A checkpoint request (host `CP`) rides this many consecutive beacons. */
#define CP_REQ_BEACONS 2u

/* Master's sensor link state by time since last uplink (D lines; the console uses only this). */
#define LINK_OK_MS    12000u
#define LINK_STALE_MS 17000u
#define SENSOR_STATUS_PERIOD_MS 5000u

/* Max offset-anchor age for normal stamping: adds <= 0.56 ms at 80 ppm worst relative drift. */
#define SYNC_TTL_MS 7000u
/* Captures with a staler anchor are held in local ticks, interpolated between the anchors around
 * them if those are at most this far apart; otherwise they become losses of unknown time. */
#define SYNC_HOLD_MAX_MS 60000u

/* Skew correction only for a plausible estimate from enough samples and span, <= SKEW_MAX_EXTRAP_MS
 * past the anchor; the raw estimate is still reported so an RC fallback (~10000 ppm) shows. */
#define SKEW_CLAMP_PPM      100      /* max plausible XO drift; a real XO is < +-40 ppm */
#define SKEW_MIN_SAMPLES    4u       /* one-way beacon samples required before trusting the slope */
#define SKEW_MIN_DL_TICKS   8000000u /* min local-tick span of a skew sample (~0.5 s) */
#define SKEW_MAX_EXTRAP_MS  SYNC_TTL_MS

/* The sensor keeps a record until the beacon's cumulative ACK (a full FIFO folds new captures
 * into one loss range); the master keeps one until the host acks its line. */
#define SENSOR_FIFO_LEN        256u
#define MASTER_EVENT_QUEUE_LEN 16u
#define MASTER_USB_RETRY_MS    100u

/* Radio reset after this many consecutive SPI no-responses or failed CADs, or a sensor's
 * BEACON_LOSS_RESET_MS without a beacon of its active session (an OFFER defers it). Backoff grows
 * to RADIO_RESET_BACKOFF_MAX_MS; each accepted beacon restarts it at BEACON_LOSS_RESET_MS. */
#define RADIO_NORESP_RESET         3u
#define BEACON_LOSS_RESET_MS       10000u
#define RADIO_RESET_BACKOFF_MAX_MS 60000u
/* A failed boot radio start is retried after this, doubling up to RADIO_RESET_BACKOFF_MAX_MS. */
#define RADIO_BOOT_RETRY_MS        1000u
/* BUSY wait per command (oscillator startup, calibration); RadioLib's 1 s stalls on dead radios. */
#define RADIO_SPI_TIMEOUT_MS       50u
/* Version-string answer deadline after NRST, before begin() (~10 s blocked on a silent radio). */
#define RADIO_PROBE_MS             100u
/* BUSY this long in one radio call with no SPI transfer is a hung radio: fault + reboot. */
#define RADIO_HANG_MS              1000u

/* More than FAULT_REBOOT_MAX consecutive fault reboots halt the board (fast LED blink); the count
 * clears after FAULT_STABLE_MS of uptime or on a reset that is not a software reset. */
#define FAULT_REBOOT_MAX 3u
#define FAULT_STABLE_MS  60000u

/* PPS qualification (master): consecutive edges 1 s apart within PPS_MAX_DEV_PPM, fresh RMC
 * status A (a prerequisite, not an accuracy proof), HFXO running. ppb window <= PPS_MAX_SPAN_S;
 * wire ticks stay raw; unqualified intervals passing the same period gate give display-only ppb. */
#define PPS_MAX_DEV_PPM     200u
#define PPS_MIN_SPAN_S      8u
#define PPS_MAX_SPAN_S      64u
#define PPS_STALE_MS        2500u  /* no PPS edge for this long -> estimate invalid */
#define GPS_RMC_STALE_MS    2500u  /* no well-formed RMC A for this long -> qualification lost */
#define GPS_RMC_LAG_MAX_MS  900u   /* an RMC done within this after a PPS edge carries its UTC */
#define GPS_CFG_RESEND_MS   10000u /* $PCAS03 (volatile in the module) re-sent at most this often */

/* Line-scan camera (master): a session triggers one line every period ticks of TIMER1's clock;
 * the camera's line rate (58 kHz) bounds the period from below. Strobe anchors are sampled every
 * CAM_SAMPLE_MS; an anchor whose tick step is not its strobe step times the period within
 * CAM_JITTER_TICKS (< CAM_PERIOD_MIN / 2) ends the session (cam_track.h). CAM_JITTER_TICKS is a
 * provisional bound until the trigger-to-exposure jitter is measured. */
#define CAM_PERIOD_MIN   276u
#define CAM_PERIOD_MAX   16000u
#define CAM_SAMPLE_MS    10u
#define CAM_JITTER_TICKS 64u

/* Role is set once at boot: master if a USB host enumerates the board within this, else sensor. */
#define ROLE_SETTLE_MS   1500u

/* Cell-to-VDDH diode drop, added to the sensor's VDDH reading to estimate the cell voltage. */
#define BATT_DIODE_DROP_MV 240u

#endif /* CONFIG_H */
