#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <WiFi.h>

// Portable across Wayne ESP32 controllers. Keep MQTT credentials in secrets.h;
// do not hardcode device IPs, names or passwords in this shared module.
class WayneEsp32Telemetry {
public:
    struct Config {
        const char* friendlyName;
        const char* hostname;
        const char* firmware;
        const char* mqttHost;
        uint16_t mqttPort;
        const char* mqttUser;
        const char* mqttPassword;
    };
    struct Sensor {
        const char* key;
        const char* name;
        const char* unit;
        const char* deviceClass;
        const char* stateClass;
    };
    using ExtraStateWriter = void (*)(JsonObject state);

    WayneEsp32Telemetry();

    void begin(const Config& config, ExtraStateWriter extraState = nullptr,
               const Sensor* extraSensors = nullptr, size_t extraCount = 0);
    void loop(bool otaReady);
    bool connected();
    const String& deviceId() const { return _deviceId; }
    const String& stateTopic() const { return _stateTopic; }
    const String& availabilityTopic() const { return _availabilityTopic; }

private:
    bool connectBroker();
    bool publishDiscovery();
    bool publishSensorDiscovery(const Sensor& sensor);
    bool publishConnectionDiscovery();
    bool publishState(bool otaReady);
    bool publishJson(const String& topic, JsonDocument& doc);
    void addDevice(JsonObject root);
    void logPublishFailure(const char* what);

    WiFiClient _socket;
    PubSubClient _mqtt;
    Config _config{};
    ExtraStateWriter _extraWriter = nullptr;
    const Sensor* _extraSensors = nullptr;
    size_t _extraCount = 0;
    String _deviceId;
    String _clientId;
    String _stateTopic;
    String _availabilityTopic;
    String _discoveryId;
    uint32_t _lastAttemptMs = 0;
    uint32_t _lastStateMs = 0;
    bool _firstAttempt = true;
    bool _discoveryComplete = false;
    bool _initialized = false;
};
