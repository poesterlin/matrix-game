#pragma once

/*
 * GPIO numbers are configured through Kconfig so one active sdkconfig is the
 * only source of pin assignments. Keep board-specific compile-time checks here.
 */
#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "Unsupported ESP32 target"
#endif
