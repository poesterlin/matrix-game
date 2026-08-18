# RF LED Matrix

Standalone ESP-IDF firmware for a 433 MHz RF receiver and a WS281x LED matrix
on the Seeed Studio XIAO ESP32-S3.

## Status

The LED output is configured for the supplied WLED setup: 256 WS281x pixels,
GRB order, GPIO 2, and a 32×8 vertical serpentine matrix with the first pixel
at the top-left. Firmware brightness is limited to 20%. The RF input captures raw pulse widths from a 433 MHz receiver;
protocol decoding is intentionally separate because the transmitter protocol
has not yet been identified.

The built-in demo animates the matrix and uses the top-left pixel as the RF
indicator: dim red means idle, and green means a valid multi-pulse RF frame was
captured in the last second. Each accepted frame also triggers a rate-limited,
bright expanding wave.

The active `sdkconfig` is the single source of GPIO assignments. The committed
target profiles provide the board target and baseline flash settings.

## Requirements

- ESP-IDF 6.0.x; ESP-IDF 6.0.2 is the known version
- CMake and the ESP-IDF toolchain
- Seeed Studio XIAO ESP32-S3
- 256-pixel WS281x matrix and an external 5 V supply rated for at least 15 A
- 433 MHz receiver with a 3.3 V-safe digital data output

Load ESP-IDF in each new shell:

```bash
. ~/esp/esp-idf/export.sh
idf.py --version
```

The version must report ESP-IDF 6.0.x.

## Build

Select the target, then build:

```bash
./switch-target.sh
idf.py build
```

The project is fixed to the ESP32-S3. The committed profile is a baseline and
must be regenerated with `menuconfig` after you confirm the physical board's
flash size and RF input pin.

Flash and monitor with the board's actual serial device:

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

Exit the monitor with `Ctrl+]`.

## Configuration

Open `idf.py menuconfig` and review the **RF LED matrix settings** menu:

- RF receiver data GPIO (GPIO 7)
- LED matrix data GPIO (GPIO 2)
- matrix width and height (32×8)
- LED brightness limit
- RF pulse capture limits

The RF GPIO default is a placeholder. Confirm it against the board pinout, and
do not use boot-strapping, USB, flash, or otherwise reserved pins. GPIO 2 is
the LED data pin from the supplied WLED configuration.

## Hardware record

Fill this table before connecting hardware. It is the source of truth for pin,
power, and driver decisions.

| Item | Selected value |
|---|---|
| ESP32 board | |
| ESP32 target | `esp32s3` |
| Flash size | 8 MB profile; verify the board |
| RF receiver model | |
| RF receiver supply | |
| RF data voltage | |
| RF input GPIO | GPIO 7 |
| RF protocol | |
| LED panel model | WS281x |
| LED interface | serial pixel |
| Matrix dimensions | 32 × 8 = 256 |
| Pixel order | GRB |
| LED data GPIO | GPIO 2 |
| LED supply voltage | 5 V |
| LED supply current rating | 15 A minimum |
| Logic-level buffer | |
| Maximum firmware brightness | 20% |

## Safety and bring-up

Power the matrix from a separate regulated supply, connect its ground to the
ESP32 ground, and verify the RF data signal never exceeds 3.3 V. Many 5 V LED
panels need a 3.3 V-to-5 V logic buffer. Start with low brightness.

Bring up the system in separate steps: verify one low-brightness LED color,
verify the serpentine corners, then capture raw RF frames. The RMT callback only
queues completed captures; pulse inspection and later protocol decoding run in
the RF task.

See [docs.md](docs.md) for the complete design and verification checklist.
