from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text()
CFG = (ROOT / "src" / "ControllerConfig.h").read_text()
SEC = (ROOT / "src" / "secrets.h.example").read_text()

assert 'VIEWSONIC_IP(192, 168, 86, 33)' in CFG
assert 'VIEWSONIC_TCP_PORT = 5000' in CFG
assert 'ShopShell_ViewSonic_Klipsch_v3.0' in CFG
assert '#define WIFI_SSID "Shangri La"' in SEC
for retired in ('"/hdmi/', 'HDMI_DELL_PIN', 'HDMI_PS4_PIN',
 'HDMI_APPLE_PIN', 'HDMI_CYCLE_PIN', 'OUTPUT_OPEN_DRAIN',
 'beginHdmiSwitch', 'pulseHdmiSwitch', 'beginStartupSequence',
 'processStartupSequence', 'selectHdmiDell', 'STARTUP_HDMI_DELAY_MS'):
    assert retired not in MAIN, retired
for retained in ('"/viewsonic/input/dp"', '"/viewsonic/input/hdmi1"',
 '"/viewsonic/input/status"', '"/viewsonic/standby"',
 '"/system/shutdown"', '"/viewsonic/status"', '"/health"',
 'beginKlipschWired()', 'beginAudioBootstrap();', 'ArduinoOTA.begin()',
 'processTcpBridge()', 'processAudioBootstrap()', 'processUsbPassthrough()'):
    assert retained in MAIN, retained
print("PASS: external HDMI switch retired, ViewSonic/Klipsch/OTA preserved")
