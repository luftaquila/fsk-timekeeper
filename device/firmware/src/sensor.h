/* Sensor input (sensor role) — DESIGN.md §6.3.
 *
 * BA2M NPN open-collector on PIN_SENSOR_IN with an internal pull-up: idle reads
 * HIGH, an event sinks the line to GND (falling edge). Stage 1 polls the level;
 * Stage 3 captures the falling edge in hardware (GPIOTE->PPI->TIMER).
 */
#ifndef SENSOR_H
#define SENSOR_H

void sensor_init(void);

/* Returns non-zero while the sensor is asserting an event (line pulled low). */
int sensor_active(void);

#endif /* SENSOR_H */
