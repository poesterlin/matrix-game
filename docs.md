# ESP32 Firmware Project Setup

This guide defines a standalone ESP-IDF project for a 433 MHz RF receiver and an
addressable LED matrix. It supports the Seeed Studio XIAO ESP32-C6 and XIAO ESP32-S3.

> **Current project profile:** This repository now targets the XIAO ESP32-S3 only.
> The connected matrix is 256 WS281x pixels on GPIO 2, using GRB order and a
> 32×8 vertical serpentine layout with the first pixel at the top-left. The
> firmware uses Espressif's native `led_strip` RMT driver and the ESP-IDF RMT RX
> driver for raw 433 MHz pulse capture. The older C6/profile examples below are
> retained as general reference but are not part of the active project setup.

The guide covers the project structure, build environment, target profiles, source layout,
hardware boundaries, task design, and first tests. It does not require another codebase.

## 1. System design

The firmware has two independent input and output paths:

```text
433 MHz receiver -> GPIO edges -> pulse buffer -> RF decoder task
                                                     |
                                                     v
                                                command queue
                                                     |
                                                     v
LED power supply -> LED matrix <- LED driver <- LED task
```

The GPIO interrupt records pulse timing. The RF decoder task validates a complete RF
message. It then sends a command to the LED task. The LED task owns the pixel buffer and
the LED driver.

This design prevents a slow LED update from blocking RF pulse capture.

## 2. Supported development setup

Use this setup:

- ESP-IDF 6.0.x
- ESP-IDF 6.0.2 as the known version
- C source files
- CMake through ESP-IDF
- FreeRTOS tasks and queues from ESP-IDF
- USB serial output at 115200 baud
- a separate configuration profile for each ESP32 target
- DIO flash mode at 80 MHz

The known boards have these differences:

| Board | CPU architecture | Typical flash size | Bluetooth support |
|---|---|---:|---|
| XIAO ESP32-C6 | RISC-V | 4 MB | Bluetooth LE available |
| XIAO ESP32-S3 | Xtensa | 8 MB | Bluetooth LE available, but not required |

Confirm the flash size on the exact board before you create its profile.

## 3. Install ESP-IDF

Install ESP-IDF 6.0.2 in `~/esp/esp-idf`. The normal Espressif installation creates the
export script used below.

Load the tools in each new shell:

```bash
. ~/esp/esp-idf/export.sh
idf.py --version
```

The version command must report ESP-IDF 6.0.x. Do not use an ESP-IDF 6.1 development
snapshot with these configuration profiles.

## 4. Create the project

Create this directory structure:

```text
rf-led-matrix/
├── .gitignore
├── CMakeLists.txt
├── README.md
├── sdkconfig.defaults
├── sdkconfig.esp32c6
├── sdkconfig.esp32s3
├── switch-target.sh
└── main/
    ├── CMakeLists.txt
    ├── Kconfig.projbuild
    ├── app_main.c
    ├── board_config.h
    ├── rf_receiver.c
    ├── rf_receiver.h
    ├── led_matrix.c
    └── led_matrix.h
```

ESP-IDF creates these files and directories. Do not commit them:

```gitignore
build/
.cache/
sdkconfig
sdkconfig.old
```

Commit `sdkconfig.defaults`, `sdkconfig.esp32c6`, and `sdkconfig.esp32s3`.

## 5. Add the root CMake file

Create `CMakeLists.txt` in the project root:

```cmake
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rf_led_matrix)
```

`project(rf_led_matrix)` sets the application and binary name.

## 6. Add the main component

Create `main/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS
        "app_main.c"
        "rf_receiver.c"
        "led_matrix.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_gpio
)
```

Add `esp_driver_rmt` if the RF or LED implementation uses the ESP-IDF RMT driver. Add
`esp_driver_spi` only if the selected LED driver uses SPI.

Do not add Wi-Fi, Bluetooth, JSON, TLS, HTTP, or WebSocket components unless the application
needs them.

### Managed components

Some LED drivers use the ESP-IDF Component Manager. If the selected driver uses it, create
`main/idf_component.yml` and add that driver's documented dependency. For example, use the
driver's exact package name and supported version range:

```yaml
dependencies:
  idf: '>=6.0,<6.1'
  vendor/component_name: '^1.0.0'
```

Run `idf.py reconfigure` after you change the manifest. ESP-IDF downloads the component and
creates `dependencies.lock` and `managed_components/`. Commit `dependencies.lock`. Follow
the team's policy for `managed_components/`. It can be generated again from the lock file.

## 7. Add project settings

