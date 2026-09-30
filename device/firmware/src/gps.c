#include "gps.h"

#include <string.h>

#include "config.h"
#include "board.h"
#include "capture.h"
#include "gpio.h"
#include "nrf.h"

#define TICKS_PER_S       16000000u
#define PPS_MAX_DEV_TICKS ((int64_t)(TICKS_PER_S / 1000000u) * (int64_t)PPS_MAX_DEV_PPM) /* 3200 */
#define PPS_RING          (PPS_MAX_SPAN_S + 1u)
#define RX_RING           512u  /* > the module's full default sentence set per second */
#define NMEA_MAX          96u
#define NMEA_FIELDS       16u

/* ---- UARTE0 receive: one-byte EasyDMA transfers re-armed by the ENDRX->STARTRX
 * short, each byte moved to a ring from the ENDRX interrupt: the main loop can
 * block for tens of ms (beacon LBT + TX) while a byte takes 1.04 ms at 9600 bps.
 * Overflow drops the newest byte and the checksum rejects the torn sentence.
 * EasyDMA buffers must live in RAM. */
static uint8_t s_dma_byte;
static volatile uint8_t s_rx_ring[RX_RING];
static volatile uint16_t s_rx_head;
static uint16_t s_rx_tail;
static uint8_t s_tx_buf[48];
static int s_tx_started;

void UART0_UARTE0_IRQHandler(void)
{
    if (NRF_UARTE0->EVENTS_ENDRX) {
        NRF_UARTE0->EVENTS_ENDRX = 0;
        uint16_t head = s_rx_head;
        uint16_t next = (uint16_t)((head + 1u) & (RX_RING - 1u));
        if (next != s_rx_tail) {
            s_rx_ring[head] = s_dma_byte;
            __DMB();
            s_rx_head = next;
        }
    }
}

static int rx_pop(uint8_t *c)
{
    uint16_t tail = s_rx_tail;
    if (tail == s_rx_head) { return 0; }
    *c = s_rx_ring[tail];
    __DMB();
    s_rx_tail = (uint16_t)((tail + 1u) & (RX_RING - 1u));
    return 1;
}

static int tx_idle(void)
{
    return !s_tx_started || NRF_UARTE0->EVENTS_ENDTX;
}

/* Non-blocking: the previous command must have finished (~40 ms at 9600). */
static int tx_line(const char *s)
{
    unsigned n = (unsigned)strlen(s);
    if (!tx_idle() || n > sizeof(s_tx_buf)) { return 0; }
    memcpy(s_tx_buf, s, n);
    NRF_UARTE0->EVENTS_ENDTX = 0;
    NRF_UARTE0->TXD.PTR = (uint32_t)s_tx_buf;
    NRF_UARTE0->TXD.MAXCNT = n;
    NRF_UARTE0->TASKS_STARTTX = 1;
    s_tx_started = 1;
    return 1;
}

/* CASIC $PCAS03: enable GGA and RMC only (checksum 0x02 = XOR of the body). The
 * setting is volatile, so it goes out at every boot and again if the module was
 * still booting and keeps sending the default set. */
static const char PCAS03_GGA_RMC[] = "$PCAS03,1,0,0,0,1,0,0,0,0,0,,,0,0*02\r\n";
static uint32_t s_cfg_sent_ms;
static int s_cfg_due;

/* ---- PPS estimator: trailing window of gated edges ------------------------ */
static uint64_t s_pps_ring[PPS_RING];
static uint8_t s_pps_w;      /* next write index */
static uint8_t s_pps_count;  /* consecutive gated edges stored (<= PPS_RING) */
static uint64_t s_pps_last_tick;
static uint32_t s_pps_last_ms;
static int s_pps_have;

/* Qualification segments for the console's PPS timeline: a segment is a run of
 * qualified edges one second apart; it ends whenever the window restarts. An
 * edge qualifies once its interval to the previous one passed the gate, so the
 * first edge of a window (a glitch that reset it, say) is never a segment. */
static uint32_t s_seg;       /* id of the current segment, 0 before the first */
static uint32_t s_seg_n;     /* index of the newest edge in it */
typedef struct {
    uint64_t tick;
    uint32_t ms;
    uint32_t seg, n;
    uint32_t utc;
    int rmc;                 /* its RMC arrived */
} edge_t;
static edge_t s_edge;        /* newest edge, until reported */
static int s_edge_pending;
static edge_t s_rep;         /* newest reported edge (repeated by later reports) */

