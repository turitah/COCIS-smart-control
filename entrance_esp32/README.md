# Entrance Terminal – Member 5

## Features implemented
- Serial management menu
- Administrator authentication (password: `cocis2026`)
- Configurable building access schedule
- System status display
- Ready for RFID integration by Member 1
- microSD event log for startup, administrator login/logout, schedule changes,
  and test-hour changes

## How to test in Wokwi
1. Open this folder in Wokwi (or copy main.c + diagram.json)
2. Start the simulation
3. Open the Serial Monitor
4. Use the menu:
   - Type `3` → login with password `cocis2026`
   - Type `5` → change the schedule
   - Type `1` → view current status
   - Type `8` → view the latest SD-card events

The microSD card uses SPI: SCK GPIO 18, MISO GPIO 19, MOSI GPIO 23, and CS
GPIO 5. It is mounted at `/sdcard`; the CSV event log is `/sdcard/events.csv`.
The card is not automatically formatted if mounting fails, so check that it
has a FAT filesystem. The system continues to run without the card and reports
that event logging is unavailable; events produced while it is unavailable
are not queued for later writing.

The current entrance firmware does not yet have RFID scan events to log. When
RFID access handling is added, call the event logger for each granted or
denied scan to include those events in the same CSV.