# Shop Promethean AP5 + Klipsch R-15PM — WIRED NEC v2.0

**Firmware ID:** `AP5_Klipsch_Wired_NEC_v2.0`  
**Target:** ESP32 Dev Module, PlatformIO, Arduino / Espressif32 6.12.0 (Arduino-ESP32 2.x, legacy RMT driver)  
**Purpose:** Deliver **demodulated NEC** remote-control pulses straight to the Klipsch's low-voltage IR DATA input through a transistor. **No IR LED and no 38 kHz carrier.**

This is a development build based on `AP5_Klipsch_TCP_Full_Control_v1.0.zip`. It preserves the panel TCP/RS-232 bridge, the AP5 power/input endpoints, the Wi-Fi settings pattern, the optional external IR-receiver diagnostic, and the 15-command Klipsch API. The original remote's **Next Track** code remains unverified; the manual logical-code endpoint allows testing additional command bytes.

> **VALIDATION STATUS:** The v1 alternative logical NEC address 0x02, Power command 0x0A was reported to work, but the specific confirmation of **speaker response** versus **loopback decoder response** was ambiguous in the conversation. This v2 **wired hardware interface has not yet been tested on the Klipsch**. HTTP `ok:true` reports a queued command, not confirmation that the speaker received or executed it.

## Safety and connection rules — read before wiring

The R-15PM is a **mains-powered amplifier**. Complete all low-voltage wiring with the speaker fully unplugged, with appropriate clearance from the AC supply section. Never probe exposed mains/PSU sections while powered. Route insulated leads safely and reassemble the amplifier before AC-powered operational testing.

Observed mapping of the **original 6644S receiver** on the front IR/LED PCB (lettering facing forward):

| Front-board receiver | Ribbon wire | Identified role |
|---|---:|---|
| Left pin 1 | 6 | Demodulated IR DATA to amplifier board |
| Middle pin 2 | 5 | Ground |
| Right pin 3 | 4 | Receiver supply (same rail as HC595 VCC) |

This was inferred from continuity mapping; **the amplifier's actual operating voltage and the output-driver type of the 6644S were not measured while mounted**.

**Electrical concept:**

```
ESP32 GPIO25 --- 330R --- 330R --- B  2N2222A
                                       C --- [protect/isolated connection] --- Klipsch ribbon 6 (IR DATA)
ESP32 GND ----------------------------- E
             `------------------------------ Klipsch ribbon 5 (GND)