/* RMC A is a conservative, expiring prerequisite, not a PPS lock or accuracy
 * flag (NMEA has no timepulse-validity indication). A cached A never qualifies
 * edges after UART reception stops. The interval gate rejects outliers but
 * cannot certify an SI-second reference. */
static int s_nav_valid;
static uint32_t s_rmc_last_ms;
static uint32_t s_hfxo_stops;

static int reference_ready(uint32_t now_ms)
{
    if (s_nav_valid && (uint32_t)(now_ms - s_rmc_last_ms) >= GPS_RMC_STALE_MS) {
        s_nav_valid = 0;
    }
    uint32_t stops = board_hfxo_stops(); /* a stop already over still breaks the window */
    if (!s_nav_valid || !board_hfclk_xtal() || stops != s_hfxo_stops) {
        s_hfxo_stops = stops;
        s_pps_count = 0;
        return 0;
    }
    return 1;
}

static void pps_feed(uint64_t tick, uint32_t now_ms)
{
    int ready = reference_ready(now_ms);
    if (s_pps_have) {
        int64_t d = (int64_t)(tick - s_pps_last_tick) - (int64_t)TICKS_PER_S;
        if (d > PPS_MAX_DEV_TICKS || d < -PPS_MAX_DEV_TICKS) { s_pps_count = 0; }
    }
    s_pps_have = 1;
    s_pps_last_tick = tick;
    s_pps_last_ms = now_ms;
    s_pps_ring[s_pps_w] = tick;
    s_pps_w = (uint8_t)((s_pps_w + 1u) % PPS_RING);
    if (s_pps_count < PPS_RING) { s_pps_count++; }
    if (!ready) { s_pps_count = 0; } /* retain the edge, not a calibration sample */

    s_edge = (edge_t){ .tick = tick, .ms = now_ms };
    if (s_pps_count == 2u) { s_seg++; s_seg_n = 0; } /* the window's first checked interval */
    else if (s_pps_count > 2u) { s_seg_n++; }
    if (s_pps_count >= 2u) { s_edge.seg = s_seg; s_edge.n = s_seg_n; }
    s_edge_pending = 1;
}

/* ppb = err * 1e9 / (n * 16e6) = err * 125 / (2 n); |err| <= 64 * 3200 so no overflow. */
static int pps_estimate(int32_t *ppb, uint8_t *span_s)
{
    if (s_pps_count < PPS_MIN_SPAN_S + 1u) { return 0; }
    unsigned n = s_pps_count - 1u;
    unsigned newest = (s_pps_w + PPS_RING - 1u) % PPS_RING;
    unsigned oldest = (s_pps_w + PPS_RING - s_pps_count) % PPS_RING;
    int64_t err = (int64_t)(s_pps_ring[newest] - s_pps_ring[oldest]) - (int64_t)n * (int64_t)TICKS_PER_S;
    *ppb = (int32_t)(err * 125 / (2 * (int64_t)n));
    *span_s = (uint8_t)n;
    return 1;
}

/* ---- NMEA -------------------------------------------------------------------- */
static char s_nmea[NMEA_MAX];
static uint8_t s_nlen;
static int s_nactive;
static uint8_t s_fix, s_sats;
static uint32_t s_rmc_utc;      /* epoch of the last valid RMC (status A), 0 = none */
static int s_rmc_valid;
static uint32_t s_last_report_ms;

static int hexval(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    return -1;
}

static unsigned num2(const char *s) { return (unsigned)((s[0] - '0') * 10 + (s[1] - '0')); }

static int digits2(const char *s) { return s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9'; }

/* Validate the complete fields before accessing fixed offsets. A checksum-valid
 * but truncated/ill-formed A sentence must not renew the navigation lease. */
static int rmc_datetime_valid(const char *time, const char *date)
{
    size_t nt = strlen(time);
    if (nt < 6u || strlen(date) != 6u ||
        !digits2(time) || !digits2(time + 2) || !digits2(time + 4) ||
        !digits2(date) || !digits2(date + 2) || !digits2(date + 4)) return 0;
    if (nt > 6u) {
        if (time[6] != '.' || nt == 7u) return 0;
        for (size_t i = 7; i < nt; i++) if (time[i] < '0' || time[i] > '9') return 0;
    }
    /* A leap-second transition needs a new calibration window; it is not a
     * normal civil second that civil_to_epoch can represent unambiguously. */
    if (num2(time) > 23u || num2(time + 2) > 59u || num2(time + 4) > 59u) return 0;
    unsigned month = num2(date + 2), day = num2(date), year = 2000u + num2(date + 4);
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1u || month > 12u || day < 1u) return 0;
    unsigned limit = days[month - 1u];
    if (month == 2u && year % 4u == 0u) limit++;
    return day <= limit;
}

