/* Board pin map — verbatim from DESIGN.md §4 (정본).
 *
 * SuperMini nRF52840 GPIO numbering: P0.n == n, P1.n == 32 + n.
 * PIN(port, n) matches Nordic's NRF_GPIO_PIN_MAP(port, n).
 *
 * Single board, role chosen by firmware (node_id): master talks to the PC
 * over USB; sensor captures events and
 * reports over LoRa. Capture pins (DIO1, SENSOR) are on port 1 — GPIOTE PSEL
 * must include the port bit (handled in the capture module, Stage 3).
 */
#ifndef CONFIG_H
#define CONFIG_H

#define PIN(port, n) ((port) * 32 + (n))

/* Status — onboard LED. Polarity unverified (DESIGN.md §10 open item); blink is
 * polarity-agnostic. */
#define PIN_LED_STATUS PIN(0, 15)

/* LoRa radio (Ra-01SH / SX1262) — SPIM + control */
#define PIN_LORA_SCK   PIN(0, 22)
#define PIN_LORA_MISO  PIN(0, 20)
#define PIN_LORA_MOSI  PIN(0, 17)
#define PIN_LORA_NSS   PIN(0, 8)
#define PIN_LORA_BUSY  PIN(1, 0)
#define PIN_LORA_DIO1  PIN(1, 6)  /* GPIOTE capture (Tx/RxDone) — port 1 */
#define PIN_LORA_NRST  PIN(0, 11) /* radio reset (NOT the board RST pin) */
#define PIN_LORA_TXEN  PIN(1, 4)  /* RF switch TX enable */
#define PIN_LORA_RXEN  PIN(0, 24) /* RF switch RX enable */

/* Sensor input (BA2M NPN open-collector, falling edge) — sensor role */
#define PIN_SENSOR_IN  PIN(1, 13) /* GPIOTE capture (falling) — port 1 */

/* GPS (ATGM336H breakout, master only; sensor boards leave U4 unpopulated).
 * P0.09/P0.10 are the nRF52840 NFC antenna pins — board.c clears
 * UICR.NFCPINS.PROTECT once per board so they work as plain GPIO. */
#define PIN_GPS_PPS    PIN(0, 9)  /* 1PPS in — GPIOTE capture (rising); re-uses the SENSOR channel on the master */
#define PIN_GPS_RXD    PIN(1, 11) /* UARTE0 RXD <- GPS TXD (NMEA, 9600 8N1) */
#define PIN_GPS_TXD    PIN(0, 10) /* UARTE0 TXD -> GPS RXD (CASIC config commands) */

/* VCC enable gate — must be driven HIGH at boot (DESIGN.md §8) */
#define PIN_EXT_POWER  PIN(0, 13)

/* LoRa radio parameters — DESIGN.md §2.1/§2.2 (KR920). One master + up to 6
 * sensors share this ONE channel; collisions are handled in the MAC, not by
 * channel separation. Bring-up tunables. */
#define LORA_FREQ_MHZ   921.3f
#define LORA_BW_KHZ     250.0f  /* SF7/BW250, symbol 512us */
#define LORA_SF         7
#define LORA_CR         5       /* coding rate 4/5 */
#define LORA_SYNCWORD   0x12
#define LORA_POWER_DBM  12      /* conducted ~+12dBm -> EIRP <= +14dBm with ~2dBi */
#define LORA_PREAMBLE   8
#define LORA_TCXO_V     1.6f    /* Ra-01SH TCXO via DIO3; if begin() fails XOSC, try 1.8/3.0 */

/* Fixed TxDone->RxDone air delay in 16 MHz ticks (DESIGN §2.9 T_air_ref).
 * Bias-only constant: it cancels for sensor-to-sensor splits (both biased
 * equally) and only shifts green-to-sensor (launch) timing. Measure once on a
 * scope (master DIO1 TxDone edge -> sensor DIO1 RxDone edge at close range);
 * 0 is acceptable for split timing. */
#define T_AIR_REF_TICKS 0u

