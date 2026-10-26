# Proposed architecture (initial draft)

## Logical terminals
1. Entrance ESP32: RFID reader, entry/exit decision logic, door indicators/actuator, local event creation.
2. Room 1 ESP32: occupancy input, light output, room-status messages.
3. Room 2 ESP32: occupancy input, light output, room-status messages.

## Shared services (implementation owner: Member 4)
- Communication transport and message framing.
- One designated SD-card writer/storage service. Do not let three terminals write the same SD card independently.
- Event identifiers/sequence numbers and duplicate handling.

## Access policy (implementation owner: Member 5)
- Schedule configuration and administrator authentication.
- Entrance asks the policy module whether access is allowed.
- Document safe emergency egress with the lecturer; do not design a system that physically traps occupants.

## Integration rule
Agree interfaces before coding. The exact communication topology and Wokwi demonstration approach remain decisions for the team meeting. Wokwi FAQ currently states multiple microcontrollers in one project are unsupported; verify the expected simulation arrangement before designing around a single multi-board diagram.