/* Proleptic Gregorian civil date -> Unix seconds (days_from_civil, H. Hinnant). */
static uint32_t civil_to_epoch(int y, int m, int d, unsigned hh, unsigned mm, unsigned ss)
{
    y -= m <= 2;
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int32_t days = era * 146097 + doe - 719468;
    return (uint32_t)days * 86400u + hh * 3600u + mm * 60u + ss;
}

/* RMC: 1 hhmmss.sss, 2 A|V, 9 ddmmyy. Time and date are what the module stamps
 * on the PPS edge that opened this second. */
static void parse_rmc(char *f[], unsigned nf)
{
    uint32_t now = board_millis();
    /* Expire BEFORE refreshing: an A arriving after a gap cannot revive samples
     * accumulated without fresh receiver status, even if no report was requested. */
    reference_ready(now);
    int fixed = nf >= 10 && !strcmp(f[2], "A") && rmc_datetime_valid(f[1], f[9]);
    /* Discard transition-adjacent samples conservatively: PPS phase may realign
     * on reacquisition. Do not assume every A/V change implies a phase step. */
    if (!fixed || fixed != s_nav_valid) { s_pps_count = 0; }
    s_nav_valid = fixed;
    if (!fixed) {
        s_rmc_valid = 0;
        return;
    }
    s_rmc_last_ms = now;
    s_rmc_utc = civil_to_epoch(2000 + (int)num2(f[9] + 4), (int)num2(f[9] + 2), (int)num2(f[9]),
                               num2(f[1]), num2(f[1] + 2), num2(f[1] + 4));
    s_rmc_valid = 1;
}

/* GGA: 6 fix quality, 7 satellites used. */
static void parse_gga(char *f[], unsigned nf)
{
    if (nf < 8) { return; }
    s_fix = (uint8_t)(f[6][0] >= '0' && f[6][0] <= '9' ? f[6][0] - '0' : 0);
    unsigned sv = 0;
    for (const char *p = f[7]; *p >= '0' && *p <= '9'; p++) { sv = sv * 10u + (unsigned)(*p - '0'); }
    s_sats = (uint8_t)(sv > 255u ? 255u : sv);
}

static void nmea_line(char *line, unsigned len)
{
    if (len < 9 || line[len - 3] != '*') { return; }
    uint8_t sum = 0;
    for (unsigned i = 1; i < len - 3; i++) { sum ^= (uint8_t)line[i]; }
    int hi = hexval(line[len - 2]);
    int lo = hexval(line[len - 1]);
    if (hi < 0 || lo < 0 || sum != (uint8_t)((hi << 4) | lo)) { return; }
    line[len - 3] = '\0';
    /* Split "$TTxxx,a,b,..." on commas in place; any talker (GP/GN/BD) is fine. */
    char *f[NMEA_FIELDS];
    unsigned nf = 0;
    f[nf++] = line + 1;
    for (char *p = line + 1; *p && nf < NMEA_FIELDS; p++) {
        if (*p == ',') { *p = '\0'; f[nf++] = p + 1; }
    }
    const char *type = f[0] + 2;
    if (!strncmp(type, "RMC", 3)) {
        parse_rmc(f, nf);
        /* The RMC that follows a PPS edge describes that edge's second; one that
         * ends the window also disqualifies the edge it follows. */
        uint64_t now = capture_now64();
        if (s_edge_pending && !s_edge.rmc &&
            now - s_edge.tick < (uint64_t)GPS_RMC_LAG_MAX_MS * (TICKS_PER_S / 1000u)) {
            s_edge.rmc = 1;
            s_edge.utc = s_rmc_valid ? s_rmc_utc : 0;
            if (!s_pps_count) { s_edge.seg = 0; s_edge.n = 0; }
        }
    } else if (!strncmp(type, "GGA", 3)) {
        parse_gga(f, nf);
    } else if (strncmp(type, "TXT", 3) != 0) {
        s_cfg_due = 1; /* default sentence set still on: the module booted after our PCAS03 */
    }
}

