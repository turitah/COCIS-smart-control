# Main Gate Access Control

The entrance ESP32 uses two RC522 RFID readers: one dedicated to entry and one
to exit. It identifies 4-, 7-, and 10-byte card UIDs, verifies the UID against
the registered users in `src/main.c`, and actuates only the matching gate for
an authorized movement. Duplicate entry and exit by a person who is not inside
are denied. Each scan decision is recorded in a 32-event RAM history, and the
live inside list and occupancy are available from the serial monitor. The
history and occupancy reset when the ESP32 restarts; this subsystem does not
persist records to an SD card or synchronize them with the room terminals.

| Device | ESP32 pins |
|---|---|
| Shared RFID SPI | SCK 18, MISO 19, MOSI 23 |
| Entry RC522 | SDA/CS 5, RST 22, 3.3V |
| Exit RC522 | SDA/CS 17, RST 21, 3.3V |
| Entry/exit servos | PWM 25 / 26 |
| Green/red LEDs | 32 / 33 |
| Buzzer | 27 |

Registered example cards are `DEADBEEF` (Dr. Ritah), `11223344` (John
Student), and `A1B2C3D4` (System Admin). Update `registered_users` in
`src/main.c` to change this demonstration allowlist. Connect each RC522 to
3.3V, not 5V. Both readers share SPI but have separate chip-select and reset
pins.

In Wokwi, start the simulation and drag one of the RFID cards onto the entry
reader to enter or onto the exit reader to leave. A granted scan opens the
corresponding servo gate for two seconds; denied scans do not open either gate.
The readers and cards are placed to the right of the ESP32 in the diagram.
Click and hold a card, drag it onto the rectangular RC522 reader, and release.
Use the lecturer card for entry first, then drag the same card onto the exit
reader. To enter without the RFID component, click the Wokwi serial terminal,
type `in DEADBEEF`, and press Enter; use `out DEADBEEF` to leave.
The serial monitor prints an event for every scan, including UID, direction,
decision, and milliseconds since boot. Its commands are `status` (occupancy
and people inside), `logs` (recent events), and `in UID` / `out UID` (serial
test scans). For example, `in DEADBEEF` simulates an entry scan. `help` lists
the commands.

If the diagram still shows only the ESP32, servos, LEDs, and buzzer, stop the
simulation and start it again from this project after saving `diagram.json` and
building the firmware. The running simulator keeps its old diagram and binary
until restarted. On boot, the serial monitor reports the RC522 version for
each detected reader; a `not detected` message means that reader is not in the
loaded diagram or its SPI/CS wiring is wrong.

Build from this directory with `pio run -j 1`. Wokwi reads
`.pio/build/esp32dev/firmware.bin` and `.pio/build/esp32dev/firmware.elf` as
configured in `wokwi.toml`; build successfully before starting the simulation.
