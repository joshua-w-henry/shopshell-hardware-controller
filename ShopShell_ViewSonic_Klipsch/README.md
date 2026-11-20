# ShopShell ViewSonic + Klipsch Controller v3.0

Derived from the **v2.6 hardware-controller firmware** on `feature/hdmi-switch-controller`, not the older AP5 firmware on `main`.

**Firmware:** `ShopShell_ViewSonic_Klipsch_v3.0` | **Hostname:** `esp32-shopshell` | **Network:** Shangri La (2.4 GHz) | **ViewSonic target (unconfirmed):** `192.168.86.33:5000`.

## Changes

- Removed the external Amazon HDMI-switch command implementation: GPIO21/27/32/33 button emulation, startup Dell-source reassertion, all `/hdmi/*` endpoints, and the old HDMI test console. Those four pins are no longer driven; disconnect obsolete switch wiring when practical.
- Retained **native ViewSonic IFP70 TCP control** on port 5000: power, standby, mute, selected input readback, DisplayPort and ViewSonic's own HDMI1 input route. HDMI1 here is a **panel input**, *not* the retired external switch.
- Retained the existing **Klipsch R-15PM wired NEC interface on GPIO25** and the entire `/klipsch/*` API, 70-step incremental volume-up audio initialization followed by Optical selection, and the policy not to modify ViewSonic/Windows mute automatically. Keep Windows output at a safe level because the speaker gain is deliberately driven to maximum on boot.
- Retained GPIO26 optical IR diagnostics, generic UART2/MAX3232 bridge on RX16/TX17 at 38400 baud with TCP port 2323, serial Wi-Fi diagnostics and LED, and ArduinoOTA.

## Home Assistant device monitoring (new)

The reusable `lib/WayneEsp32Telemetry` module publishes MAC-identified MQTT telemetry every 60 seconds to RTR Mosquitto (`192.168.86.24:1883`), with retained Home Assistant discovery, retained online/offline LWT, Wi-Fi RSSI/IP/hostname/SSID, uptime, firmware, reset reason, free heap, OTA state, plus last ViewSonic command result, Klipsch queue, and audio bootstrap state. It never commands display power or audio through MQTT. **Set the actual MQTT username and password locally in `src/secrets.h` before flashing.** See [Wayne telemetry guide](lib/WayneEsp32Telemetry/README.md).

## Addresses and USB flash

The ViewSonic and ESP32 are **different IPs**. `192.168.86.33` accepted a TCP connection on port 5000, but that alone does not prove identity. Reserve its DHCP address after verification. The ESP32 will get its *own* IP on Shangri La; do not assume unlabeled `192.168.86.56` belongs to it.

1. Open **`ShopShell_ViewSonic_Klipsch/`** (the folder containing `platformio.ini`) in VS Code.
2. Copy `src/secrets.h.example` to `src/secrets.h`; enter the actual **Shangri La** password and RTR **MQTT username/password** locally. Never commit secrets.h. If copying an old secrets.h, change the SSID.
3. Connect USB and flash `esp32dev` in PlatformIO, or run `pio run -e esp32dev -t upload`.
4. Serial Monitor at **115200**: confirm v3.0 firmware ID, Wi-Fi connected, ESP32 assigned IP, and `Arduino OTA ready`. A disconnected unit logs Wi-Fi status every 10 seconds.
5. Run `powershell -File tools/ReadOnly-Commission.ps1 -ControllerIp <actual-ESP32-IP>` or open `http://<actual-ESP32-IP>/health`. Verify the ViewSonic target in `/viewsonic/status` and read-only `/viewsonic/input/status`. Don't send power/standby commands until .86.33 is confirmed to be the right screen.
6. OTA later: `pio run -e esp32dev_ota -t upload`. Set `upload_port` to the confirmed ESP32 IP if `esp32-shopshell.local` does not resolve.

## API contract

Read-only GET: `/health`, `/viewsonic/status`, `/viewsonic/input/status`, `/system/startup/status`, `/klipsch/status`, `/klipsch/commands`, `/ir/last`.

ViewSonic POST: `/viewsonic/power/on`, `/viewsonic/standby`, `/viewsonic/mute/on`, `/viewsonic/mute/off`, `/viewsonic/input/dp`, `/viewsonic/input/hdmi1`, `/system/shutdown`. The latter **only places the panel in standby**; the Windows and S31 AC-cutoff sequence runs in ShopShell.

Klipsch: `/klipsch/remote` browser interface, `GET/POST /klipsch/<command>` for speaker controls; `POST /system/audio/bootstrap` to rerun audio setup.

**Removed:** all `/hdmi/*` routes. WayneShell/ShopShell's Jackie PS4/Apple TV buttons currently depend on those routes and will require removal or remapping separately.

**STOP before testing ShopShell OFF:** desktop ShopShell still hardcodes ESP32 `192.168.87.42` and Tasmota S31 `192.168.87.45`. Both addresses must be independently rediscovered and updated in ShopShell (separate repo) before supervised shutdown. Flashing this ESP32 alone does not fix desktop targets.

## Safety

Klipsch data is active-low through an existing 2N2222 transistor on GPIO25; do not connect GPIO25 directly to the amplifier IR DATA. Firmware actions are not physical acknowledgments from the speaker. HTTP/TCP services are unauthenticated and should stay on a trusted LAN, never port-forwarded. CI compiles with dummy Wi-Fi credentials; real hardware must still be commissioned.
