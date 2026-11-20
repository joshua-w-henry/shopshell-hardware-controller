#include "WayneEsp32Telemetry.h"

#include <esp_system.h>
#include <esp_timer.h>

namespace {
constexpr uint32_t RETRY_MS = 30000;     // Missing broker must not monopolize controller loop.
constexpr uint32_t TELEMETRY_MS = 60000; // One minute: adequate for fleet health.
constexpr uint16_t MQTT_PACKET_BYTES = 2048;

// Each key maps to a property in the retained JSON state payload.
// Sensor descriptors carry no site-specific information, so this file can be
// copied unmodified into the well, compressor and other Wayne ESP32 projects.
const WayneEsp32Telemetry::Sensor STANDARD_SENSORS[] = {
    {"ip", "IP Address", nullptr, nullptr, nullptr},
    {"hostname", "Hostname", nullptr, nullptr, nullptr},
    {"ssid", "Wi-Fi SSID", nullptr, nullptr, nullptr},
    {"rssi_dbm", "Wi-Fi RSSI", "dBm", "signal_strength", "measurement"},
    {"wifi_channel", "Wi-Fi Channel", nullptr, nullptr, nullptr},
    {"uptime_s", "Uptime", "s", "duration", "measurement"},
    {"free_heap_b", "Free Heap", "B", "data_size", "measurement"},
    {"firmware", "Firmware Version", nullptr, nullptr, nullptr},
    {"reset_reason", "Last Reset Reason", nullptr, nullptr, nullptr},
    {"mac", "Wi-Fi MAC", nullptr, nullptr, nullptr},
    {"ota_ready", "OTA Ready", nullptr, nullptr, nullptr},
};

const char* resetReasonText() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "power_on";
        case ESP_RST_EXT: return "external_reset";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt_watchdog";
        case ESP_RST_TASK_WDT: return "task_watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_DEEPSLEEP: return "deep_sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "sdio";
        default: return "unknown";
    }
}
} // namespace

WayneEsp32Telemetry::WayneEsp32Telemetry() : _mqtt(_socket) {}

void WayneEsp32Telemetry::begin(const Config& config,
                               ExtraStateWriter extraState,
                               const Sensor* extraSensors,
                               size_t extraCount) {
    _config = config;
    _extraWriter = extraState;
    _extraSensors = extraSensors;
    _extraCount = extraCount;

    // MAC-derived identity survives IP changes, firmware upgrades and renaming.
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    mac.toLowerCase();
    _deviceId = "wayne_esp32_" + mac;
    _clientId = "wayne-" + mac;
    _stateTopic = "wayne/esp32/" + mac + "/state";
    _availabilityTopic = "wayne/esp32/" + mac + "/availability";
    _discoveryId = _deviceId;

    if (!_mqtt.setBufferSize(MQTT_PACKET_BYTES)) {
        Serial.println("WayneTelemetry: unable to allocate MQTT packet buffer");
    }
    _mqtt.setKeepAlive(20);
    _mqtt.setSocketTimeout(2);
    IPAddress brokerIp;
    if (brokerIp.fromString(config.mqttHost)) {
        _mqtt.setServer(brokerIp, config.mqttPort);
    } else {
        _mqtt.setServer(config.mqttHost, config.mqttPort);
    }
    _initialized = true;
    _firstAttempt = true;
    Serial.printf("WayneTelemetry: id=%s broker=%s:%u interval=60s\n",
                  _deviceId.c_str(), config.mqttHost, config.mqttPort);
}

bool WayneEsp32Telemetry::connected() {
    return _initialized && _mqtt.connected();
}

void WayneEsp32Telemetry::loop(bool otaReady) {
    if (!_initialized || WiFi.status() != WL_CONNECTED)
        return;

    if (!_mqtt.connected()) {
        const uint32_t now = millis();
        if (!_firstAttempt && uint32_t(now - _lastAttemptMs) < RETRY_MS)
            return;
        _firstAttempt = false;
        _lastAttemptMs = now;
        if (!connectBroker())
            return;
        // Broker connection can happen after long Wi-Fi outage.
        _discoveryComplete = false;
        _lastStateMs = now;
        _discoveryComplete = publishDiscovery();
        publishState(otaReady);
    }

    _mqtt.loop();

    if (!_discoveryComplete)
        _discoveryComplete = publishDiscovery();

    if (uint32_t(millis() - _lastStateMs) >= TELEMETRY_MS) {
        _lastStateMs = millis();
        publishState(otaReady);
    }
}

