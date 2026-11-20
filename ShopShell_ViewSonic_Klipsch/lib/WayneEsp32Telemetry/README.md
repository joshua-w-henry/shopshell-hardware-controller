# Wayne ESP32 Telemetry — portable module (v1)

This folder is designed to be **copied unchanged** into other PlatformIO ESP32
projects under `lib/WayneEsp32Telemetry/src/`. It is not an ESPHome firmware
replacement: it runs alongside each controller's existing Arduino logic.

## What it does

- Uses a stable **Wi-Fi MAC-derived identity** `wayne_esp32_<12-hex-digits>` across IP
  changes and firmware upgrades. Never use a DHCP IP as a Home Assistant unique ID.
- Creates **one Home Assistant MQTT device** with 11 built-in diagnostic sensors,
  an independent online/offline connectivity binary sensor, plus optional
  application-specific sensors.
- Publishes **retained Home Assistant discovery**, **retained JSON state**
  every 60 seconds, and **retained online/offline availability** with an
  MQTT Last Will (QoS 1) for power/network failures.
- Reconnects at most once every 30 seconds if MQTT is unavailable. The app
  keeps running locally even when the broker is down.
- Keeps existing HTTP `/health` endpoints operational; firmware may add
  MQTT health fields there.
- Does not publish MQTT passwords, Wi-Fi passwords, or other credentials.

## MQTT topics for MAC aa:bb:cc:dd:ee:ff

```text
wayne/esp32/aabbccddeeff/state         retained JSON each minute
wayne/esp32/aabbccddeeff/availability  retained online/offline (LWT)
homeassistant/sensor/wayne_esp32_aabbccddeeff_<key>/config
homeassistant/binary_sensor/wayne_esp32_aabbccddeeff_connection/config
```

Home Assistant MQTT integration **must be connected** to RTR's Mosquitto broker
and MQTT discovery enabled. MQTT discovery defaults to `homeassistant/`.
With retained discovery configuration, the device survives HA restarts without
manually recreating entities. The connection binary sensor is deliberately
not tied to the availability topic as an availability condition, so it
displays **OFF** rather than **unavailable** on MQTT Last Will.

### Built-in sensors

IP Address, Hostname, Wi-Fi SSID, Wi-Fi RSSI (dBm), Wi-Fi Channel, Uptime (s),
Free Heap (B), Firmware Version, Last Reset Reason, Wi-Fi MAC, OTA Ready,
and Connection (binary sensor). All are diagnostic entities.
The hardware owner can add more via `Sensor` descriptors and a
`JsonObject` callback: ShopShell adds ViewSonic Last Command Result,
Klipsch Command Queue, and Audio Bootstrap Stage.

**Important:** "ViewSonic Last Command Result" indicates the last attempted
control exchange, **not** an independent ongoing network health check.

## Integration with a new firmware project

1. Copy `lib/WayneEsp32Telemetry/src/WayneEsp32Telemetry.{h,cpp}`.
2. Add `knolleary/PubSubClient@2.8` and
   `bblanchon/ArduinoJson@6.21.5` to PlatformIO `lib_deps`.
3. Put `MQTT_HOST`, `MQTT_PORT`, `MQTT_USERNAME`, and
   `MQTT_PASSWORD` in **ignored `src/secrets.h`**. The host may be the
   fixed IP `192.168.86.24` while the controller gets its IP from DHCP.
4. Instantiate `WayneEsp32Telemetry telemetry;`, call
   `telemetry.begin({friendlyName, hostname, firmware, host, port, user, pass},
   optionalWriter, optionalSensors, optionalCount)` in `setup()` after Wi-Fi
   is initialized, and call `telemetry.loop(otaReady)` in the primary loop.
5. Confirm the ESP32 boots and retains normal hardware control **without MQTT**,
   then confirm HA creates the device after broker connectivity.
6. Keep the controller's existing `/health` endpoint for emergency access
   when MQTT is unavailable.

### Boot and offline behavior

Initial broker connection can briefly block during network timeouts, but
subsequent attempts are spaced by 30 seconds and MQTT socket read timeouts
are set to 2 seconds. Do **not** depend on MQTT for safety-critical or
time-sensitive outputs. The 60-second state heartbeat is not a watchdog
and should not be used to command safety relays.

Broker availability status may lag a lost Wi-Fi signal until MQTT keepalive
expires; it is **not** an instantaneous network-link indicator.
A device abruptly replaced with a new ESP32 will have a new MAC and therefore
a new Home Assistant device; rename/archive the retired device in HA.

## Commissioning

From RTR or an MQTT client, subscribe to `wayne/esp32/+/availability` and
`wayne/esp32/+/state`. You can also inspect **Home Assistant → Settings →
Devices & services → MQTT → Devices** and search for
"ShopShell ViewSonic + Klipsch".

Use independent device identification before testing physical standby:
ShopShell's ViewSonic target `192.168.86.33:5000` is still a
candidate until verified. The S31-06 power timer and Windows shutdown
remain outside this library and are controlled by ShopShell.

## Security

Keep Mosquitto authenticated and bound to trusted LAN/VPN networks; do not
port-forward TCP 1883 or HTTP 80. `secrets.h` must stay gitignored.
This is local unencrypted MQTT appropriate for a trusted LAN, not a service
for use over the public internet. Upgrade to MQTT over TLS if you eventually
route across untrusted segments.
