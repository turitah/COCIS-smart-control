# CoCIS Smart Control and Monitoring System

ESP-IDF coursework starter for the CoCIS smart building pilot (three logical terminals):
- Entrance ESP32: RFID access decisions, entry/exit events, occupant tracking.
- Room 1 ESP32: occupancy-based lighting.
- Room 2 ESP32: occupancy-based lighting.
- Shared modules: communication, storage, schedule/admin interfaces.

## Important Wokwi limitation
Wokwi's FAQ currently says multiple microcontrollers in one project are unsupported. Keep the three firmware targets logically separate in this repository. Validate the lecturer's demonstration expectation and use separate Wokwi simulations (or a supported network-gateway approach) if necessary. See https://docs.wokwi.com/faq.

## Tonight's entrance milestone
The `shared/access_logic` module implements and host-tests the entrance's decision/state logic independently from the RFID reader driver. The `entrance_esp32` folder is the ESP-IDF project scaffold. The RFID hardware driver and its SPI wiring must be integrated and tested before claiming the RFID subsystem is complete.

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
