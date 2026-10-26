# Interface contract — agree before merging

## Access event
Minimum fields:
- `event_id` or monotonic sequence
- `terminal_id` (e.g. `ENTRANCE`)
- `uid` (RFID UID, not a person's password)
- `direction` (`IN` or `OUT`)
- `decision` (`GRANTED` or `DENIED`)
- `timestamp` or documented simulation-time substitute
- `reason` (e.g. `UNKNOWN_CARD`, `OUTSIDE_SCHEDULE`, `ALREADY_INSIDE`)

## Room status
- `terminal_id` (`ROOM1` or `ROOM2`)
- `occupied` (true/false)
- `light_on` (true/false)
- `sequence` or timestamp

## Decisions to agree as a group
1. Wi-Fi protocol and message format.
2. Who owns authoritative building occupancy and schedule state.
3. How messages are acknowledged/retried and deduplicated.
4. Which terminal writes to the SD card.
5. What each terminal does when communication/storage is unavailable.
6. How an administrator authenticates and how schedule changes are audited.
