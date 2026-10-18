#include "ViewSonicLan.h"

ViewSonicLanControl::ViewSonicLanControl(
    IPAddress address,
    uint16_t port,
    uint32_t timeoutMs)
    : _address(address),
      _port(port),
      _timeoutMs(timeoutMs)
{
}

bool ViewSonicLanControl::setStandby()
{
    // IFP70 Set Power: s ! 000 = standby
    return sendSetCommand("801s!000\r");
}

bool ViewSonicLanControl::setPowerOn()
{
    // IFP70 Set Power: s ! 001 = on
    return sendSetCommand("801s!001\r");
}

bool ViewSonicLanControl::setMute(bool muted)
{
    // IFP70 Set Mute: s 6 001 = mute, 000 = unmute
    return sendSetCommand(muted ? "801s6001\r" : "801s6000\r");
}

bool ViewSonicLanControl::sendSetCommand(const char *command)
{
    _lastAttemptMs = millis();
    _lastCommand = command;
    _lastCommand.replace("\r", "<CR>");
    _lastReply = "none";
    _lastOk = false;

    if (WiFi.status() != WL_CONNECTED)
    {
        _lastReply = "wifi_disconnected";
        return false;
    }

    WiFiClient client;
    if (!client.connect(_address, _port))
    {
        _lastReply = "connect_failed";
        return false;
    }

    client.print(command);
    client.flush();

    const uint32_t started = millis();
    String reply;
    while (static_cast<uint32_t>(millis() - started) < _timeoutMs)
    {
        while (client.available())
        {
            const char ch = static_cast<char>(client.read());
            reply += ch;
            if (ch == '\r')
            {
                client.stop();
                _lastReply = reply;
                _lastReply.replace("\r", "<CR>");
                // Set-function valid reply for display ID 01 is 401+<CR>.
                _lastOk = reply.startsWith("401+");
                return _lastOk;
            }
        }
        delay(1);
    }

    client.stop();
    _lastReply = reply.isEmpty() ? "timeout" : reply;
    _lastReply.replace("\r", "<CR>");
    return false;
}