/* Channel-access timing on the one shared channel (DESIGN §2.8). The beacon at
 * the top of each 1 s frame is the sync anchor. Asynchronous EVENTs use CAD +
 * ACK + backoff. STATUS is periodic diagnostics: each sensor sends it once per
 * STATUS_PERIOD_S beacons, at a hash-chosen offset MEASURED FROM THE BEACON RxDone
 * so the transmit sits in the guarded middle of an inter-beacon gap — never on air
 * when a beacon arrives. The radio is half-duplex: while transmitting it is deaf, so
 * a STATUS placed on top of a beacon makes the sensor miss that beacon. (The earlier
 * absolute-phase scheme picked an offset anywhere in the 5 s cycle, ignorant of
 * beacon timing, so ~8% of STATUS transmits clobbered a beacon — the regression that
 * made rx_miss climb at point-blank range. The original slot-based design was beacon-
 * relative for exactly this reason; this keeps that property without slots.) The
 * beacon phase is FIXED per sensor (my_id % STATUS_PERIOD_S) so STATUS lands on the
 * same 1-of-5 beacons every period → a regular ~5 s interval (no jitter toward the
 * STALE window), self-assigned from the chip id with no slot number. Only the in-gap
 * offset is re-hashed per period, so two sensors sharing a phase still avoid a
 * permanent collision (self-healing); a CAD just before TX defers the rare same-
 * period overlap, and a lost STATUS is loss-tolerant. AEAD
 * sealing (DESIGN §2.11) makes the largest packet (sealed STATUS, 53 B) ~51 ms on
 * air at SF7/BW250/CR4-5. */
#define STATUS_PERIOD_S     5u    /* STATUS sent once per this many beacons */
#define STATUS_GAP_GUARD_MS 200u  /* earliest offset after a beacon RxDone for STATUS TX — clears the beacon air + best-effort beacon delay */
#define STATUS_GAP_SPAN_MS  500u  /* width of the hash-chosen offset window; GUARD+SPAN (=700) < ~1000 ms beacon gap → STATUS air ends well before the next beacon */

/* Checkpoint on demand (DESIGN §2.8 item 4). A beacon carries cp_req, a request id;
 * every synced sensor answers each id once with an immediate checkpoint (after its
 * own pending events are acked), so the PC can close a stopped run or a finished
 * sprint within ~1 s instead of waiting for the STATUS_PERIOD_S checkpoint. The
 * master raises a new id whenever it queues a sensor capture/loss event or the PC
 * sends `CP`, and repeats it on this many consecutive beacons so a sensor that
 * misses one beacon still hears it. */
#define CP_REQ_BEACONS 2u

/* Listen-before-talk (DESIGN §2.8) — KR920 coexistence. Before every transmit the
 * node runs a LoRa CAD (radio_lbt_clear → SX1262 scanChannel) and transmits only
 * if no preamble is detected; this is the channel check the pre-LBT firmware
 * already used for EVENT/STATUS backoff. NOTE: CAD detects LoRa activity on our
 * SF, not arbitrary energy — for a strict energy-threshold LBT (the RRA 고시 may
 * require −65 dBm energy detect) a working instantaneous-RSSI path must be
 * validated on hardware; the earlier RSSI attempt mis-read after startReceive and
 * starved the beacon, so it was reverted to CAD.
 *
 * The asynchronous traffic (EVENT/STATUS) gives up on a busy channel — EVENT
 * backs off into its next ACK retry, STATUS skips the cycle (loss-tolerant). The
 * beacon must NOT give up: it is the sync anchor with no retransmit, so a skip
 * makes every sensor miss it (rx_miss++ in lockstep) and ages links toward STALE.
 * Instead the master runs BEST-EFFORT LBT for the beacon — it senses and, if busy,
 * re-senses a few times within a bounded window long enough for an in-flight peer
 * STATUS (~51 ms at SF7/BW250) to finish, then transmits regardless. "Listen
 * before talk" still holds for every transmit; only the give-up is replaced by a
 * bounded wait. The wait is harmless to sync — sync rides the actual TxDone
 * capture, not the nominal beacon instant — and almost always zero (channel ~95%
 * idle). BEACON_LBT_TRIES × BEACON_LBT_GAP_MS sets the window (≈ one STATUS air). */
#define BEACON_LBT_TRIES  8u
#define BEACON_LBT_GAP_MS 6u

/* EVENT freshness gate (DESIGN §2.11), a defence-in-depth backstop to the
 * master-session binding (event names the master boot_id) and the per-sensor
 * replay counter. An EVENT carries an absolute master-time tick; the master
 * rejects one whose timestamp is too old (stale replay within a session) or
 * implausibly in the future. The window is asymmetric: a real event is at most a
 * few ms in the future (sensor sync error), but can be a few s in the past if
 * the link was momentarily congested, so the past bound is generous and the
 * future bound is tight. */
#define EVENT_FRESH_MS  3000u /* max age (past) accepted */
#define EVENT_FUTURE_MS 250u  /* max future skew accepted (sync error headroom) */

