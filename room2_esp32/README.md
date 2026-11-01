# Member 3 — Room 2

This is like an automated room occupancy tracker and smart light controller that uses a PIR (Passive Infrared) motion sensor to detect human presence, automatically switches a room light on or off and broadcasts the real-time room status over a network via MQTT.

# Core Functions

- Occupancy and light control. It monitors a PIR sensor connected to pin GPIO 13. When motion is detected, it turns ON a light connected to pin GPIO 26. If no motion is detected for 10 seconds (EMPTY_AFTER_MS), the room is marked as empty, and the light turns OFF.
  
- WiFi connectivity. It automatically connects to an open wireless network named "Wokwi-GUEST"
  
- MQTT smart updates. It establishes a connection to a public test broker. Whenever the room changes state from empty to occupied or vice versa, it publishes a JSON payload ({"occupied": true} or {"occupied": false}) to a specific topic like room2/occupancy.
