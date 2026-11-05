# Main Gate Access Control

ESP-IDF firmware for the CoCIS main gate terminal. Two RC522 readers represent
the entry and exit sides of the gate. The terminal validates card UIDs, enforces
occupancy and schedule rules, opens the matching servo, drives the green/red LED
and buzzer, keeps a 64-event RAM history, and appends access events to an SD card
CSV log.

## Hardware

| Device | ESP32 pin |
|---|---|
| Shared SPI SCK / MISO / MOSI | 18 / 19 / 23 |
| Entry RC522 CS / RST | 5 / 21 |
| Exit RC522 CS / RST | 14 / 22 |
| microSD CS | 13 |
| Entry / exit servos | 25 / 26 |
| Entry / exit pushbuttons | 16 / 17, button to GND |
| Green / red LEDs | 32 / 33 |
| PWM buzzer | 27 |

Both RC522 readers and the microSD card share SPI3 (VSPI). The firmware initializes the
bus once, then attaches both readers and the card to it. Connect RC522 modules to
3.3V, not 5V. Servo power is taken from VIN in the Wokwi diagram.

In `wokwi-esp32-devkit-v1` diagrams, GPIO pins use board pin IDs such as `D18`, `D19`,
`D23`, `D5`, `D22`, `D14`, `D21`, and `D13`; GPIO 16/17 are exposed as `RX2`/`TX2`.
Raw IDs like `esp:18` or `esp:16` are not valid on this board and leave every device
disconnected.

## Registered cards

| UID | Name | Role |
|---|---|---|
| `DEADBEEF` | Dr. Ritah | Lecturer |
| `11223344` | John Student | Student |
| `55667788` | Mary Student | Student |
| `04112233445566` | NFC Staff | Staff |
| `C0FFEE99` | System Admin | Admin |

Both RC522 parts in `diagram.json` set the Blue Card UID to `DE:AD:BE:EF`. In
Wokwi, click a reader to open its control panel, select a card, then press Tap
or toggle Hold to present it to the reader. Selecting a card alone does not
scan it. With the panel focused, `b`, `g`, `y`, `r`, `n`, and `k` select cards,
and `t` performs a tap. If the panel is
unavailable, edit the reader's `attrs.uid` in `diagram.json` and restart the
simulation. The Red Card (`AA:BB:CC:DD`) is an unknown card.

For deterministic verification without the card UI, use the serial menu or the
ENTRY/EXIT buttons. These feed the same gate-control path as a real scan.

## Access rules

- Entry requires a registered card, an open schedule, the user not already
  inside, and available capacity.
- Exit is always allowed during an emergency or after hours so occupants are
  not trapped. An after-hours exit is recorded with reason `AFTER_HOURS_EXIT`.
- The default schedule is 07:00-18:00 and is enabled. It is stored in NVS under
  namespace `gate_cfg`.
- The software clock starts at 08:00 on each boot and can be changed by an
  administrator. The ESP32 has no RTC in this simulation.
- Scanning the admin card or entering the admin PIN opens a 120-second admin
  session. Admin card scans do not change occupancy.

## Serial menu

The serial monitor runs at 115200 baud and shows the menu at startup. Single-key
commands are processed immediately.

| Key | Action |
|---|---|
| `1` | Simulate entry for Dr. Ritah |
| `2` | Simulate exit for Dr. Ritah |
| `3` | Simulate entry for John |
| `4` | Simulate exit for John |
| `5` | Simulate an unknown card |
| `6` | Show clock, schedule, occupancy, SD, and admin status |
| `7` | Show the recent event log |
| `8` | Admin login with PIN `2580` |
| `9` | Show the menu |

The ENTRY and EXIT pushbuttons simulate a Dr. Ritah scan from the corresponding
reader. They are useful for demonstrating the gate when the RFID card control
panel is awkward to use; they are not required in the final hardware.

## Admin commands

After pressing `8` to log in, or scanning the admin card, the session stays
active for 120 seconds. Press the following single keys:

| Key | Action |
|---|---|
| `a` | Enable or disable schedule enforcement |
| `s` | Set schedule start and end as `HH:MM HH:MM` |
| `t` | Set the software clock as `HH:MM` or `HH:MM:SS` |
| `c` | Clear occupancy |
| `q` | Log out |

Invalid PINs, invalid schedule/clock entries, and every accepted configuration
change are recorded in the event log.

## SD card log

Wokwi creates a FAT16 filesystem automatically. The VS Code extension uses the
`wokwi.toml` `[[sdcard]]` entry and the `sdcard/` folder so the card is attached
and writes persist back to that folder. The firmware mounts it at `/sdcard` and
appends events to `/sdcard/access_log.csv`. If the card cannot be mounted, access
control continues and events remain available in RAM and on the serial monitor.

The CSV header is:

```text
event_id,uptime_ms,clock,terminal,uid,name,role,direction,decision,reason,source,admin,schedule,occupancy
```

`source` identifies `RFID_ENTRY`, `RFID_EXIT`, `BUTTON_ENTRY`, `BUTTON_EXIT`,
`SERIAL`, or `SERIAL_PIN`. Admin configuration entries use direction `NA` and
uid `SYSTEM`.

## Coordination

All card events from both readers are queued to one gate-control task. That task
owns occupancy, schedule, admin session, gate outputs, and the SD writer, so
state updates and shared records are serialized without races. Each event is
also printed as a single `EVENT ...` line for future terminal-to-terminal
coordination; the entrance terminal is the designated SD-record writer.

## Suggested Wokwi demo

1. Start the simulation and confirm the startup self-test and `MAIN GATE READY`.
2. Press `1` to simulate Dr. Ritah entering; the entry servo and grant feedback
   should activate.
3. Press `1` again to show `ALREADY_INSIDE`.
4. Press `5` to show an unknown-card denial.
5. Press `8`, enter PIN `2580`, then use `s` to set `08:00 17:00` and `t` to set
   `20:10`.
6. Press `1` for Dr. Ritah to show `OUTSIDE_SCHEDULE`.
7. Press `2` to demonstrate the permitted `AFTER_HOURS_EXIT`.
8. Press `6` for status and `7` for the RAM event log.
9. Check the Wokwi SD card file `/access_log.csv` for the CSV records.

## Build and run

From this directory:

```sh
pio run -j 1
```

Then start Wokwi from this project folder. `wokwi.toml` points to:

- `.pio/build/esp32dev/firmware.bin`
- `.pio/build/esp32dev/firmware.elf`

The startup self-test opens both servos, lights the green LED, and sounds the
buzzer for 700 ms. The gate is ready after `MAIN GATE READY` appears.

## Wokwi troubleshooting

- Stop and restart the simulation after rebuilding; Wokwi can keep the previous
  firmware loaded until the session is restarted.
- The RC522 readers and microSD share SPI3 (VSPI) on the IOMUX pins 18/19/23. In
  `diagram.json` these must be written as `esp:D18`, `esp:D19`, and `esp:D23`; the
  RC522 `3.3V` pin goes to `esp:3V3`. The firmware drives all three chip selects
  high before mounting the card, then initializes the readers before the SD card.
  If a reader still reports a timeout, the serial menu and buttons remain usable so
  the access logic can still be demonstrated.
- After an SD write, enable `writeback = true` (already set) to copy
  `access_log.csv` back into `sdcard/` when the simulation stops.
