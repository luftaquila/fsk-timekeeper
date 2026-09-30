/* Execute the production parser/estimator/poll/report path with injected DMA
 * bytes, PPS captures and board time. Every scenario runs in a new process, as
 * on MCU boot; only the hardware boundary is fake. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/gps.c"

static uint64_t now_ms;
static uint64_t next_pps;
static int have_pps;
static int xtal = 1;
static uint32_t hfxo_stops;
static unsigned clock_ppm;

uint32_t board_millis(void) { return (uint32_t)now_ms; }
int board_hfclk_xtal(void) { return xtal; }
uint32_t board_hfxo_stops(void) { return hfxo_stops; }
uint64_t capture_now64(void) { return now_ms * 16000u + now_ms * 16u * clock_ppm / 1000u; }
int capture_pps_get(uint64_t *tick)
{
    if (!have_pps) return 0;
    *tick = next_pps;
    have_pps = 0;
    return 1;
}

static void byte(uint8_t c)
{
    s_dma_byte = c; /* what EasyDMA writes before raising ENDRX */
    NRF_UARTE0->EVENTS_ENDRX = 1;
    UART0_UARTE0_IRQHandler();
}

static void sentence(const char *body, int corrupt)
{
    unsigned sum = 0;
    char suffix[6];
    byte('$');
    for (const char *p = body; *p; p++) { byte((uint8_t)*p); sum ^= (uint8_t)*p; }
    snprintf(suffix, sizeof(suffix), "*%02X\r\n", sum ^ (corrupt ? 1u : 0u));
    for (const char *p = suffix; *p; p++) byte((uint8_t)*p);
    gps_poll();
}

static void rmc(int active)
{
    char body[90];
    unsigned second = (unsigned)(now_ms / 1000u % 60u);
    snprintf(body, sizeof(body), "GNRMC,1200%02u.000,%c,3730.0,N,12700.0,E,0,0,280926,,,A", second, active ? 'A' : 'V');
    sentence(body, 0);
}

static void edge(unsigned second, int status)
{
    now_ms = (uint64_t)second * 1000u;
    next_pps = capture_now64();
    have_pps = 1;
    gps_poll();
    now_ms += 100u;
    if (status >= 0) rmc(status);
}

static gps_report_t report(void)
{
    gps_report_t out;
    gps_report(&out);
    return out;
}

