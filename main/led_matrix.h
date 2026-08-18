#pragma once

#include <stdint.h>

#include "esp_err.h"

/** Initialize the LED matrix boundary without transmitting hardware data yet. */
esp_err_t led_matrix_init(void);

/** Fill the matrix and transmit one complete frame. */
esp_err_t led_matrix_fill(uint8_t red, uint8_t green, uint8_t blue);

/** Set one logical matrix coordinate using the configured serpentine layout. */
esp_err_t led_matrix_set_xy(uint16_t x, uint16_t y,
                            uint8_t red, uint8_t green, uint8_t blue);

/** Transmit the current pixel buffer. */
esp_err_t led_matrix_refresh(void);

/** Start the built-in serpentine mapping and color animation demo. */
esp_err_t led_matrix_start_demo(void);