Create `main/Kconfig.projbuild`:

```kconfig
menu "RF LED matrix settings"

config RF_INPUT_GPIO
    int "433 MHz receiver data GPIO"
    default 2

config LED_DATA_GPIO
    int "LED matrix data GPIO"
    default 3

config LED_MATRIX_WIDTH
    int "LED matrix width"
    default 16
    range 1 256

config LED_MATRIX_HEIGHT
    int "LED matrix height"
    default 16
    range 1 256

config LED_BRIGHTNESS
    int "Initial LED brightness percent"
    default 10
    range 0 100

endmenu
```

The GPIO defaults are placeholders. Replace them after you check the board pinout and the
panel interface. Do not assign a boot strap pin, USB pin, flash pin, or another reserved pin.

Open these settings with:

```bash
idf.py menuconfig
```

Application code reads the values as `CONFIG_RF_INPUT_GPIO`, `CONFIG_LED_DATA_GPIO`,
`CONFIG_LED_MATRIX_WIDTH`, `CONFIG_LED_MATRIX_HEIGHT`, and `CONFIG_LED_BRIGHTNESS`.

## 8. Add the source interfaces

Create `main/rf_receiver.h`:

```c
#pragma once

#include "esp_err.h"

esp_err_t rf_receiver_init(void);
```

Create `main/led_matrix.h`:

```c
#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t led_matrix_init(void);
esp_err_t led_matrix_fill(uint8_t red, uint8_t green, uint8_t blue);
```

Create `main/app_main.c`:

```c
#include "esp_err.h"
#include "esp_log.h"

#include "led_matrix.h"
#include "rf_receiver.h"

static const char *TAG = "rf_led_matrix";

void app_main(void) {
	ESP_LOGI(TAG, "starting firmware");
	ESP_ERROR_CHECK(led_matrix_init());
	ESP_ERROR_CHECK(rf_receiver_init());
	ESP_LOGI(TAG, "startup complete");
}
```

Create `rf_receiver.c` and `led_matrix.c` with temporary functions that return `ESP_OK`.
This lets you verify the project before you add hardware code.

`main/board_config.h` can contain target-specific pin aliases when both boards use the same
silkscreen labels but different GPIO numbers:

```c
#pragma once

#include "driver/gpio.h"

#if CONFIG_IDF_TARGET_ESP32C6
// Replace these values with checked XIAO ESP32-C6 GPIO numbers.
#define BOARD_RF_INPUT_GPIO GPIO_NUM_2
#define BOARD_LED_DATA_GPIO GPIO_NUM_3
#elif CONFIG_IDF_TARGET_ESP32S3
// Replace these values with checked XIAO ESP32-S3 GPIO numbers.
#define BOARD_RF_INPUT_GPIO GPIO_NUM_2
#define BOARD_LED_DATA_GPIO GPIO_NUM_3
#else
#error "Unsupported ESP32 target"
#endif
```

Use either Kconfig GPIO values or `board_config.h`. Do not keep two sources for the same pin
assignment. Kconfig is easier to change without editing code. A board header gives stricter
compile-time mappings.

## 9. Create the default configuration

Create `sdkconfig.defaults`:

```ini
CONFIG_ESPTOOLPY_FLASHMODE_DIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y
CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y
```

Keep this file limited to settings that apply to both boards. Set the flash size and other
chip-specific values in each target profile.

The project does not need a custom partition table for a local RF and LED application. Use
the ESP-IDF default single-application table until the application needs OTA updates or a
large data partition.

## 10. Create target profiles

Create the ESP32-C6 profile:

```bash
. ~/esp/esp-idf/export.sh
idf.py set-target esp32c6
idf.py menuconfig
cp sdkconfig sdkconfig.esp32c6
```

Set these values in `menuconfig`:

- flash size: 4 MB, or the actual board flash size
- flash mode: DIO
- flash frequency: 80 MHz
- console baud rate: 115200
- secondary console: USB Serial/JTAG
- partition table: single factory application
- Bluetooth: disabled unless the application needs it
- Wi-Fi: disabled unless the application needs it

Create the ESP32-S3 profile:

```bash
idf.py set-target esp32s3
idf.py menuconfig
cp sdkconfig sdkconfig.esp32s3
```

Use the same settings, but select 8 MB flash if that is the board's actual flash size.

Do not hand-edit generated `sdkconfig` files unless you know the exact Kconfig symbols and
their dependencies. Use `menuconfig`, test the result, then copy the active file to its
target profile.

## 11. Add the target switch script