static void warm(void)
{
    now_ms = 100;
    rmc(1);
    for (unsigned i = 1; i <= 9; i++) edge(i, 1);
    gps_report_t out = report();
    assert(out.pps_valid == 1);
    assert(out.span_s == 8);
    assert(out.ppb == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    gps_init();
    const char *scenario = argv[1];
    if (!strcmp(scenario, "healthy")) {
        now_ms = 100; rmc(1);
        for (unsigned i = 1; i <= 8; i++) edge(i, 1);
        assert(report().pps_valid == 0);
        edge(9, 1);
        assert(report().pps_valid == 1);
    } else if (!strcmp(scenario, "frequency_scale")) {
        clock_ppm = 50;
        now_ms = 100; rmc(1);
        for (unsigned i = 1; i <= 9; i++) edge(i, 1);
        gps_report_t out = report();
        assert(out.pps_valid == 1 && out.ppb == 50000 && out.span_s == 8);
        assert(out.utc_s != 0);
    } else if (!strcmp(scenario, "stale_rmc")) {
        warm();
        for (unsigned i = 10; i <= 13; i++) edge(i, -1);
        gps_report_t out = report();
        assert(out.pps_valid == 0 && out.span_s == 0 && out.ppb == 0);
    } else if (!strcmp(scenario, "resume_after_silence")) {
        warm();
        for (unsigned i = 10; i <= 13; i++) edge(i, -1);
        // No report was requested during the outage. A fresh A must not revive
        // the old window, even if the PPS never stopped.
        rmc(1);
        assert(report().pps_valid == 0);
        for (unsigned i = 14; i <= 21; i++) edge(i, 1);
        assert(report().pps_valid == 0);
        edge(22, 1);
        assert(report().pps_valid == 1);
    } else if (!strcmp(scenario, "void_transition")) {
        warm();
        edge(10, 0);
        assert(report().pps_valid == 0);
        edge(11, 1);
        assert(report().pps_valid == 0);
        for (unsigned i = 12; i <= 19; i++) edge(i, 1);
        assert(report().pps_valid == 0);
        edge(20, 1);
        assert(report().pps_valid == 1);
    } else if (!strcmp(scenario, "malformed_active")) {
        warm();
        sentence("GNRMC,12,A", 0);
        assert(report().pps_valid == 0);
    } else if (!strcmp(scenario, "bad_date")) {
        warm();
        sentence("GNRMC,120010.000,A,3730.0,N,12700.0,E,0,0,310299,,,A", 0);
        assert(report().pps_valid == 0);
    } else if (!strcmp(scenario, "bad_time")) {
        warm();
        sentence("GNRMC,126099.000,A,3730.0,N,12700.0,E,0,0,280926,,,A", 0);
        assert(report().pps_valid == 0);
    } else if (!strcmp(scenario, "bad_checksum")) {
        warm();
        for (unsigned i = 10; i <= 13; i++) {
            edge(i, -1);
            sentence("GNRMC,120010.000,A,3730.0,N,12700.0,E,0,0,280926,,,A", 1);
        }
        assert(report().pps_valid == 0);
    } else if (!strcmp(scenario, "hfxo_recovery")) {
        warm();
        xtal = 0;
        gps_poll();
        assert(report().pps_valid == 0);
        xtal = 1;
        edge(10, 1);
        assert(report().pps_valid == 0);
        for (unsigned i = 11; i <= 18; i++) edge(i, 1);
        assert(report().pps_valid == 1);
    } else if (!strcmp(scenario, "missed_pps")) {
        warm();
        now_ms = 12000;
        rmc(1);
        assert(report().pps_valid == 0);
        edge(13, 1);
        assert(report().pps_valid == 0);
    } else if (!strcmp(scenario, "report_expiry")) {
        warm();
        edge(10, -1); edge(11, -1);
        now_ms = 11599; /* A is still within the lease; PPS is fresh */
        assert(report().pps_valid == 1);
        now_ms = 11600; /* RMC age exactly 2500 ms, while PPS age is only 600 ms */
        assert(report().pps_valid == 0);
        rmc(1);
        edge(12, 1);
        assert(report().pps_valid == 0);
    } else if (!strcmp(scenario, "status_not_exact_a")) {
        warm();
        sentence("GNRMC,120010.000,AB,3730.0,N,12700.0,E,0,0,280926,,,A", 0);
        assert(report().pps_valid == 0);
    } else if (!strcmp(scenario, "millis_wrap")) {
        now_ms = (uint64_t)UINT32_MAX - 4000u;
        rmc(1);
        for (unsigned i = 0; i < 9; i++) {
            now_ms += 1000;
            next_pps = capture_now64(); have_pps = 1;
            gps_poll(); rmc(1);
        }
        assert(report().pps_valid == 1);
        now_ms += 2500;
        assert(report().pps_valid == 0);
    } else if (!strcmp(scenario, "segments")) {
        /* every qualified edge reports its segment and its second within it */
        now_ms = 100; rmc(1);
        edge(1, 1); /* the first edge of a window has no checked interval yet */
        assert(report().seg == 0);
        for (unsigned i = 2; i <= 9; i++) {
            edge(i, 1);
            gps_report_t out = report();
            assert(out.pps_tick == next_pps && out.seg == 1 && out.n == i - 2u && out.utc_s != 0);
        }
        edge(10, 0); /* V: the edge its RMC follows is not qualified, the segment ends */
        gps_report_t out = report();
        assert(out.pps_tick == next_pps && out.seg == 0 && out.utc_s == 0);
        edge(11, 1); /* A again: this transition edge is discarded too */
        assert(report().seg == 0);
        edge(12, 1); /* a new window starts */
        assert(report().seg == 0);
        edge(13, 1);
        out = report();
        assert(out.seg == 2 && out.n == 0);
        edge(14, 1);
        out = report();
        assert(out.seg == 2 && out.n == 1);
        /* a missing pulse breaks the one-second chain */
        edge(16, 1);
        assert(report().seg == 0);
        edge(17, 1);
        out = report();
        assert(out.seg == 3 && out.n == 0);
        /* a repeat without a new edge reports the same edge again */
        now_ms += 1000;
        out = report();
        assert(out.seg == 3 && out.n == 0 && out.pps_tick == next_pps);
    } else if (!strcmp(scenario, "glitch")) {
        /* A spurious pulse after an edge, before its RMC: it restarts the window and
         * takes that RMC, but neither it nor the next real edge is qualified, so no
         * one-edge segment can pin the timeline to it. */
        warm();
        now_ms = 10000;
        next_pps = capture_now64(); have_pps = 1;
        gps_poll();
        now_ms = 10050;
        uint64_t glitch = capture_now64();
        next_pps = glitch; have_pps = 1;
        gps_poll();
        now_ms = 10100;
        rmc(1);
        gps_report_t out = report();
        assert(out.pps_tick == glitch && out.seg == 0);
        edge(11, 1); /* 0.95 s after the glitch */
        assert(report().seg == 0);
        edge(12, 1);
        out = report();
        assert(out.seg == 2 && out.n == 0);
    } else if (!strcmp(scenario, "hfxo_stop_between_edges")) {
        /* an HFXO stop already over by the next edge still breaks the window */
        warm();
        hfxo_stops++;
        edge(10, 1); /* first edge of a new window */
        gps_report_t out = report();
        assert(out.pps_valid == 0 && out.seg == 0);
        edge(11, 1);
        out = report();
        assert(out.seg == 2 && out.n == 0);
    } else if (!strcmp(scenario, "report_timeout")) {
        warm();
        now_ms = 10000;
        next_pps = capture_now64(); have_pps = 1;
        gps_poll(); /* edge without its RMC */
        now_ms = 10000 + GPS_RMC_LAG_MAX_MS - 1u;
        s_last_report_ms = now_ms; /* only the edge can make a report due */
        assert(!gps_report_due());
        now_ms = 10000 + GPS_RMC_LAG_MAX_MS;
        assert(gps_report_due());
        gps_report_t out = report();
        assert(out.pps_tick == next_pps && out.seg == 1 && out.n == 8 && out.utc_s == 0);
    } else {
        fprintf(stderr, "unknown scenario: %s\n", scenario);
        return 2;
    }
    printf("PASS %s\n", scenario);
    return 0;
}
