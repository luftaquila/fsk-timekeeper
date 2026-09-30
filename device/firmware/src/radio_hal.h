/* RadioLib hardware abstraction for the nRF52840, bare metal: gpio.h pins, raw
 * SPIM0, and board_micros()/board_millis() for time. RadioLib polls DIO1
 * itself, so attachInterrupt/pulseIn are stubs. Main-loop context only. */
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

    /* True once after an SPIM transfer did not finish in time. */
    bool takeSpiTimeout();

  private:
    uint32_t _sck;
    uint32_t _miso;
    uint32_t _mosi;
    bool _spiTimeout;
};

#endif /* RADIO_HAL_H */
