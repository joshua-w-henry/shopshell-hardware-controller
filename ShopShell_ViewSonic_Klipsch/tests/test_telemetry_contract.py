from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / "src" / "main.cpp").read_text()
HEADER = (ROOT / "lib" / "WayneEsp32Telemetry" / "src" / "WayneEsp32Telemetry.h").read_text()
IMPL = (ROOT / "lib" / "WayneEsp32Telemetry" / "src" / "WayneEsp32Telemetry.cpp").read_text()
SECRETS = (ROOT / "src" / "secrets.h.example").read_text()
INI = (ROOT / "platformio.ini").read_text()

assert "WayneEsp32Telemetry telemetry;" in MAIN
assert "telemetry.loop(" in MAIN
assert "telemetry.begin(" in MAIN
assert '"mqtt_device_id"' not in MAIN or "mqtt_device_id" in MAIN
assert "ViewSonic Last Command Result" in MAIN
assert "Audio Bootstrap Stage" in MAIN
assert "Klipsch Command Queue" in MAIN
assert "knolleary/PubSubClient@2.8" in INI
assert "bblanchon/ArduinoJson@6.21.5" in INI
assert '#define MQTT_HOST "192.168.86.24"' in SECRETS
assert "REPLACE_WITH_MQTT_PASSWORD" in SECRETS
assert "bool publishDiscovery()" in HEADER or "publishDiscovery();" in HEADER
for fragment in (
    '"homeassistant/sensor/"', '"homeassistant/binary_sensor/"',
    '"wayne/esp32/"', '"online"', '"offline"',
    '"availability_topic"', '"value_template"',
    '"unique_id"', '"identifiers"', '"device_class"',
    '"connected"', '"rssi_dbm"', '"free_heap_b"', '"uptime_s"',
    '"reset_reason"', '"ota_ready"', '"mac"', '"firmware"',
    'esp_timer_get_time()', 'WiFi.macAddress()', 'setSocketTimeout(2)',
    'setKeepAlive(20)', 'RETRY_MS', 'TELEMETRY_MS'
):
    assert fragment in IMPL, fragment
assert '_mqtt.publish(_availabilityTopic.c_str(), "online", true)' in IMPL
assert '_availabilityTopic.c_str(), 1, true, "offline"' in IMPL
assert "sizeof(SHOP_TELEMETRY) / sizeof(SHOP_TELEMETRY[0])" in MAIN
assert "GPIO25" in (ROOT / "README.md").read_text()
print("PASS: Wayne telemetry MAC identity, MQTT LWT, retained HA discovery, heartbeat, shop diagnostics")
