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
    // IFP70 Set Power: s ! 000 = standby.
    //
    // On the IFP6570, accepting standby starts the Android shutdown countdown
    // and the LAN-control service may disappear before it returns 401+<CR>.
    // A successful TCP connection plus a complete command write is therefore
    // sufficient for standby only. Explicit negative replies still fail.
    return sendSetCommand("801s!000\r", true);
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

bool ViewSonicLanControl::setDisplayPort()
{
    // IFP70 Set Input Select: s " 009 = DisplayPort.
    return sendSetCommand("801s\"009\r");
}

bool ViewSonicLanControl::setHdmi1()
{
    // IFP70 Set Input Select: s " 004 = HDMI1.
    return sendSetCommand("801s\"004\r");
}

bool ViewSonicLanControl::getInput(
    String &source,
    bool &signalDetected,
    String &rawValue)
{
    // IFP70 Get Input Select: g j 000.
    //
    // Reply value's first digit is signal detection and the final two digits
    // identify the selected source. Examples:
    //   109 = signal present on DisplayPort
    //   009 = DisplayPort selected, no signal
    //   104 = signal present on HDMI1
    String value;
    if (!sendGetCommand("801gj000\r", 'j', value))
        return false;

    rawValue = value;
    signalDetected = value.length() == 3 && value[0] == '1';

    const String sourceCode =
        value.length() == 3
            ? value.substring(1)
            : String();

    if (sourceCode == "09")
        source = "dp";
    else if (sourceCode == "04")
        source = "hdmi1";
    else if (sourceCode == "14")
        source = "hdmi2";
    else if (sourceCode == "24")
        source = "hdmi3";
    else if (sourceCode == "34")
        source = "hdmi4";
    else if (sourceCode == "0A")
        source = "android";
    else
        source = "other";

    return true;
}

bool ViewSonicLanControl::sendSetCommand(
    const char *command,
    bool acceptSuccessfulWriteWithoutReply)
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

    const size_t commandLength = strlen(command);
    const size_t written = client.print(command);
    client.flush();

    if (written != commandLength)
    {
        client.stop();
        _lastReply = "write_failed";
        return false;
    }

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

    if (acceptSuccessfulWriteWithoutReply && reply.isEmpty())
    {
        _lastReply = "sent_no_ack";
        _lastOk = true;
        return true;
    }

    _lastReply = reply.isEmpty() ? "timeout" : reply;
    _lastReply.replace("\r", "<CR>");
    return false;
}

bool ViewSonicLanControl::sendGetCommand(
    const char *command,
    char expectedCommandCode,
    String &value)
{
    _lastAttemptMs = millis();
    _lastCommand = command;
    _lastCommand.replace("\r", "<CR>");
    _lastReply = "none";
    _lastOk = false;
    value = "";

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

    const size_t commandLength = strlen(command);
    const size_t written = client.print(command);
    client.flush();

    if (written != commandLength)
    {
        client.stop();
        _lastReply = "write_failed";
        return false;
    }

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

                if (reply.length() < 9 ||
                    !reply.startsWith("801r") ||
                    reply[4] != expectedCommandCode)
                {
                    return false;
                }

                value = reply.substring(5, 8);
                _lastOk = value.length() == 3;
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
