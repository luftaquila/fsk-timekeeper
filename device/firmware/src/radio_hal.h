/* RadioLib hardware abstraction for the nRF52840 (bare-metal, no Arduino).
 *
 * Implements RadioLibHal over: gpio.h (GPIO), raw SPIM0 (SPI), and a free-
 * running TIMER2 at 1 MHz (millis/micros). attachInterrupt/pulseIn are stubs —
 * RadioLib's blocking transmit/receive polls digitalRead(DIO1) (SX126x.cpp),
 * so they are not needed until async/HW-capture (Stage 3). Modelled on the
 * RadioLib NonArduino EspHal example.
 */
#ifndef RADIO_HAL_H
#define RADIO_HAL_H

#include <RadioLib.h>

/* GPIO mode/level constants handed to the RadioLibHal base. */
#define NRFHAL_INPUT   0x00u
#define NRFHAL_OUTPUT  0x01u
#define NRFHAL_LOW     0x00u
#define NRFHAL_HIGH    0x01u
#define NRFHAL_RISING  0x01u
#define NRFHAL_FALLING 0x02u

class NrfHal : public RadioLibHal {
  public:
    NrfHal(uint32_t sck, uint32_t miso, uint32_t mosi);

    void init() override;
    void term() override;

    void pinMode(uint32_t pin, uint32_t mode) override;
    void digitalWrite(uint32_t pin, uint32_t value) override;
    uint32_t digitalRead(uint32_t pin) override;

    void attachInterrupt(uint32_t interruptNum, void (*cb)(void), uint32_t mode) override;
    void detachInterrupt(uint32_t interruptNum) override;

    void delay(RadioLibTime_t ms) override;
    void delayMicroseconds(RadioLibTime_t us) override;
    RadioLibTime_t millis() override;
    RadioLibTime_t micros() override;
    long pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout) override;

    void spiBegin() override;
    void spiBeginTransaction() override;
    void spiTransfer(uint8_t* out, size_t len, uint8_t* in) override;
    void spiEndTransaction() override;
    void spiEnd() override;

  private:
    uint32_t _sck;
    uint32_t _miso;
    uint32_t _mosi;
};

#endif /* RADIO_HAL_H */
