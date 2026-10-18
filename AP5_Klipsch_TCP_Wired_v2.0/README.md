# ShopShell Hardware Controller — HDMI Switch + Klipsch R-15PM v2.5

**Firmware ID:** `ShopShell_Hardware_Controller_v2.5`  
**Target:** ESP32 Dev Module, PlatformIO, Arduino / Espressif32 6.12.0  
**Network hostname:** `promethean-shop`  
**Primary role:** local low-level hardware controller for the ShopShell station.

This version is derived from the proven AP5 + wired Klipsch v2.0 baseline. The Promethean AP5-specific power and source commands are retired. The existing MAX3232 UART hardware remains available as a general RS-232 bridge on TCP port 2323.

## Current hardware functions

### HDMI switch

The Amazon 3-input HDMI switch wired remote uses passive button-to-ground signaling. Measured idle level is approximately 3.27 V and a physical button press pulls the selected line to common/ground.

The ESP32 therefore emulates the remote with **open-drain GPIO outputs only**:

| Shop function | Remote closure | ESP32 | Firmware route |
|---|---|---:|---|
| Dell / ShopShell | pin 5 ↔ pin 1 | GPIO27 | `POST /hdmi/input/1` or `/hdmi/dell` |
| PS4 | pin 5 ↔ pin 3 | GPIO32 | `POST /hdmi/input/2` or `/hdmi/ps4` |
| Apple TV | pin 5 ↔ pin 2 | GPIO33 | `POST /hdmi/input/3` or `/hdmi/apple-tv` |
| Cycle | pin 5 ↔ pin 4 | GPIO21 | `POST /hdmi/cycle` |
| Common | pin 5 | ESP32 GND | direct common reference |

`HIGH` on these GPIOs means released/high-impedance. `LOW` means button pressed. The firmware never intentionally drives the switch lines HIGH against the switch's own pull-ups.

Each command releases all four lines, pulls only the requested line LOW for 160 ms, then releases it.

### Cold-boot source selection

After ESP32 boot:

1. Wait 3.0 seconds for the HDMI switch to power up.
2. Select Input 1 / Dell / ShopShell.
3. Wait 2.5 seconds.
4. Reassert Input 1 once.

This runs locally and does not depend on Wi-Fi, ShopShell, Home Assistant, or Internet access.

### Appliance boot audio normalization

v2.5 deliberately **does not change ViewSonic mute state during boot**. ViewSonic mute/unmute was found to interact unpredictably with the Windows audio endpoint/mixer, including leaving the station muted after startup.

The automatic audio initialization is now local to the Klipsch path:

1. Wait 7 seconds after ESP32 boot.
2. Feed **70 Klipsch Volume Up presses** into the wired NEC queue in bounded chunks.
3. Wait for the volume ramp to drain completely.
4. Queue **Klipsch Optical** and wait for that command to drain.
5. Mark audio bootstrap complete.

The sequence does not depend on ViewSonic LAN availability or Wi-Fi and does not alter Windows/ViewSonic audio state. Manual ViewSonic mute/unmute routes remain available for diagnostics but are not used automatically.

The boot audio state is exposed by `GET /system/startup/status`, and the sequence can be retriggered manually with `POST /system/audio/bootstrap`.

### ViewSonic LAN / coordinated shutdown

The ESP32 now speaks the native IFP70 LAN protocol directly to the ViewSonic:

- Display address: `192.168.87.74`
- TCP port: `5000`
- Mute ON: `801s6001<CR>`
- Mute OFF: `801s6000<CR>`
- Power ON: `801s!001<CR>`
- Standby: `801s!000<CR>` — a complete TCP write counts as accepted even if the panel drops LAN control before returning `401+<CR>` during its Android shutdown countdown.

`POST /system/shutdown` sends ViewSonic standby and requires a valid set-command acknowledgement. ShopShell uses this as a **best-effort** display step after arming the S31 delayed AC cutoff; a display-control failure must not prevent normal Windows shutdown.

With ViewSonic CEC and Wake on Active Source enabled, the resulting appliance behavior is:

```text
BOOT
S31 AC restored
-> Dell / HDMI switch power up
-> ESP32 selects Dell / Input 1 locally
-> active Dell HDMI wakes ViewSonic
-> Klipsch gain normalized to MAX, Optical selected
-> ViewSonic / Windows mute state remains untouched
-> Windows launches ShopShell

SHUTDOWN
ShopShell OFF
-> S31 autonomous 5-minute AC cutoff armed
-> ESP32 tells ViewSonic to enter standby
-> ViewSonic CEC may power off the active CEC source
-> Windows shuts down normally
-> S31 removes station AC after the safety delay
```

### Klipsch R-15PM wired control

The proven wired NEC interface remains unchanged:

```text
ESP32 GPIO25 -> 2N2222A base drive -> Klipsch IR DATA pull-down
ESP32 GND    -> Klipsch signal ground
```

The ESP32 RMT peripheral emits the demodulated NEC envelope directly. There is **no 38 kHz carrier** on the wire. GPIO25 drives the transistor; the Klipsch circuit provides the data-line pull-up.

The existing 15-command Klipsch API remains available, including power, mute, volume, subwoofer level, source selection, play/pause, previous track, and LED toggle. Next Track remains unverified.

### General RS-232 bridge

The old AP5 UART/MAX3232 hardware is retained as a general serial interface:

