# Main Gate Access Control

The firmware in `src/main.c` scans the entry and exit RC522 readers, checks the
built-in sample card UIDs, and operates the matching servo, LEDs, and buzzer.
It also provides a serial test menu to simulate entry and exit scans.

| Device | ESP32 pins |
|---|---|
| Shared RFID SPI | SCK 18, MISO 19, MOSI 23 |
| Entry RC522 | CS 5, RST 22 |
| Exit RC522 | CS 17, RST 21 |
| Entry/exit servos | PWM 25 / 26 |
| Green/red LEDs | 32 / 33 |
| Buzzer | 27 |

Registered example UIDs are `DEADBEEF`, `11223344`, and `A1B2C3D4`. Change the
`registered_cards` list in `src/main.c` to use different cards. Entry cards
cannot be used to enter twice without first exiting. Recognized cards may
always exit. The microSD card is present in the diagram but is not used by this
firmware. The Wokwi diagram includes RFID cards with these sample UIDs.

Build from this directory with `pio run -j 1`. The single-job build avoids
missing-object failures during ESP-IDF's initial parallel build. The VS Code
build task uses this command too. Wokwi expects
`.pio/build/esp32dev/firmware.bin`; build successfully before starting the
simulation.
