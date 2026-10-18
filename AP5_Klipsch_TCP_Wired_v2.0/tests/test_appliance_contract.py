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
assert "KLIPSCH_BOOT_QUEUE_WATERMARK = 20" in MAIN
assert "viewSonic.setMute(true)" in MAIN
assert "queueNextKlipschBootVolumeChunk()" in MAIN
assert 'findKlipschCommand("input/optical")' in MAIN
assert "SelectingKlipschOptical" in MAIN
assert "WaitingForKlipschOptical" in MAIN
assert "viewSonic.setMute(false)" in MAIN
assert "MAX_QUEUE = 96" in KLIPSCH
assert "viewSonic.setStandby()" in MAIN
assert "ShopShell_Hardware_Controller_v2.3" in MAIN

# Cold-boot safety invariant: the gain ramp cannot begin until deterministic
# ViewSonic mute has succeeded. A slow ViewSonic must be retried rather than
# causing a permanent startup failure.
mute_pos = MAIN.index("if (viewSonic.setMute(true))")
queue_pos = MAIN.index("queueNextKlipschBootVolumeChunk()")
optical_pos = MAIN.index("queueKlipschBootOptical()")
unmute_pos = MAIN.index("if (viewSonic.setMute(false))")
assert mute_pos < queue_pos < optical_pos < unmute_pos
assert "AUDIO_BOOT_MAX_VIEWSONIC_ATTEMPTS" not in MAIN
assert 'failAudioBootstrap("viewsonic_mute_failed")' not in MAIN
assert "ViewSonic not ready (%s); retrying" in MAIN
assert "ViewSonic unmute not ready (%s); retrying" in MAIN

# Queue-capacity invariant: the 70-step ramp is fed in bounded chunks and
# Optical is not selected until the volume queue has drained.
assert "remaining > 10 ? 10" in MAIN
assert "klipschQueueDepth() >= KLIPSCH_BOOT_QUEUE_WATERMARK" in MAIN
assert 'audioBootstrapStage = AudioBootstrapStage::SelectingKlipschOptical' in MAIN

print("PASS: ViewSonic + appliance boot/shutdown contract")
print("  retry ViewSonic until ready -> incremental 70-step Klipsch max -> Optical -> unmute")
print("  ViewSonic standby exposed for coordinated shutdown")
