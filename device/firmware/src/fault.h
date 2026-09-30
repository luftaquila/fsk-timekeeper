/* Fault handling (W6-3). HardFault, MemManage, BusFault, UsageFault, NMI and
 * every unused interrupt vector save the fault PC/LR/cause in RAM that survives
 * a system reset, then reboot. More than FAULT_REBOOT_MAX consecutive fault
 * reboots halt the board with a fast LED blink (double-tap RST reaches the
 * bootloader). No watchdog: it would survive into the bootloader. */
#ifndef FAULT_H
#define FAULT_H

#include <stdint.h>

/* First thing in main(): read and clear RESETREAS, evaluate the saved record. */
void fault_boot(void);

/* RESET_* byte (proto_usb.h) for this boot. */
uint8_t fault_reset_reason(void);

/* The fault that rebooted us, once; returns 0 when there is none to report. */
int fault_take_report(uint32_t *pc, uint32_t *lr, const char **cause, uint32_t *cfsr);

/* Call every main-loop pass: after FAULT_STABLE_MS of uptime the consecutive
 * fault count clears. */
void fault_service(uint32_t uptime_ms);

/* ---- pure logic, exposed for host tests ---------------------------------- */
typedef struct {
    uint32_t magic;
    uint32_t count;    /* consecutive fault reboots */
    uint32_t pending;  /* a fault reboot not yet reported */
    uint32_t pc, lr, cfsr, cause;
    uint32_t check;
} fault_rec_t;

#define FAULT_CAUSE_HARD  1u
#define FAULT_CAUSE_MEM   2u
#define FAULT_CAUSE_BUS   3u
#define FAULT_CAUSE_USAGE 4u
#define FAULT_CAUSE_IRQ   5u

/* Boot evaluation of a record against RESETREAS; returns the RESET_* byte and
 * sets *report when a fault reboot is to be reported. */
uint8_t fault_eval_boot(fault_rec_t *r, uint32_t resetreas, int *report);
/* Record a fault; returns 1 when the board must halt instead of rebooting. */
int fault_note(fault_rec_t *r, uint32_t pc, uint32_t lr, uint32_t cfsr, uint32_t cause);

#endif /* FAULT_H */