Klipsch IR DATA idle HIGH (from Klipsch pull-up, if present)
GPIO25 LOW -> transistor OFF -> line released / idle
GPIO25 HIGH -> transistor ON  -> line LOW   / NEC mark
```

- GPIO25 is **only connected to the base resistor**. Never connect the ESP32 GPIO directly to ribbon 6. No 5 V drive supply is needed for the wired data pull-down.
- Your measured 2N2222A device has **base as center leg**, but verify emitter and collector for the specific transistor before connecting. Optional ~100 kΩ base-to-GND pulldown holds the driver off during ESP32 reset/boot.
- **CRITICAL:** The factory 6644S receiver output may be push-pull and drive its data line HIGH. An externally added transistor pulling the same signal LOW can fight it. **Do not wire the transistor collector directly in parallel with a still-connected factory receiver output until that stage is electrically characterized.** The safest first configuration electrically isolates the factory receiver OUT from the data signal (leaving Klipsch data pull-up and ground intact), or uses an engineered current-limited interface after verifying idle voltage and source impedance. A protective resistor alone cannot guarantee correct logic levels against a strong active-high source.
- **Check power domains:** Confirm factory IR DATA idle voltage and the receiver/output circuit **before** tying anything powered to the ESP32, with appropriately isolated, pre-attached leads and mains sections enclosed. Do not exceed transistor ratings. An open-collector pull-down cannot create an idle HIGH on an otherwise floating input—verify a pull-up exists and is rated for the speaker's logic supply.
- **No electrical connection to ribbon 4/VCC** is required for this driver. ESP32 GND and Klipsch signal ground must be correctly referenced unless a proper galvanic isolation stage is substituted.
- Avoid blasting audio while testing. Start with a source LED toggle or volume **down** at low upstream playback gain. Power is a toggle, not discrete ON/OFF.

## Waveform and bit format

The original optical chain was: `GPIO25 -> 38 kHz emitter -> 6644S -> Klipsch IR DATA`.

The wired chain is: `GPIO25 -> NPN open collector -> Klipsch IR DATA`.

The ESP32's **RMT hardware** emits exact demodulated envelope timing (1 microsecond tick):

- **NEC full frame:** 9,000 µs active-low header, 4,500 µs idle, 32 data bits MSB-first in the encoded raw representation, then 560 µs active-low trailing mark.
- **Each data bit:** 560 µs active-low mark followed by 560 µs idle for 0, or 1,690 µs idle for 1.
- **NEC repeat frame:** 9,000 µs mark + 2,250 µs space + 560 µs mark, dispatched ~110 ms after previous start when requested.
- **Logical NEC address:** 0x02, with inverted address byte and inverted command byte, and bit reversal matching `IRremoteESP8266::encodeNEC()` used in the v1 alternative test.
- **Power:** Logical `addr=0x02`, `cmd=0x0A`; raw `0x40BF50AF`.
- **No 38 kHz carrier**, and GPIO25 returns LOW after every transmission.

The GPIO26 **optional optical IR receiver** still provides `/ir/last` if you retain it as a separate diagnostic accessory. It **will not decode** the new wired output directly from GPIO25; the wire carries the *already demodulated* envelope, not 38 kHz IR light. Use a logic analyzer or oscilloscope on the isolated low-voltage output to validate this new electrical signal. `/klipsch/status` reports the queued/delivered waveform requests, not speaker acknowledgement.

## GPIO and original Promethean functions

| Signal | ESP32 | Notes |
|---|---:|---|
| Klipsch wired NEC | GPIO25 | HIGH pulses to NPN base; active-LOW data at speaker |
| Optional *optical* diagnostic IR RX | GPIO26 | Unchanged; 3.3 V IR receiver OUT |
| Promethean RS-232 RX (UART2) | GPIO16 | 38,400 8N1 |
| Promethean RS-232 TX (UART2) | GPIO17 | 38,400 8N1 |
| Promethean transparent TCP bridge | Port 2323 | Unchanged |
| HTTP API | Port 80 | Unchanged |
| Network hostname | promethean-shop | Wi-Fi credentials in `src/secrets.h` |

**Panel startup remains unchanged:** after ESP boot, 5 s -> AP5 Power On; 7 s later HDMI2; 10 s later HDMI2 reassert. It works without Wi-Fi. This firmware **does not automatically change the Klipsch power, source, or volume** on ESP32 boot.

## HTTP control

The original URI names remain unchanged, so existing ShopShell and Home Assistant integrations need no endpoint changes.

Base example: `http://192.168.87.42` (verify actual address via `/health`; IP could change).

| Action | Route | Logical command |
|---|---|---|
| Power toggle | `/klipsch/power` | 0x0A |
| Mute toggle | `/klipsch/mute` | 0x0D |
| Volume up | `/klipsch/volume/up` | 0x1C |
| Volume down | `/klipsch/volume/down` | 0x07 |
| Subwoofer up | `/klipsch/sub/up` | 0x0C |
| Subwoofer down | `/klipsch/sub/down` | 0x04 |
| Subwoofer reset | `/klipsch/sub/reset` | 0x03 |
| Status LED toggle | `/klipsch/led` | 0x08 |
| Play / pause | `/klipsch/media/play-pause` | 0x1E |
| Previous track | `/klipsch/media/previous` | 0x02 |
| Phono | `/klipsch/input/phono` | 0x01 |
| AUX | `/klipsch/input/aux` | 0x09 |
| Optical | `/klipsch/input/optical` | 0x00 |
| USB | `/klipsch/input/usb` | 0x48 |
| Bluetooth | `/klipsch/input/bluetooth` | 0x5C |
| Next track | `/klipsch/media/next` | **Unknown**; returns HTTP 501 |

