/* Master <-> PC USB-CDC line protocol v6 (DESIGN §8). Newline-delimited ASCII,
 * first token = line type. Formatting is dependency-free (newlib-nano has no
 * %f / %llu) so 64-bit ticks survive intact. Node token: "0" = master, sensors
 * = 8 uppercase hex digits. crc = 8 uppercase hex digits of CRC-32/IEEE over the
 * line text before the space that precedes it.
 *
 *   Master -> PC:
 *     I FSK-WL <usb_proto> <fw> <devid16> <M|S> <radio_proto> <freq_khz> <sf> <bw> <ticks_per_ms> <reset_reason>
 *       <power_dbm> <lbt>                  (the stored radio settings, radio_cfg.h)
 *     H <now_tick> <uptime_ms> <beacon_seq> <nsensors>
 *     E <hseq> <node> <C|L|K> <ev_seq> <flags> <master_boot_id> <sensor_boot_id>
 *       <sync_age_ms> <rssi> <snr> <capture_seq> <payload> <crc>
 *       C: <count> <tick_1> .. <tick_count>  (capture_seq + i - 1 at tick_i)
 *       L: <end_seq> <tick> <end_tick>       (node 0: master timebase ended at tick)
 *       K: <tick> <cad_count> <spi_count> <tx_count> <rx_count> <clock_count> <init_count>
 *          <recovery_count> <recovery_failures> <recovery_reason> <recovery_result>
 *          (through tick the last capture is capture_seq; history frozen at creation)
 *     D <node> <OK|STALE|LOST> <skew_ppm> <rx_miss> <beacon_gap> <last_seen_ms> <rssi> <snr>
 *       <lat_ms> <temp_c10> <batt_mv> <sec_drop> <provisioned> <sync_valid> <skew_valid>
 *       <XTAL|RC> <sync_age_ms> <capture_overflow> <fifo_drop> <queue_depth> <queue_overflow>
 *       <err_flags> <ver_drop> <tx_drop> <reset_reason> <sensor_boot_id> <master_boot_id>
 *       <recovery_count> <recovery_failures> <recovery_reason> <recovery_result>
 *       D recovery fields describe the master only; sensor D uses four zeroes.
 *       Sensor history is carried by acknowledged E K lines, not periodic D lines.
 *     T <32-hex token> <tick> <master_boot_id>
 *     P <pps_tick> <utc_s> <ppb> <pps_valid> <fix> <sats> <span_s> <seg> <n>
 *       One report per captured edge; no periodic or ?STATUS replay without a new edge.
 *       ppb/span_s may describe an unqualified display-only estimate; seg gates calibration.
 *     J <node8hex> <WAIT|SYNC|READY|REMOVED> (admission status; ?STATUS refreshes it)
 *     A <cmd> OK
 *     K <64-hex|NONE>                   (reply to ?KEY over USB only)
 *     X <reason> | X <code> <count> | X fault <pc> <lr> <cause> <cfsr>
 *   PC -> master (K, R, ?KEY, ?ID and PING are also accepted by sensors):
 *     ?ID | ?STATUS | ?KEY | PING | K <64-hex> | T <32-hex> | CP
 *     JOIN | DROP <node8hex> (master only; bounded admission / release registration)
 *     R <freq_khz> <power_dbm> <0|1>   (store/apply; I reports stored target settings)
 *       A R OK: stored and RX started; X cfgfail: storage failed;
 *       I then X cfgapply: stored, radio startup failed; X cfgpending: stored, bring-up pending
 *     C <hseq> <master_boot_id> <crc>   (host stored this E line; pops the queue head on exact match)
 */
#ifndef PROTO_USB_H
#define PROTO_USB_H

#include <stdint.h>
#include "radio_health.h"
#include "protocol.h"
#include "radio_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PU_USB_PROTO 6u

/* Reset reason byte (I line, D line, sensor diagnostics). 0 = power-on/brown-out. */
#define RESET_PIN    0x01u
#define RESET_DOG    0x02u
#define RESET_SREQ   0x04u
#define RESET_LOCKUP 0x08u
#define RESET_OFF    0x10u
#define RESET_VBUS   0x20u
#define RESET_FAULT  0x40u /* the previous boot ended in a fault handler */
#define RESET_OTHER  0x80u

/* Radio faults clear on verified recovery; other bits are sticky (errlog.h). */
#define ERR_RADIO_RESET  0x0001u /* legacy history bit; v5 does not emit it */
#define ERR_CAD_TIMEOUT  0x0002u
#define ERR_SPI_TIMEOUT  0x0004u
#define ERR_TX_FAIL      0x0008u
#define ERR_RX_FAIL      0x0010u
#define ERR_HFXO_RESTART 0x0020u
#define ERR_HW_TIMEOUT   0x0040u
#define ERR_FIFO_FULL    0x0080u
#define ERR_QUEUE_FULL   0x0100u
#define ERR_USB_DROP     0x0200u
#define ERR_ACK          0x0400u
#define ERR_ID_COLLISION 0x0800u
#define ERR_RADIO_CLOCK  0x1000u /* SX1262 XOSC startup / PLL lock, not MCU HFXO */
#define ERR_RADIO_INIT   0x2000u /* device calibration / unexpected radio mode */

