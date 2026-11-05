# CoCIS Smart Control and Monitoring System

ESP-IDF coursework starter for the CoCIS smart building pilot (three logical terminals):
- Entrance ESP32: RFID access decisions, entry/exit events, occupant tracking.
- Room 1 ESP32: occupancy-based lighting.
- Room 2 ESP32: occupancy-based lighting.
- Shared modules: communication, storage, schedule/admin interfaces.

## Important Wokwi limitation
Wokwi's FAQ currently says multiple microcontrollers in one project are unsupported. Keep the three firmware targets logically separate in this repository. Validate the lecturer's demonstration expectation and use separate Wokwi simulations (or a supported network-gateway approach) if necessary. See https://docs.wokwi.com/faq.

## Entrance subsystem
The `entrance_esp32` ESP-IDF project integrates two RC522 readers, entry/exit gate servos, access decisions, occupancy, schedule/admin configuration, SD CSV logging, and a bounded RAM event log. The host-testable policy is in `shared/access_logic`. Firmware compilation and the JSON wiring diagram are validated; card scans still need to be exercised in Wokwi or on the physical readers before claiming hardware-level validation. Occupancy resets on restart; access events are appended to `/sdcard/access_log.csv` when the SD card is available.

## Repository conventions
- `main`: integration branch; only tested changes should be merged.
- `feature/<name>`: each member's working branch.
- Open a pull request before merging.
- Every feature must include a test/demo note in the pull request.
- Do not commit build outputs, personal tokens, or private credentials.

## Folders
- `entrance_esp32/`: entrance firmware project scaffold.
- `room1_esp32/`, `room2_esp32/`: reserved for room members.
- `shared/access_logic/`: host-testable access decision and occupancy logic.
- `docs/`: requirements, architecture and interface contracts.
- `tests/`: host-side tests.

## Host test (Windows with GCC/MinGW or Linux)
From repository root:
```sh
gcc -std=c11 -Wall -Wextra -Werror shared/access_logic/access_logic.c tests/test_access_logic.c -o test_access_logic
./test_access_logic
```
On Windows, run `.\test_access_logic.exe` after compiling.
