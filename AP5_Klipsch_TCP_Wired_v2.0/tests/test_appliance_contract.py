from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
VIEWSONIC = (ROOT / "src" / "ViewSonicLan.cpp").read_text(encoding="utf-8")
KLIPSCH = (ROOT / "src" / "KlipschWired.cpp").read_text(encoding="utf-8")

for command in (
    '801s!000\\r',
    '801s!001\\r',
    '801s6001\\r',
    '801s6000\\r',
):
    assert command in VIEWSONIC, f"Missing ViewSonic command {command}"

for route in (
    "/viewsonic/status",
    "/viewsonic/mute/on",
    "/viewsonic/mute/off",
    "/viewsonic/power/on",
    "/viewsonic/standby",
    "/system/audio/bootstrap",
    "/system/startup/status",
    "/system/shutdown",
):
    assert f'"{route}"' in MAIN, f"Missing route {route}"

assert "KLIPSCH_BOOT_MAX_STEPS = 70" in MAIN
assert "viewSonic.setMute(true)" in MAIN
assert "queueKlipschBootNormalization()" in MAIN
assert "findKlipschCommand(\"input/optical\")" in MAIN
assert "viewSonic.setMute(false)" in MAIN
assert "MAX_QUEUE = 96" in KLIPSCH
assert "viewSonic.setStandby()" in MAIN
assert "ShopShell_Hardware_Controller_v2.2" in MAIN

# Safety invariant: failure to establish upstream mute must fail the boot
# bootstrap instead of proceeding with the max-volume ramp.
mute_pos = MAIN.index("if (viewSonic.setMute(true))")
fail_pos = MAIN.index('failAudioBootstrap("viewsonic_mute_failed")')
queue_pos = MAIN.index("queueKlipschBootNormalization()")
assert mute_pos < fail_pos < queue_pos

print("PASS: ViewSonic + appliance boot/shutdown contract")
print("  upstream mute -> 70-step Klipsch max -> Optical -> upstream unmute")
print("  ViewSonic standby exposed for coordinated shutdown")