static void nmea_byte(uint8_t c)
{
    if (c == '$') { s_nactive = 1; s_nlen = 0; s_nmea[s_nlen++] = '$'; return; }
    if (!s_nactive) { return; }
    if (c == '\n' || c == '\r') {
        s_nmea[s_nlen] = '\0';
        nmea_line(s_nmea, s_nlen);
        s_nactive = 0;
        return;
    }
    if (s_nlen >= NMEA_MAX - 1u) { s_nactive = 0; return; } /* overlong: drop */
    s_nmea[s_nlen++] = (char)c;
}

/* ---- public ------------------------------------------------------------------ */
void gps_init(void)
{
    gpio_cfg_input_pullup(PIN_GPS_RXD); /* idle-high; no garbage when the GPS is unpopulated */
    gpio_set(PIN_GPS_TXD);
    gpio_cfg_output(PIN_GPS_TXD);

    NRF_UARTE0->ENABLE = UARTE_ENABLE_ENABLE_Disabled;
    NRF_UARTE0->PSEL.RXD = PIN_GPS_RXD;
    NRF_UARTE0->PSEL.TXD = PIN_GPS_TXD;
    NRF_UARTE0->PSEL.RTS = 0xFFFFFFFFu;
    NRF_UARTE0->PSEL.CTS = 0xFFFFFFFFu;
    NRF_UARTE0->BAUDRATE = UARTE_BAUDRATE_BAUDRATE_Baud9600;
    NRF_UARTE0->CONFIG = ((uint32_t)UARTE_CONFIG_HWFC_Disabled << UARTE_CONFIG_HWFC_Pos) |
                         ((uint32_t)UARTE_CONFIG_PARITY_Excluded << UARTE_CONFIG_PARITY_Pos);
    NRF_UARTE0->ENABLE = UARTE_ENABLE_ENABLE_Enabled;

    NRF_UARTE0->RXD.PTR = (uint32_t)&s_dma_byte;
    NRF_UARTE0->RXD.MAXCNT = 1;
    NRF_UARTE0->EVENTS_ENDRX = 0;
    NRF_UARTE0->SHORTS = UARTE_SHORTS_ENDRX_STARTRX_Msk;
    NRF_UARTE0->INTENSET = UARTE_INTENSET_ENDRX_Msk;
    NVIC_ClearPendingIRQ(UART0_UARTE0_IRQn);
    NVIC_SetPriority(UART0_UARTE0_IRQn, 5); /* GPIOTE capture is 3, nrfx_power 7 */
    NVIC_EnableIRQ(UART0_UARTE0_IRQn);
    NRF_UARTE0->TASKS_STARTRX = 1;

    tx_line(PCAS03_GGA_RMC);
    s_cfg_sent_ms = board_millis();
    s_last_report_ms = s_cfg_sent_ms;
    s_hfxo_stops = board_hfxo_stops();
}

void gps_poll(void)
{
    uint32_t now = board_millis();
    reference_ready(now);
    uint64_t tick;
    if (capture_pps_get(&tick)) { pps_feed(tick, now); }

    uint8_t c;
    for (unsigned i = 0; i < 128u && rx_pop(&c); i++) { nmea_byte(c); }

    if (s_cfg_due && (uint32_t)(now - s_cfg_sent_ms) >= GPS_CFG_RESEND_MS && tx_line(PCAS03_GGA_RMC)) {
        s_cfg_sent_ms = now;
        s_cfg_due = 0;
    }
}

static int edge_settled(uint32_t now)
{
    return s_edge_pending && (s_edge.rmc || (uint32_t)(now - s_edge.ms) >= GPS_RMC_LAG_MAX_MS);
}

int gps_report_due(void)
{
    uint32_t now = board_millis();
    return edge_settled(now) || (uint32_t)(now - s_last_report_ms) >= 1000u;
}

void gps_report(gps_report_t *out)
{
    uint32_t now = board_millis();
    int32_t ppb = 0;
    uint8_t span = 0;
    int ready = reference_ready(now);
    if (!s_pps_have || (uint32_t)(now - s_pps_last_ms) >= PPS_STALE_MS) s_pps_count = 0;
    if (edge_settled(now)) {
        s_rep = s_edge;
        s_edge_pending = 0;
    }
    int valid = ready && pps_estimate(&ppb, &span);
    out->pps_tick = s_rep.tick;
    out->utc_s = s_rep.utc;
    out->ppb = valid ? ppb : 0;
    out->pps_valid = (uint8_t)(valid ? 1u : 0u);
    out->fix = s_fix;
    out->sats = s_sats;
    out->span_s = valid ? span : 0;
    out->seg = s_rep.seg;
    out->n = s_rep.n;
    s_last_report_ms = now;
}
