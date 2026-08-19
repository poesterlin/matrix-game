# Press-and-See Light Game

This project is a cause-and-effect light game for a two-year-old.

The child presses a 433 MHz transmitter button. The LED matrix responds with a
bright moving wave. The child can repeat the action as often as they want.

The game has no score, timer, or losing state. It gives the child one clear
action and one clear result.

An adult must build, power, test, and supervise the game.

## Play instructions

Use these instructions with the child:

1. Hold the button.
2. Press the button.
3. Look at the lights.
4. Press the button again.

Use short words such as “press,” “look,” and “again.” Let the child control the
pace. Do not require the child to press the button in a particular way.

The matrix shows a rainbow while it waits. A valid button signal starts a bright
expanding wave. The top-left LED turns green for one second after a valid signal.

## Game list

Use one game at a time. Start with **Button Wave** because it has the simplest
action and result.

| Game | Child action | Matrix response |
|---|---|---|
| Button Wave | Press the button | A wave moves across the matrix |
| Rainbow Press | Press the button again | The wave starts with a new color |
| Big and Small | Press once or press many times | The lights show different wave sizes |
| Find the Light | Look for the bright light | One bright light moves across the matrix |
| Stop and Go | Press to start and press again to stop | The rainbow starts or stops |
| Count the Waves | Press and count | One wave appears for each press |

Only **Button Wave** is implemented in the current firmware. The other games
are animation ideas for later versions.

## Child safety

Complete these checks before play:

1. Put the power supply where the child cannot reach it.
2. Cover the ESP32-S3, receiver, connectors, and exposed wires.
3. Secure the matrix to a wall, frame, or table.
4. Use a transmitter with a large sealed case.
5. Remove loose batteries and small parts from the play area.
6. Check the matrix and wires before each play session.
7. Stay with the child during play.

The 20% firmware brightness limit reduces LED power use. It does not make an
unsafe power supply, wire, enclosure, or battery safe.

## Current hardware profile

| Item | Value |
|---|---|
| MCU | ESP32-S3 |
| Flash | 8 MB |
| LED type | WS281x |
| LED count | 256 |
| Matrix size | 32 × 8 |
| Color order | GRB |
| Matrix layout | Vertical serpentine |
| First LED | Top-left |
| LED data | GPIO 2 |
| RF receiver | 433 MHz digital receiver |
| RF data | GPIO 7 |
| Firmware brightness limit | 20% |

The LED matrix needs a separate regulated 5 V power supply. The supply shown in
the WLED setup is rated for at least 15 A. Connect the supply ground to the
ESP32-S3 ground.

## Demo behavior

The firmware starts a rainbow animation when the board boots.

The RF receiver captures raw pulse lengths. It does not decode a remote-control
protocol yet. A valid multi-pulse frame must contain at least three RMT symbols.
The firmware accepts one frame trigger at most every 100 ms.

Each accepted RF frame causes a bright expanding wave on the matrix. The
top-left LED shows receiver state:

- Dim red: no valid RF frame arrived during the last second.
- Green: a valid RF frame arrived during the last second.

The receiver task also logs the first pulse widths and the total frame length.
Use these logs to identify the transmitter protocol later.

## Wiring

Connect the hardware as follows:

| Device pin | ESP32-S3 connection |
|---|---|
| WS281x data input | GPIO 2 |
| 433 MHz receiver data output | GPIO 7 |
| WS281x ground | Common ground |
| Receiver ground | Common ground |

Use the voltage required by the receiver module. The receiver data output must
not exceed 3.3 V. Add a level shifter or divider when the receiver produces a
5 V signal.

Many WS281x strips work more reliably with a 5 V logic-level data signal. Add a
3.3 V-to-5 V logic buffer when the matrix requires it. Keep the data wire short.

Do not power the matrix from the ESP32-S3 3.3 V output.

## Software structure

The application has three main parts:

| File | Responsibility |
|---|---|
| `main/app_main.c` | Starts the matrix, receiver, and demo |
| `main/led_matrix.c` | Maps matrix coordinates and drives WS281x pixels |
| `main/rf_receiver.c` | Captures and logs raw RF pulses |

The LED code uses Espressif's `led_strip` component with the ESP-IDF RMT TX
driver. It does not use FastLED.

The RF code uses the ESP-IDF RMT RX driver. The RMT clock runs at 1 MHz, so
captured durations are reported in microseconds. The receiver uses a 1 µs
minimum pulse filter and a 10 ms frame-end timeout.

The LED task owns the LED buffer. The RF task does not write to the LED driver.
The RF callback only places completed captures on a queue.