/* A sensor may stamp/send only while its last offset anchor is this fresh. This
 * is an accuracy gate, unlike EVENT_FRESH_MS (transport/replay age). Seven
 * seconds covers the 5 s STATUS cadence without a healthy-link dead band and,
 * at the design's worst-case 80 ppm relative drift, contributes <=0.56 ms. */
#define SYNC_TTL_MS 7000u

/* Reliable event handoff is finite-buffered and fail-closed. The sensor keeps a
 * pending event until the master ACKs it, while the master keeps an accepted
 * event until the host confirms server storage. Any overflow/expiry is sticky
 * health state and blocks later timing rather than silently continuing. */
#define SENSOR_EVENT_QUEUE_LEN 8u
#define SENSOR_EVENT_RETRY_MS  80u
#define SENSOR_EVENT_MAX_AGE_MS 2500u
#define MASTER_EVENT_QUEUE_LEN 16u
#define MASTER_USB_RETRY_MS    100u

/* USB SOF is a gross HFXO monitor only. USB 2.0 permits +-500 ppm SOF error, so
 * this value is reported but never used to calibrate ticks. */
#define USB_CLOCK_WINDOW_FRAMES 10000u
#define USB_CLOCK_MAX_GAP_FRAMES 500u

/* GPS PPS calibration (master). Consecutive PPS edges must be one nominal second
 * apart within PPS_MAX_DEV_PPM (3200 ticks): a missed pulse, a glitch or an RC-
 * fallback clock fails the gate and restarts the window. Per-edge noise is ~30 ns
 * PPS jitter ⊕ 62.5 ns capture quantisation ≈ 0.56 tick, so a span of N seconds
 * resolves ≈ 50/N ppb; the trailing window is capped at PPS_MAX_SPAN_S so the
 * estimate follows crystal temperature drift within about a minute. The estimate
 * is reported on the P line and applied by the console — wire ticks stay raw. */
#define PPS_MAX_DEV_PPM     200u
#define PPS_MIN_SPAN_S      8u
#define PPS_MAX_SPAN_S      64u
#define PPS_STALE_MS        2500u  /* no PPS edge for this long -> estimate reported invalid */
#define GPS_RMC_LAG_MAX_MS  900u   /* an RMC completed within this after a PPS edge carries that edge's UTC */
#define GPS_CFG_RESEND_MS   10000u /* $PCAS03 (GGA+RMC only; volatile in the module) re-sent at most this often */

/* Event-timestamp skew correction (DESIGN §2.5). The sensor's offset is measured at a
 * beacon (sync_ref_tick); applying the measured clock skew to the drift since that anchor
 * removes the ~drift·Δt error in an event's master timestamp (≈18 ppm ⇒ ~18 µs/s, growing
 * if beacons are missed). Guards: correct only with a validated, plausible skew
 * (|·| ≤ SKEW_CLAMP_PPM, after ≥ SKEW_MIN_SAMPLES offset samples) measured over a long
 * enough span (≥ SKEW_MIN_DL_TICKS, else the slope is noise), and extrapolate at most
 * SKEW_MAX_EXTRAP_MS past the anchor. The raw skew is still reported in STATUS (clamped
 * only to the i16 wire range) so a real fault — HFXO failure / RC fallback at ~10000 ppm
 * (board.c) — stays visible; only the event correction is gated. */
#define SKEW_CLAMP_PPM      100      /* max plausible XO drift to apply; a real XO is < ±40 ppm */
#define SKEW_MIN_SAMPLES    4u       /* offset-ring samples required before trusting the slope */
#define SKEW_MIN_DL_TICKS   8000000u /* min local-tick span for a skew sample (~0.5 s @ 16 MHz) */
#define SKEW_MAX_EXTRAP_MS  SYNC_TTL_MS /* never extrapolate beyond the valid-sync window */

/* Role (master/sensor) is decided ONCE at boot from USB host enumeration
 * (DESIGN §8): the master is the board a PC enumerates over USB-CDC. At boot we
 * pump the USB stack up to ROLE_SETTLE_MS for a host to appear; if one does this
 * board is the master, else a sensor. There is no live re-check/auto-reset — USB
 * presence can drop when the host isn't holding the port (autosuspend) and an
 * auto-reset on that rebooted the master, breaking sync. Change role = reset the
 * board. */
#define ROLE_SETTLE_MS   1500u

/* W5 Schottky drop between the cell and VDDH (DESIGN: BAT60B ~0.24 V; some boards
 * ship a silicon diode ~0.7 V). Added back to the sensor's measured VDDH to
 * estimate cell voltage. Master reports raw VDDH (charge rail), so it skips this. */
#define BATT_DIODE_DROP_MV 240u

#endif /* CONFIG_H */
