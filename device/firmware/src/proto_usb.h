/* Master <-> PC USB-CDC line protocol (DESIGN §8). Newline-delimited ASCII,
 * one record per line, first token is a single-letter type. Replaces the legacy
 * FSK-TC "$...!" framing. All formatting is dependency-free (no float / long-long
 * printf, which newlib-nano omits) so 64-bit ticks survive intact.
 *
 *   Master -> PC:
 *     I FSK-WL <fw> <devid16hex> <freq_mhz> <sf> <bw> <ticks_per_ms>
 *     H <now_tick> <uptime_ms> <beacon_seq> <nsensors_seen>
 *     E <node> <ev_seq> <tmaster_tick> <flags> <rssi> <snr> <master_boot_id>
 *       <sensor_boot_id> <capture_seq> <end_seq> <end_tick> <sync_age_ms>
 *     D <node> <state> <offset> <skew> <rx_miss> <gap> <last_seen> <rssi> <snr>
 *       <lat_ms> <temp_c10> <batt_mv> <sec_drop> <provisioned> <sync_valid>
 *       <skew_valid> <XTAL|RC> <sync_age_ms> <capture_overflow> <event_drop>
 *       <queue_depth> <queue_busy> <usb_ref_valid> <usb_ref_ppm> <sensor_boot_id> <master_boot_id>
 *       (<node> is the sensor's 16-hex chip id; the master's own self-report D line
 *        uses the literal "0" so the PC can tell the master apart from sensors)
 *       (node 0 = master self-report: temp + charge-rail batt_mv; LoRa fields 0;
 *        sec_drop = security drops — for node 0 the master's AEAD-verify failures
 *        (forgery/wrong-key), for a sensor its replay/freshness/binding rejects;
 *        provisioned = 1 if the master holds a fleet key, else 0)
 *     T <32-hex token> <tick> <master_boot_id>
 *     A <cmd> OK
 *     X <reason>
 *   PC -> Master (K and ?ID/PING also accepted by sensors, for provisioning):
 *     ?ID | ?STATUS | PING | K <64-hex>
 *     C <0|node8hex> <ev_seq> <tmaster_tick> <master_boot_id> <sensor_boot_id> (server commit ACK)
 *     T <32-hex token> (fresh master capture request)
 */
#ifndef PROTO_USB_H
#define PROTO_USB_H

#include <stdint.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Link-state codes used in the D line and pu_emit_diag(). */
#define PU_STATE_OK    0
#define PU_STATE_STALE 1
#define PU_STATE_LOST  2

void pu_emit_identity(uint32_t devid_hi, uint32_t devid_lo);
void pu_emit_heartbeat(uint64_t now_tick, uint32_t uptime_ms, uint8_t beacon_seq, int nseen);
/* node = the sensor's 32-bit id, emitted as 8 hex chars. */
int pu_emit_event(uint32_t node_id, const event_pl_t *event, uint32_t sensor_boot_id, float rssi, float snr);
/* is_master=1 emits the literal "0" node token (master self-report); otherwise the
 * sensor's 32-bit id as 8 hex chars. */
void pu_emit_diag(uint32_t node_id, int is_master,
                  int state, int64_t offset_tick, int32_t skew_ppm,
                  uint16_t rx_miss, uint16_t beacon_gap, uint32_t last_seen_ms,
                  float rssi, float snr, uint32_t lat_ms,
                  int16_t temp_c10, uint16_t batt_mv,
                  uint32_t sec_drop, int provisioned,
                  int sync_valid, int skew_valid, int clock_xtal, uint16_t sync_age_ms,
                  uint16_t capture_overflow, uint16_t event_drop,
                  uint16_t queue_depth, uint16_t queue_overflow,
                  int usb_ref_valid, int32_t usb_ref_ppm, uint32_t sensor_boot_id, uint32_t master_boot_id);
void pu_emit_clock(const char *token, uint64_t tick, uint32_t master_boot_id);
void pu_emit_ack(const char *cmd);
void pu_emit_err(const char *reason);

/* Parsed PC->master command. */
typedef enum {
    PU_CMD_NONE = 0, /* no complete line yet */
    PU_CMD_ID,
    PU_CMD_STATUS,
    PU_CMD_PING,
    PU_CMD_SETKEY,   /* K <64-hex>: write the 32-byte fleet key (see pu_setkey) */
    PU_CMD_EVENT_ACK,/* C <node> <seq> <tick> <master_boot> <sensor_boot>: host committed this event */
    PU_CMD_CLOCK,    /* T <32-hex token>: capture a fresh arm boundary */
    PU_CMD_BAD,      /* a full line was parsed but unrecognised */
} pu_cmd_t;

/* Feed one received byte. Returns a command when a full line terminates, else
 * PU_CMD_NONE. */
pu_cmd_t pu_feed(int c);

/* The 32-byte key parsed from the most recent PU_CMD_SETKEY line. Valid only
 * immediately after pu_feed() returns PU_CMD_SETKEY. */
const uint8_t *pu_setkey(void);
uint32_t pu_event_ack_node(void);
uint16_t pu_event_ack_seq(void);
uint64_t pu_event_ack_tick(void);
uint32_t pu_event_ack_boot(void);
uint32_t pu_event_ack_sensor_boot(void);
const char *pu_clock_token(void);

#ifdef __cplusplus
}
#endif

#endif /* PROTO_USB_H */