| Function | Setting |
|---|---|
| ESP32 RX | GPIO16 |
| ESP32 TX | GPIO17 |
| UART | 38,400 baud, 8N1 |
| Raw TCP bridge | port 2323 |
| USB serial passthrough | ESP32 USB serial ↔ UART2 |

No AP5-specific serial command is sent automatically in v2.5.

### Optional optical IR diagnostic

GPIO26 remains available for the existing optical IR receiver diagnostic. `GET /ir/last` reports the last decoded optical frame. It is independent of the wired Klipsch GPIO25 output.

## HTTP API

Base example: `http://192.168.87.42`

### Controller / HDMI

- `GET /health` — firmware, IP, RSSI, uptime, RS-232 bridge baud.
- `GET /hdmi/status` — wiring map and last commanded HDMI action. This is command history only; the HDMI switch has no feedback line.
- `POST /hdmi/input/1` — Dell / ShopShell.
- `POST /hdmi/input/2` — PS4.
- `POST /hdmi/input/3` — Apple TV.
- `POST /hdmi/cycle` — cycle button.
- Friendly aliases: `/hdmi/dell`, `/hdmi/ps4`, `/hdmi/apple-tv`.

### ViewSonic / system orchestration

- `GET /viewsonic/status` — last LAN command/result known by the ESP32.
- `POST /viewsonic/mute/on`
- `POST /viewsonic/mute/off`
- `POST /viewsonic/power/on`
- `POST /viewsonic/standby`
- `GET /system/startup/status` — current boot-audio stage and any failure.
- `POST /system/audio/bootstrap` — manually rerun the safe audio-normalization sequence.
- `POST /system/shutdown` — request ViewSonic standby for coordinated station shutdown.

### Klipsch

- `GET /klipsch/remote`
- `GET /klipsch/commands`
- `GET /klipsch/status`
- `GET/POST /klipsch/send?cmd=volume/up`
- `GET/POST /klipsch/volume/up?count=3`
- `GET/POST /klipsch/hold?cmd=volume_up&repeats=3`
- `GET/POST /klipsch/nec?cmd=0x0A`

Named command routes:

- `/klipsch/power`
- `/klipsch/mute`
- `/klipsch/volume/up`
- `/klipsch/volume/down`
- `/klipsch/sub/up`
- `/klipsch/sub/down`
- `/klipsch/sub/reset`
- `/klipsch/led`
- `/klipsch/media/play-pause`
- `/klipsch/media/previous`
- `/klipsch/input/phono`
- `/klipsch/input/aux`
- `/klipsch/input/optical`
- `/klipsch/input/usb`
- `/klipsch/input/bluetooth`

## Current GPIO map

| GPIO | Function |
|---:|---|
| 16 | RS-232 UART RX |
| 17 | RS-232 UART TX |
| 21 | HDMI switch Cycle |
| 25 | Klipsch wired NEC / 2N2222 base drive |
| 26 | Optional optical IR receiver |
| 27 | HDMI switch Input 1 / Dell |
| 32 | HDMI switch Input 2 / PS4 |
| 33 | HDMI switch Input 3 / Apple TV |

## Build / flash

1. Copy `src/secrets.h.example` to `src/secrets.h` if needed and enter the shop Wi-Fi credentials. Never commit `src/secrets.h`.
2. Open the directory containing `platformio.ini` in VS Code / PlatformIO.
3. Build and upload.
4. Open Serial Monitor at 115,200 baud.
5. Confirm startup messages show the HDMI GPIO map, Klipsch RMT ready, and two Dell/Input-1 source selections.
6. Open `http://192.168.87.42/health` and verify firmware `ShopShell_Hardware_Controller_v2.5`.
7. Use the standalone commissioning console in `tools/` to test each HDMI input and the existing Klipsch controls.

## Commissioning cautions

- HDMI GPIOs are **open-drain only**. Do not change them to ordinary push-pull outputs that drive 3.3 V HIGH into the switch.
- The HDMI switch common/remote pin 5 is directly tied to ESP32 ground in the installed wiring.
- `/hdmi/status` cannot know the physical selected source; it reports only the last command issued by the ESP32.
- Klipsch power is a toggle and amplifier volume has no readback. The automatic max-gain sequence therefore assumes the Klipsch is already powered/available.
- Automatic boot intentionally does not issue ViewSonic mute/unmute commands because they were observed to interact with the Windows audio mixer state.
- HTTP and TCP 2323 are unauthenticated LAN services. Do not expose them directly to an untrusted network or the Internet.

## Verification

`tests/test_nec_waveform.py` continues to validate the Klipsch NEC field encoding and timing assumptions. Hardware acceptance for v2.5 is:

- Input 1 selects Dell / ShopShell.
- Input 2 selects PS4.
- Input 3 selects Apple TV.
- Cycle advances the switch exactly once per request.
- After a controller cold boot, the switch settles on Dell / Input 1.
- Existing Klipsch wired controls remain functional.
- General RS-232 TCP bridge still listens on port 2323.

### v2.5 additional acceptance

- Automatic boot reaches `audio_stage=complete`.
- Klipsch finishes at maximum internal gain and Optical input.
- Automatic boot does not issue ViewSonic mute or unmute commands.
- The 70-step Klipsch ramp is fed incrementally and does not rely on the NEC queue being large enough to hold the entire sequence at once.
- `POST /system/shutdown` places the ViewSonic into standby.
- ShopShell OFF continues Windows shutdown even if ViewSonic standby fails.