#define PU_STATE_OK    0
#define PU_STATE_STALE 1
#define PU_STATE_LOST  2

/* Longest E line (5 edges) is ~210 characters. */
#define PU_LINE_MAX 240

typedef struct {
    uint32_t node_id;
    int      is_master;
    int      state;            /* PU_STATE_* */
    int32_t  skew_ppm;
    uint16_t rx_miss;
    uint8_t  beacon_gap;
    uint32_t last_seen_ms;
    float    rssi, snr;
    uint32_t lat_ms;
    int16_t  temp_c10;
    uint16_t batt_mv;
    uint32_t sec_drop;
    int      provisioned;
    uint8_t  health;           /* HEALTH_* bits */
    uint16_t sync_age_ms;
    uint16_t capture_overflow;
    uint16_t fifo_drop;
    uint16_t queue_depth;
    uint16_t queue_overflow; /* refused enqueue attempts since boot, including retries */
    uint16_t err_flags;
    uint32_t ver_drop;
    uint32_t tx_drop;
    uint8_t  reset_reason;
    uint64_t sensor_boot_id;
    uint64_t master_boot_id;
    radio_recovery_diag_t recovery; /* master only; sensor history is in E K */
} pu_diag_t;

/* Control lines (I, A, X, T) that USB cannot take right now wait in a small
 * retry queue (pu_service); periodic lines (H, D, P) are dropped and counted. */
void pu_emit_identity(uint32_t devid_hi, uint32_t devid_lo, int is_master, uint8_t reset_reason,
                      const radio_cfg_t *radio);
void pu_emit_heartbeat(uint64_t now_tick, uint32_t uptime_ms, uint8_t beacon_seq, int nseen);
void pu_emit_diag(const pu_diag_t *d);
void pu_emit_clock(const char *token, uint64_t tick, uint64_t master_boot_id);
void pu_emit_pps(uint64_t pps_tick, uint32_t utc_s, int32_t ppb, int pps_valid, uint8_t fix,
                 uint8_t sats, uint8_t span_s, uint32_t seg, uint32_t n);
void pu_emit_ack(const char *cmd);
enum { PU_JOIN_WAIT, PU_JOIN_SYNC, PU_JOIN_READY, PU_JOIN_REMOVED };
void pu_emit_join(uint32_t node, int state);
/* NULL means no valid stored key. Never sent over the radio. */
void pu_emit_key(const uint8_t *key);
void pu_emit_err(const char *reason);
void pu_emit_err_count(const char *code, uint32_t count);
void pu_emit_fault(uint32_t pc, uint32_t lr, const char *cause, uint32_t cfsr);

/* Retry queued control lines; drops (and counts) one that waited over 1 s. */
void pu_service(uint32_t now_ms);
uint32_t pu_tx_drop(void);

/* E line into buf (NUL-terminated, with "\n"). Returns the length, *crc = its crc. */
int pu_format_uplink(char *buf, unsigned cap, uint32_t hseq, uint32_t node_id, uint64_t sensor_boot_id,
                     const uplink_pl_t *u, float rssi, float snr, uint32_t *crc);
int pu_format_timebase_end(char *buf, unsigned cap, uint32_t hseq, uint64_t master_boot_id,
                           uint64_t tick, uint32_t *crc);

/* Parsed PC -> master command. */
typedef enum {
    PU_CMD_NONE = 0,  /* no complete line yet */
    PU_CMD_ID,
    PU_CMD_STATUS,
    PU_CMD_PING,
    PU_CMD_SETKEY,    /* K <64-hex> (see pu_setkey) */
    PU_CMD_GETKEY,    /* ?KEY: read the flash key over USB */
    PU_CMD_RADIO,     /* R <freq_khz> <power_dbm> <0|1>, within radio_cfg_valid() (see pu_radio_cfg) */
    PU_CMD_EVENT_ACK, /* C <hseq> <master_boot_id> <crc> (see pu_ack_*) */
    PU_CMD_CLOCK,     /* T <32-hex> (see pu_clock_token) */
    PU_CMD_CHECKPOINT,/* CP */
    PU_CMD_JOIN,      /* JOIN: open a bounded admission window */
    PU_CMD_DROP,      /* DROP <node8hex>: release a master registration */
    PU_CMD_BAD,       /* a full line that is not a command */
} pu_cmd_t;

/* Feed one received byte; returns a command when a line completes. */
pu_cmd_t pu_feed(int c);

/* Arguments of the last parsed command; valid right after pu_feed() returns it. */
const uint8_t *pu_setkey(void);
const radio_cfg_t *pu_radio_cfg(void);
uint32_t pu_drop_node(void);
uint32_t pu_ack_hseq(void);
uint64_t pu_ack_boot(void);
uint32_t pu_ack_crc(void);
const char *pu_clock_token(void);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_USB_H */
