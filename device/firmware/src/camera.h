/* Line-scan camera on the master. A session runs TIMER4 at period ticks of the TIMER1 clock; each
 * period drives TRIG (camera Line 2+) high for half a period. Every exposure's Strobe is latched
 * and counted by capture.c, and anchors sampled every CAM_SAMPLE_MS map camera line numbers to
 * master ticks (cam_track.h). The camera host's `V` commands (proto_usb.h) become E records of the
 * camera node (node_sender_id()) in the host queue. Main-loop context. */
#ifndef CAMERA_H
#define CAMERA_H

#include <stdint.h>

#include "master_queue.h"
#include "proto_usb.h"

/* TRIG pin, TIMER4 and the Strobe capture, without a session. Once, after capture_init(). */
void camera_init(void);

/* End the session and stop TRIG; later records get X camoff until the next V S. */
void camera_stop(void);

/* Take a Strobe anchor every CAM_SAMPLE_MS while a session runs. */
void camera_poll(uint32_t now_ms);

/* One `V` command, answered with A V OK or X <reason>; records carry master_boot_id. */
void camera_command(const pu_camera_cmd_t *cmd, mq_t *q, uint64_t master_boot_id, uint32_t now_ms);

/* The camera node's D report (master_boot_id left 0); 0 before the first session. */
int camera_diag(pu_diag_t *d, uint32_t now_ms);

#endif /* CAMERA_H */