bool WayneEsp32Telemetry::connectBroker() {
    // Retained LWT makes a device show offline even if it loses power suddenly.
    const bool authenticated = _config.mqttUser &&
                               _config.mqttUser[0] != '\0';
    bool ok = false;
    if (authenticated) {
        ok = _mqtt.connect(_clientId.c_str(),
                           _config.mqttUser, _config.mqttPassword,
                           _availabilityTopic.c_str(), 1, true, "offline");
    } else {
        ok = _mqtt.connect(_clientId.c_str(),
                           _availabilityTopic.c_str(), 1, true, "offline");
    }
    if (!ok) {
        Serial.printf("WayneTelemetry: MQTT connection failed, state=%d; retry in 30s\n",
                      _mqtt.state());
        return false;
    }

    if (!_mqtt.publish(_availabilityTopic.c_str(), "online", true))
        Serial.println("WayneTelemetry: failed to publish online status");
    Serial.println("WayneTelemetry: MQTT connected, availability online");
    return true;
}

void WayneEsp32Telemetry::addDevice(JsonObject root) {
    JsonObject device = root.createNestedObject("device");
    JsonArray ids = device.createNestedArray("identifiers");
    ids.add(_deviceId);
    device["name"] = _config.friendlyName;
    device["manufacturer"] = "Wayne";
    device["model"] = "ESP32 Controller";
    device["sw_version"] = _config.firmware;
}

bool WayneEsp32Telemetry::publishJson(const String& topic, JsonDocument& doc) {
    char payload[1450];
    if (measureJson(doc) >= sizeof(payload)) {
        Serial.printf("WayneTelemetry: JSON too large for %s\n", topic.c_str());
        return false;
    }
    const size_t written = serializeJson(doc, payload, sizeof(payload));
    return _mqtt.publish(topic.c_str(),
                         reinterpret_cast<const uint8_t*>(payload),
                         static_cast<unsigned int>(written), true);
}

bool WayneEsp32Telemetry::publishSensorDiscovery(const Sensor& sensor) {
    StaticJsonDocument<900> doc;
    const String entityId = _discoveryId + "_" + sensor.key;
    const String topic = "homeassistant/sensor/" + entityId + "/config";
    doc["name"] = sensor.name;
    doc["unique_id"] = entityId;
    doc["state_topic"] = _stateTopic;
    doc["availability_topic"] = _availabilityTopic;
    doc["payload_available"] = "online";
    doc["payload_not_available"] = "offline";
    doc["value_template"] = "{{ value_json." + String(sensor.key) + " }}";
    doc["entity_category"] = "diagnostic";
    if (sensor.unit) doc["unit_of_measurement"] = sensor.unit;
    if (sensor.deviceClass) doc["device_class"] = sensor.deviceClass;
    if (sensor.stateClass) doc["state_class"] = sensor.stateClass;
    addDevice(doc.to<JsonObject>()); // Rebuild with root set before serialization.
    return publishJson(topic, doc);
}

bool WayneEsp32Telemetry::publishConnectionDiscovery() {
    StaticJsonDocument<900> doc;
    JsonObject root = doc.to<JsonObject>();
    root["name"] = "Connection";
    root["unique_id"] = _discoveryId + "_connection";
    root["state_topic"] = _availabilityTopic;
    root["payload_on"] = "online";
    root["payload_off"] = "offline";
    root["device_class"] = "connectivity";
    root["entity_category"] = "diagnostic";
    // No availability_topic: OFF must remain visible when the LWT fires.
    addDevice(root);
    return publishJson("homeassistant/binary_sensor/" + _discoveryId +
                       "_connection/config", doc);
}

bool WayneEsp32Telemetry::publishDiscovery() {
    bool ok = publishConnectionDiscovery();
    for (const auto& sensor : STANDARD_SENSORS)
        ok = publishSensorDiscovery(sensor) && ok;
    for (size_t i = 0; i < _extraCount; ++i)
        ok = publishSensorDiscovery(_extraSensors[i]) && ok;
    if (!ok)
        Serial.println("WayneTelemetry: incomplete HA discovery, will retry");
    else
        Serial.println("WayneTelemetry: retained HA discovery published");
    return ok;
}

bool WayneEsp32Telemetry::publishState(bool otaReady) {
    StaticJsonDocument<1400> doc;
    JsonObject state = doc.to<JsonObject>();
    state["ip"] = WiFi.localIP().toString();
    state["hostname"] = _config.hostname;
    state["ssid"] = WiFi.SSID();
    state["rssi_dbm"] = WiFi.RSSI();
    state["wifi_channel"] = WiFi.channel();
    // esp_timer is 64-bit monotonic: unlike millis(), it won't wrap after 49 days.
    state["uptime_s"] = static_cast<uint32_t>(esp_timer_get_time() / 1000000ULL);
    state["free_heap_b"] = ESP.getFreeHeap();
    state["firmware"] = _config.firmware;
    state["reset_reason"] = resetReasonText();
    state["mac"] = WiFi.macAddress();
    state["ota_ready"] = otaReady ? "ready" : "not_ready";
    if (_extraWriter)
        _extraWriter(state);
    if (!publishJson(_stateTopic, doc)) {
        Serial.println("WayneTelemetry: failed to publish state");
        return false;
    }
    return true;
}
