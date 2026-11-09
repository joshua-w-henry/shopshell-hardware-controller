from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")

EXPECTED = {
    "HDMI_DELL_PIN": 27,
    "HDMI_PS4_PIN": 32,
    "HDMI_APPLE_PIN": 33,
    "HDMI_CYCLE_PIN": 21,
}

for symbol, pin in EXPECTED.items():
    needle = f"static constexpr uint8_t {symbol} = {pin};"
    assert needle in MAIN, f"Missing or changed HDMI mapping: {needle}"
    assert f"pinMode({symbol}, OUTPUT_OPEN_DRAIN);" in MAIN, f"{symbol} must remain open-drain"

for route in (
    "/hdmi/input/1",
    "/hdmi/input/2",
    "/hdmi/input/3",
    "/hdmi/cycle",
    "/hdmi/status",
):
    assert f'"{route}"' in MAIN, f"Missing route {route}"

assert "STARTUP_HDMI_DELAY_MS" in MAIN
assert "STARTUP_HDMI_REASSERT_DELAY_MS" in MAIN
assert "selectHdmiDell();" in MAIN
assert "ShopShell_Hardware_Controller_v2.6" in MAIN

assert '"/panel/power/' not in MAIN
assert '"/panel/input/' not in MAIN
assert "CMD_POWER_ON" not in MAIN
assert "CMD_HDMI1" not in MAIN

print("PASS: HDMI switch contract")
print("  GPIO27 = Dell / Input 1")
print("  GPIO32 = PS4 / Input 2")
print("  GPIO33 = Apple TV / Input 3")
print("  GPIO21 = Cycle")
print("  open-drain outputs and current HTTP routes present")