## Matrix mapping

The physical matrix is wired as vertical columns. Even columns run from top to
bottom. Odd columns run from bottom to top.

For a logical coordinate `(x, y)`, the firmware uses:

```text
physical_y = y                    when x is even
physical_y = height - 1 - y        when x is odd
index = x * height + physical_y
```

This mapping matches the WLED settings: first LED at the top-left, vertical
orientation, and serpentine enabled.

## Install ESP-IDF

Use ESP-IDF 6.0.x. ESP-IDF 6.0.2 is the tested version.

Load the ESP-IDF environment in each new shell:

```bash
. ~/esp/esp-idf/export.sh
idf.py --version
```

The version command must report ESP-IDF 6.0.x.

## Build

From the project directory, select the ESP32-S3 profile and build:

```bash
./switch-target.sh
idf.py build
```

The helper restores `sdkconfig.esp32s3` as the active configuration. The build
creates the firmware at `build/rf_led_matrix.bin`.

The project uses these managed components:

- `espressif/led_strip` version 3.0.3

ESP-IDF records the exact component versions in `dependencies.lock`.

## Flash the board

Find the serial device assigned to the ESP32-S3. The connected board used
`/dev/ttyACM0`.

Flash the firmware:

```bash
idf.py -p /dev/ttyACM0 flash
```

Flash and open the serial monitor in one command:

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

Exit the monitor with `Ctrl+]`.

The boot log should contain these messages:

```text
matrix=32x8 brightness=20% RF GPIO=7 LED GPIO=2
WS281x matrix ready: 32x8, GRB, GPIO 2
433 MHz raw receiver ready on GPIO 7
matrix demo started
startup complete
```

## Configuration

Open the project settings with:

```bash
idf.py menuconfig
```

Open **RF LED matrix settings**. The active values are:

| Setting | Value |
|---|---:|
| RF receiver data GPIO | 7 |
| LED matrix data GPIO | 2 |
| Matrix width | 32 |
| Matrix height | 8 |
| Brightness | 20% |
| RF capture symbols | 128 |
| Minimum pulse filter | 1 µs |
| Maximum pulse duration | 10,000 µs |

After testing a change, copy the active configuration back to
`sdkconfig.esp32s3` if you want to keep it as the project profile.

## RF capture notes

The receiver output is a demodulated digital signal. The ESP32-S3 measures the
high and low durations. It does not measure the 433 MHz carrier directly.

Begin protocol work with raw captures:

1. Open the serial monitor.
2. Press one transmitter button several times.
3. Record the pulse widths and symbol counts.
4. Compare repeated frames from the same button.
5. Identify the preamble, bit timing, data length, and repeat pattern.

Do not add protocol decoding until the raw frame pattern is known. The current
firmware only reports frames and uses them as display events.

## Power and safety checks

Complete these checks before you give the transmitter to a child:

1. Use a regulated 5 V supply for the matrix.
2. Connect all grounds together.
3. Confirm the receiver output voltage is 3.3 V or less.
4. Add a logic-level buffer when the matrix needs 5 V data logic.
5. Check the supply voltage at the far end of the matrix.
6. Check the supply current during a bright frame.
7. Keep the firmware brightness at 20%.

A board reset during RF activity can indicate a power problem. Check the 5 V
rail, ground connection, USB cable, and supply wiring before debugging the RF
decoder.

## Verification checklist

Use this checklist after wiring or firmware changes:

- The project builds for ESP32-S3.
- The boot log reports GPIO 2 for LED data.
- The boot log reports GPIO 7 for RF data.
- The boot log reports a 20% brightness limit.
- The matrix shows the rainbow animation.
- The top-left LED is dim red when the receiver is idle.
- A valid RF frame changes the top-left LED to green.
- A valid RF frame produces an expanding matrix wave.
- The serial monitor reports raw pulse widths.
- The board does not reset during repeated RF activity.

## Project files

```text
.
├── CMakeLists.txt
├── README.md
├── docs.md
├── dependencies.lock
├── main/
│   ├── CMakeLists.txt
│   ├── Kconfig.projbuild
│   ├── app_main.c
│   ├── board_config.h
│   ├── idf_component.yml
│   ├── led_matrix.c
│   ├── led_matrix.h
│   ├── rf_receiver.c
│   └── rf_receiver.h
├── sdkconfig.defaults
├── sdkconfig.esp32s3
└── switch-target.sh
```

Generated files such as `build/`, `sdkconfig`, and `managed_components/` do not
need to be committed. Commit `dependencies.lock` so later builds use the same
component versions.