Other endpoints:

- `GET /health` — firmware version, IP, Wi-Fi strength, uptime.
- `GET /klipsch/remote` — built-in browser control surface.
- `GET /klipsch/commands` — machine-readable command table and source notes.
- `GET /klipsch/status` — transport, output initialization, commands and repeat frames sent.
- `GET/POST /klipsch/send?cmd=volume/up` — named command.
- `GET/POST /klipsch/volume/up?count=3` — three separate full NEC presses; bounded 1–10.
- `GET/POST /klipsch/hold?cmd=volume_up&repeats=3` — one full frame plus 3 NEC repeat frames; bounded 0–5 on repeatable commands.
- `GET/POST /klipsch/nec?cmd=0x0A` — manual logical command `0x00..0xFF` at fixed address `0x02` (bounded experimental use).
- `GET /ir/last` — optional optical diagnostic receiver on GPIO26, not wire loopback.
- `POST /panel/power/on`, `/panel/power/off`, `/panel/input/hdmi1` ... `/hdmi4`, `/panel/input/ops` — preserved.

Example for ShopShell:

```powershell
$base = 'http://192.168.87.42'
Invoke-RestMethod -Method POST -Uri "$base/klipsch/input/optical"
Invoke-RestMethod -Method POST -Uri "$base/klipsch/volume/up?count=3"
Invoke-RestMethod -Uri "$base/klipsch/status"
```

### Installation

1. Extract this ZIP and open the directory containing `platformio.ini` in VS Code / PlatformIO.
2. Copy existing Wi-Fi `src/secrets.h` into this project's `src/` (see `secrets.h.example`).
3. Build and upload. Monitor Serial at 115,200 baud; check for `Klipsch WIRED NEC ... RMT ready` and raw Power `0x40BF50AF`.
4. Verify `GET /health` contains `AP5_Klipsch_Wired_NEC_v2.0` and `GET /klipsch/status` shows `carrier_enabled:false`, `output_ready:true`.
5. On a bench, **before connecting to Klipsch**, verify GPIO25 pulses HIGH for NEC mark intervals and idles LOW, ideally with scope or logic analyzer. With a 2N2222, check that the isolated collector follows the inverse polarity when pulled up to a **known safe low-voltage test supply via a resistor**. The existing optical receiver will not validate the new output.
6. Only connect the speaker once the input and original receiver's output-driver behavior are characterized, with all low-voltage cabling installed while the speaker is disconnected from AC.

**LAN security:** HTTP and the transparent TCP bridge are unauthenticated; do not expose these ports to untrusted networks or directly to the Internet.

## Verification coverage and limitations

- The NEC field encoding, all 15 logical command codes, and timing definitions are tested with offline assertions in `tests/test_nec_waveform.py`.
- The standalone `KlipschWired.cpp` file can be syntax-checked with local C++ mocks, though the native ESP32 RMT/Arduino toolchain is needed for a complete firmware build.
- Hardware testing on the wired Klipsch input remains **pending**. There is currently no feedback indicating that the amplifier accepted a command.

## Reference starting points

- Original R-15PM manual: https://assets.klipsch.com/product-manuals/R-15PM-Manual-v07WEB.pdf
- Original v1.0 working project: `AP5_Klipsch_TCP_Full_Control_v1.0.zip` (not included in this ZIP).
- IRremoteESP8266 original encoding: `encodeNEC(0x02, cmd)`; the new implementation deliberately matches that raw result while removing the IR carrier.
