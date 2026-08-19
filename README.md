# Press-and-See Light Game

A simple light game for a two-year-old: press a 433 MHz button, watch a light wave on a 32×8 LED matrix.

Runs on a Seeed Studio XIAO ESP32-S3.

## How to Play

1. Child presses the big button.
2. Lights wave across the matrix.
3. Repeat.

No score, no losing. Just action and reaction.

Status LED (top-left): **green** = signal received; **dim red** = waiting.

## Games

| Game | What the child does | What the lights do |
|---|---|---|
| Button Wave | Press the button | A wave moves across the matrix |
| Rainbow Press | Press the button again | The wave starts with a new color |
| Big and Small | Press once or press many times | The lights show different wave sizes |
| Find the Light | Look for the bright light | One bright light moves across the matrix |
| Stop and Go | Press to start and press again to stop | The rainbow starts or stops |
| Count the Waves | Press and count | One wave appears for each press |

**Button Wave** is the current game. The other games are ideas for future
animations.

## Safety

- Adult builds, powers, and tests everything.
- Keep power supply, wires, and boards away from the child.
- Secure the LED matrix.
- Use a large transmitter with no small removable parts.
- Supervise during play.

## Wiring

| Signal | Pin |
|---|---|
| LED data | GPIO 2 |
| RF receiver data | GPIO 7 |
| LED power | External 5 V (≥15 A) |
| Ground | Common between all devices |

Use a logic-level buffer on LED data if your matrix needs 5 V logic. Verify RF output is 3.3 V safe.

## Build

Requires ESP-IDF 6.0.x.

```bash
. ~/esp/esp-idf/export.sh
./switch-target.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## Configuration

```bash
idf.py menuconfig
```

Review **RF LED matrix settings** for GPIOs, matrix size, brightness limit (default 20%), and RF capture limits.

## Hardware Record

| Item | Value |
|---|---|
| Board | Seeed Studio XIAO ESP32-S3 |
| RF receiver GPIO | 7 |
| LED data GPIO | 2 |
| Matrix | 32×8 GRB, serpentine |
| LED supply | 5 V, ≥15 A |
| Max brightness | 20% |
