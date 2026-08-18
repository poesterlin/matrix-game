#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** Initialize continuous raw pulse capture from the 433 MHz receiver. */
esp_err_t rf_receiver_init(void);

/** Return true for one second after a raw RF frame has been captured. */
bool rf_receiver_signal_recent(void);

/** Return the number of raw RF frames captured since boot. */
uint32_t rf_receiver_frame_count(void);
