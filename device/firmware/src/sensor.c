#include "sensor.h"

#include "config.h"
#include "gpio.h"

void sensor_init(void)
{
    gpio_cfg_input_pullup(PIN_SENSOR_IN);
}

int sensor_active(void)
{
    /* NPN open-collector: pulled HIGH when idle, sunk to GND on an event. */
    return gpio_read(PIN_SENSOR_IN) == 0;
}
