#pragma once

#include <Arduino.h>
#include <WiFi.h>

class ViewSonicLanControl
{
public:
    ViewSonicLanControl(IPAddress address, uint16_t port = 5000, uint32_t timeoutMs = 900);

    bool setStandby();
    bool setPowerOn();
    bool setMute(bool muted);

    bool lastOk() const { return _lastOk; }
    const String &lastCommand() const { return _lastCommand; }
    const String &lastReply() const { return _lastReply; }
    uint32_t lastAttemptMs() const { return _lastAttemptMs; }

private:
    bool sendSetCommand(const char *command);

    IPAddress _address;
    uint16_t _port;
    uint32_t _timeoutMs;
    bool _lastOk = false;
    String _lastCommand = "none";
    String _lastReply = "none";
    uint32_t _lastAttemptMs = 0;
};