Create `switch-target.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

TARGET="${1:-}"
if [ "$TARGET" != "esp32c6" ] && [ "$TARGET" != "esp32s3" ]; then
	echo "Usage: $0 <esp32c6|esp32s3>"
	exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SDKCONFIG_DEST="$SCRIPT_DIR/sdkconfig"
SDKCONFIG_SRC="$SCRIPT_DIR/sdkconfig.$TARGET"

if [ ! -f "$SDKCONFIG_SRC" ]; then
	echo "Error: $SDKCONFIG_SRC not found"
	exit 1
fi

CURRENT_TARGET=""
if [ -f "$SDKCONFIG_DEST" ]; then
	CURRENT_TARGET=$(grep -oP 'CONFIG_IDF_TARGET="\K[^"]+' "$SDKCONFIG_DEST" || true)
fi

if [ "$CURRENT_TARGET" != "$TARGET" ]; then
	echo "Switching from '${CURRENT_TARGET:-none}' to '$TARGET'"
	idf.py set-target "$TARGET"
	cp "$SDKCONFIG_SRC" "$SDKCONFIG_DEST"
	idf.py fullclean
else
	echo "Already targeting $TARGET"
	if ! cmp -s "$SDKCONFIG_SRC" "$SDKCONFIG_DEST"; then
		echo "Restoring $SDKCONFIG_SRC"
		cp "$SDKCONFIG_SRC" "$SDKCONFIG_DEST"
		idf.py fullclean
	fi
fi

echo "Target: $TARGET"
echo "Run: idf.py build"
```

Make it executable:

```bash
chmod +x switch-target.sh
```

The script treats each committed target profile as the source of truth. `menuconfig` changes
only the active `sdkconfig`. Copy tested changes back to the correct profile before you run
the switch script again.

## 12. Connect the 433 MHz receiver

A common 433 MHz receiver module provides power, ground, and demodulated digital data. The
ESP32 does not decode the radio carrier. It measures the high and low pulse lengths on the
data pin.

Before you connect it:

1. Find the module's supply voltage.
2. Measure or confirm its data-output voltage.
3. Make sure the ESP32 input never receives more than 3.3 V.
4. Add a level shifter or divider if the receiver produces 5 V logic.
5. Connect the receiver ground to the ESP32 ground.
6. Add the antenna specified for the receiver module.

Do not power a 5 V-only receiver from 3.3 V without checking its data sheet. Do not connect
a 5 V data output directly to the ESP32.

### RF software rules

Configure the data pin as an input. Select the pull-up or pull-down mode from the receiver's
idle level. Do not assume that every receiver has the same idle level.

Use a GPIO edge interrupt or an RMT receive channel to measure pulses. If you use a GPIO
interrupt, apply these rules:

1. Put the interrupt handler in IRAM when the driver requires it.
2. Read a monotonic timer for each edge.
3. Store only the edge level and elapsed time.
4. Send the sample to a fixed queue or ring buffer.
5. Return from the interrupt immediately.
6. Decode and log samples in a normal FreeRTOS task.

Start with raw pulse logging. Record several presses from each transmitter button. Determine
the preamble, bit timing, message length, repeat pattern, and tolerance from those samples.
Only then implement the decoder.

Reject a message when its pulse count, timing, checksum, or repeated value is invalid. Add
duplicate suppression if one button press sends the same message many times.

## 13. Identify the LED panel

"Addressable LED matrix" can describe two different interfaces. Identify the connector and
controller before you select a driver.

### Serial pixels such as WS2812, WS2812B, or SK6812

These panels usually have power, ground, data input, and sometimes data output. They use one
timed data signal. Use an ESP-IDF 6 compatible RMT LED-strip component.

Check these items:

- LED color order, such as GRB or RGB
- pixel count
- matrix width and height
- row-major or column-major order
- progressive or serpentine wiring
- protocol frequency and reset time
- RGB or RGBW pixel format

The LED driver works with a linear pixel index. Add one coordinate function that maps
`(x, y)` to that index. Keep all wiring-order logic in this function.

### HUB75 RGB panel

A HUB75 panel has multiple RGB data lines, row-address lines, clock, latch, and output-enable.
It is not a one-wire addressable panel. Use an ESP-IDF 6 compatible HUB75 DMA driver.

Check the panel scan ratio, connector pinout, width, height, color depth, and required GPIO
count. Check that the selected driver supports both the chip and ESP-IDF 6.0 before you fix
the project version.

A HUB75 panel can consume more memory, CPU time, DMA resources, and GPIO pins than a serial
pixel panel. Confirm that the RF capture method and LED driver do not need the same hardware
peripheral.

