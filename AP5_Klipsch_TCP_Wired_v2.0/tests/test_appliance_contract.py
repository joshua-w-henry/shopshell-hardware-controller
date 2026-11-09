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
    '801s\\\"009\\r',
    '801s\\\"004\\r',
    '801gj000\\r',
):
    assert command in VIEWSONIC, f"Missing ViewSonic command {command}"

for route in (
    "/viewsonic/status",
    "/viewsonic/mute/on",
    "/viewsonic/mute/off",
    "/viewsonic/power/on",
    "/viewsonic/standby",
    "/viewsonic/input/status",
    "/viewsonic/input/dp",
    "/viewsonic/input/hdmi1",
    "/system/audio/bootstrap",
    "/system/startup/status",
    "/system/shutdown",
):
    assert f'"{route}"' in MAIN, f"Missing route {route}"

assert "KLIPSCH_BOOT_MAX_STEPS = 70" in MAIN
assert "KLIPSCH_BOOT_QUEUE_WATERMARK = 20" in MAIN
assert "queueNextKlipschBootVolumeChunk()" in MAIN
assert 'findKlipschCommand("input/optical")' in MAIN
assert "SelectingKlipschOptical" in MAIN
assert "WaitingForKlipschOptical" in MAIN
assert "MAX_QUEUE = 96" in KLIPSCH
assert "viewSonic.setStandby()" in MAIN
assert "acceptSuccessfulWriteWithoutReply" in VIEWSONIC
assert '"sent_no_ack"' in VIEWSONIC
assert "ShopShell_Hardware_Controller_v2.6" in MAIN

# Cold-boot invariant: automatic audio bootstrap must not touch the
# ViewSonic mute state. Manual mute routes remain available for diagnostics.
bootstrap_start = MAIN.index("void processAudioBootstrap()")
bootstrap_end = MAIN.index("enum class StartupStage", bootstrap_start)
bootstrap = MAIN[bootstrap_start:bootstrap_end]
assert "setMute(" not in bootstrap
assert "MutingViewSonic" not in bootstrap
assert "UnmutingViewSonic" not in bootstrap
assert "WaitingForStartDelay" in bootstrap

queue_pos = bootstrap.index("queueNextKlipschBootVolumeChunk()")
optical_pos = bootstrap.index("queueKlipschBootOptical()")
assert queue_pos < optical_pos

# Queue-capacity invariant: the 70-step ramp is fed in bounded chunks and
# Optical is not selected until the volume queue has drained.
assert "remaining > 10 ? 10" in MAIN
assert "klipschQueueDepth() >= KLIPSCH_BOOT_QUEUE_WATERMARK" in MAIN
assert 'audioBootstrapStage = AudioBootstrapStage::SelectingKlipschOptical' in MAIN

print("PASS: ViewSonic + appliance boot/shutdown contract")
print("  ViewSonic mute state untouched -> incremental 70-step Klipsch max -> Optical")
print("  ViewSonic standby exposed for coordinated shutdown")
print("  ViewSonic DP/HDMI1 set + input readback exposed")
