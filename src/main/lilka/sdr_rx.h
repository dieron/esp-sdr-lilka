#pragma once
/* On-device receiver API for the Lilka front end. Implemented in
 * main/targets/esp32s3/receiver.c when ESP_SDR_LILKA is defined. */
#include <stdbool.h>
#include <stdint.h>

#define SDR_MAX_SAMPLES 16380u

/* Rate codes understood by the S3 dump engine. */
#define SDR_RATE_80M 0u
#define SDR_RATE_40M 1u
#define SDR_RATE_16M 6u

typedef enum { SDR_OK, SDR_ERR_ARGS, SDR_ERR_GAIN, SDR_ERR_TIMEOUT } sdr_status_t;

/* Retune to a whole-MHz receive frequency (recalibrates the RX DC tables). */
bool sdr_tune(unsigned mhz);
unsigned sdr_frequency(void);
/* manual=false selects the hardware AGC; code is a PHY gain index, not dB. */
bool sdr_gain_set(bool manual, unsigned code);
unsigned sdr_gain_min(void);
unsigned sdr_gain_max(void);
/* Analog filter: <0 automatic (PHY default), 0 widest, else approx. MHz. */
void sdr_bandwidth(int mhz);
/* One snapshot of n raw 32-bit words: bits 0-9 I, bits 10-19 -Q (both signed
 * 10-bit), bits 20-27 gain index. The buffer is reused by the next capture. */
sdr_status_t sdr_capture(unsigned n, unsigned rate_code, const uint32_t **samples, uint32_t *elapsed_us);

/* Provided by main/lilka/lilka_app.c */
void lilka_early_init(void);
void lilka_app_run(void);