## 14. Power the LED matrix safely

Do not power the matrix from the XIAO 3.3 V pin.

Use these rules:

1. Use a separate regulated supply with the panel's required voltage.
2. Size the supply for the panel's maximum current.
3. Connect the panel supply ground to the ESP32 ground.
4. Inject power at more than one point when the panel maker requires it.
5. Use wire sized for the expected current.
6. Add the panel maker's recommended input capacitor.
7. Add a data-line resistor when the LED or driver documentation recommends it.
8. Start with a low brightness limit.

For an RGB serial panel, a conservative worst-case estimate is 60 mA per pixel at full white.
The actual value depends on the LED type, driver, brightness setting, and color. Use the panel
data sheet or measure the completed panel before you select the final supply.

Many 5 V serial LEDs do not reliably recognize a 3.3 V data-high level. Use a 3.3 V to 5 V
logic buffer when the LED input threshold requires it. Select a fast buffer intended for
digital logic. Keep the data wire short.

## 15. Define task ownership

Use these initial tasks:

| Context | Responsibility | Must not do |
|---|---|---|
| GPIO ISR or RMT callback | capture RF timing | decode, allocate memory, log, or update LEDs |
| RF decoder task | validate pulses and create commands | write directly to the LED peripheral |
| LED task | own pixels and transmit frames | wait for RF pulses in a polling loop |

Use a fixed queue between the decoder and LED tasks. Define a small command structure. For
example, it can contain a command type, color, brightness, and animation number.

Prefer fixed buffers during RF capture. Define what happens when a queue is full. For the
first version, count and log dropped messages outside the interrupt.

## 16. Build, flash, and monitor

Load ESP-IDF and select a target:

```bash
. ~/esp/esp-idf/export.sh
cd rf-led-matrix
./switch-target.sh esp32c6
idf.py build
```

Flash and open the monitor:

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

Use `./switch-target.sh esp32s3` for the ESP32-S3. Replace `/dev/ttyACM0` if the board uses
another port. Exit the ESP-IDF monitor with `Ctrl+]`.

Use these commands when necessary:

```bash
idf.py menuconfig       # edit the active target configuration
idf.py reconfigure      # refresh CMake and managed components
idf.py fullclean        # remove all generated build output
idf.py size             # show application memory use
idf.py size-components  # show memory use by component
```

## 17. Bring up the hardware

Test each boundary separately:

1. Build the temporary driver functions.
2. Flash the firmware.
3. Confirm that the monitor shows `startup complete`.
4. Implement only the LED driver.
5. Show one fixed color at low brightness.
6. Verify every matrix corner and the serpentine mapping.
7. Disconnect or disable LED output updates.
8. Implement raw RF pulse capture.
9. Log several known transmitter messages.
10. Implement and test the RF decoder.
11. Add the command queue and LED task.
12. Test RF reception during repeated LED updates.
13. Measure supply voltage and current during the brightest allowed frame.
14. Reboot the board many times and confirm consistent startup.

Do not debug RF decoding and LED timing at the same time. Prove each driver first.

## 18. Verification checklist

Before normal use, verify these items:

- `idf.py --version` reports ESP-IDF 6.0.x.
- Both target profiles build when both boards are supported.
- The configured flash size matches each physical board.
- No assigned GPIO conflicts with USB, flash, boot straps, or another driver.
- The RF data voltage stays in the ESP32 input range.
- The RF interrupt does not allocate memory or update LEDs.
- The decoder rejects malformed and incomplete messages.
- The matrix type, pixel format, and wiring order are correct.
- The LED supply can provide the measured load.
- The ESP32 and external supplies share ground.
- A logic-level buffer is present when the panel requires 5 V data.
- The brightness limit applies before the first full frame.
- RF reception continues while the matrix updates.

## 19. Record the final hardware configuration

Add this table to the project README and fill it before assembly:

| Item | Selected value |
|---|---|
| ESP32 board | |
| ESP32 target | `esp32c6` or `esp32s3` |
| Flash size | |
| RF receiver model | |
| RF receiver supply | |
| RF data voltage | |
| RF input GPIO | |
| RF protocol | |
| LED panel model | |
| LED interface | serial pixel or HUB75 |
| Matrix dimensions | |
| Pixel order | |
| LED data GPIO or HUB75 pin map | |
| LED supply voltage | |
| LED supply current rating | |
| Logic-level buffer | |
| Maximum firmware brightness | |

This table is the source of truth for pin assignments, power checks, driver selection, and
target configuration.
