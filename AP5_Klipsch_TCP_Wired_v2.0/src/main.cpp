#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <stdlib.h>
#include <IRrecv.h>
#include <IRutils.h>

#include "secrets.h"
#include "KlipschWired.h"

static constexpr char HOSTNAME[] = "promethean-shop";
static constexpr uint16_t TCP_PORT = 2323;
static constexpr uint16_t HTTP_PORT = 80;

// General RS-232 bridge retained from the AP5 installation. It is no longer
// used for display source selection, but remains available for future shop
// hardware on the existing MAX3232 interface.
static constexpr int SERIAL_RX = 16;
static constexpr int SERIAL_TX = 17;
static constexpr uint32_t SERIAL_BAUD = 38400;

static constexpr uint16_t IR_RX_PIN = 26; // optional optical receiver diagnostic

// Amazon 3-input HDMI switch wired-remote emulation. The switch holds each
// signal at ~3.27 V and a physical button pulls one signal to common/ground.
// HIGH on these open-drain outputs means released/high-Z; LOW means pressed.
static constexpr uint8_t HDMI_DELL_PIN = 27;  // remote pin 1: Input 1 / Dell / ShopShell
static constexpr uint8_t HDMI_PS4_PIN = 32;   // remote pin 3: Input 2 / PS4
static constexpr uint8_t HDMI_APPLE_PIN = 33; // remote pin 2: Input 3 / Apple TV
static constexpr uint8_t HDMI_CYCLE_PIN = 21; // remote pin 4: Cycle
static constexpr uint16_t HDMI_PRESS_MS = 160;
static constexpr uint32_t STARTUP_HDMI_DELAY_MS = 3000;
static constexpr uint32_t STARTUP_HDMI_REASSERT_DELAY_MS = 2500;

HardwareSerial SerialBridge(2);
WiFiServer tcpServer(TCP_PORT);
WiFiClient tcpClient;
WebServer httpServer(HTTP_PORT);

// Optional optical IR receive diagnostic, independent of wired GPIO25 output.
IRrecv irReceiver(IR_RX_PIN);
decode_results irResults;
uint32_t irReceiveCount = 0;
uint32_t irLastSeenMs = 0;
String irLastProtocol = "none";
String irLastCode = "none";
String irLastAddress = "none";
String irLastCommand = "none";
uint16_t irLastBits = 0;

void processIrReceiver()
{
    if (!irReceiver.decode(&irResults)) return;

    irReceiveCount++;
    irLastSeenMs = millis();
    irLastProtocol = typeToString(irResults.decode_type);
    irLastBits = irResults.bits;

    char hex[20];
    if (irResults.bits <= 32)
        snprintf(hex, sizeof(hex), "0x%08lX",
                 static_cast<unsigned long>(irResults.value & 0xFFFFFFFFULL));
    else
        snprintf(hex, sizeof(hex), "0x%08lX%08lX",
                 static_cast<unsigned long>(irResults.value >> 32),
                 static_cast<unsigned long>(irResults.value & 0xFFFFFFFFULL));
    irLastCode = hex;
    if (irResults.decode_type == NEC && irResults.bits == 32) {
        char addrBuf[12], cmdBuf[12];
        snprintf(addrBuf, sizeof(addrBuf), "0x%04X", static_cast<unsigned>(irResults.address));
        snprintf(cmdBuf, sizeof(cmdBuf), "0x%02X", static_cast<unsigned>(irResults.command));
        irLastAddress = addrBuf;
        irLastCommand = cmdBuf;
    } else {
        irLastAddress = "none";
        irLastCommand = "none";
    }

    Serial.printf("IR RECEIVED: protocol=%s bits=%u code=%s count=%lu\n",
                  irLastProtocol.c_str(), irLastBits, irLastCode.c_str(),
                  static_cast<unsigned long>(irReceiveCount));
    irReceiver.resume();
}

bool networkServicesStarted = false;

// HDMI switch command history. There is no feedback wire, so this records only
// what the ESP32 commanded, not the physical selected source.
const char *hdmiLastAction = "none";
uint32_t hdmiLastCommandMs = 0;
uint32_t hdmiCommandCount = 0;

void releaseAllHdmiSwitchLines()
{
    digitalWrite(HDMI_DELL_PIN, HIGH);
    digitalWrite(HDMI_PS4_PIN, HIGH);
    digitalWrite(HDMI_APPLE_PIN, HIGH);
    digitalWrite(HDMI_CYCLE_PIN, HIGH);
}

void beginHdmiSwitch()
{
    digitalWrite(HDMI_DELL_PIN, HIGH);
    digitalWrite(HDMI_PS4_PIN, HIGH);
    digitalWrite(HDMI_APPLE_PIN, HIGH);
    digitalWrite(HDMI_CYCLE_PIN, HIGH);
    pinMode(HDMI_DELL_PIN, OUTPUT_OPEN_DRAIN);
    pinMode(HDMI_PS4_PIN, OUTPUT_OPEN_DRAIN);
    pinMode(HDMI_APPLE_PIN, OUTPUT_OPEN_DRAIN);
    pinMode(HDMI_CYCLE_PIN, OUTPUT_OPEN_DRAIN);
    releaseAllHdmiSwitchLines();
    Serial.println("HDMI switch ready: GPIO27=Dell, GPIO32=PS4, GPIO33=Apple TV, GPIO21=Cycle");
}

void pulseHdmiSwitch(uint8_t pin, const char *action)
{
    releaseAllHdmiSwitchLines();
    delay(2);
    digitalWrite(pin, LOW);
    delay(HDMI_PRESS_MS);
    digitalWrite(pin, HIGH);
    hdmiLastAction = action;
    hdmiLastCommandMs = millis();
    ++hdmiCommandCount;
    Serial.printf("HDMI switch: %s (GPIO%u, %u ms)\n", action, pin, HDMI_PRESS_MS);
}

void sendJsonOk(const char *action)
{
    String json = "{\"ok\":true,\"action\":\"";
    json += action;
    json += "\"}";
    httpServer.send(200, "application/json", json);
}

void selectHdmiDell() { pulseHdmiSwitch(HDMI_DELL_PIN, "hdmi/input/1:dell"); }
void selectHdmiPs4() { pulseHdmiSwitch(HDMI_PS4_PIN, "hdmi/input/2:ps4"); }
void selectHdmiAppleTv() { pulseHdmiSwitch(HDMI_APPLE_PIN, "hdmi/input/3:apple-tv"); }
void cycleHdmiInput() { pulseHdmiSwitch(HDMI_CYCLE_PIN, "hdmi/cycle"); }

// Dynamic Klipsch HTTP API. Single canonical command table in KlipschWired.cpp.
// GET for manual browser tests; POST for ShopShell and Home Assistant.
void jsonError(int status, const char *error)
{
    String json = "{\"ok\":false,\"error\":\"";
    json += error;
    json += "\"}";
    httpServer.send(status, "application/json", json);
}

String hexCommand(uint8_t command)
{
    char buffer[8];
    snprintf(buffer, sizeof(buffer), "0x%02X", command);
    return String(buffer);
}

String hexRaw(uint8_t command)
{
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "0x%08lX",
             static_cast<unsigned long>(klipschRawNEC(command)));
    return String(buffer);
}

bool parseUInt(const String &text, uint32_t maxAllowed, uint32_t &out)
{
    if (text.isEmpty() || text.startsWith("-") || text.startsWith("+")) return false;
    char *end = nullptr;
    const int base = text.startsWith("0x") || text.startsWith("0X") ? 16 : 10;
    unsigned long v = strtoul(text.c_str(), &end, base);
    if (end == text.c_str() || *end != '\0' || v > maxAllowed) return false;
    out = static_cast<uint32_t>(v);
    return true;
}

void handleKlipschCommands()
{
    String json = "{\"model\":\"R-15PM\",\"transport\":\"wired_demodulated_nec\",";
    json += "\"encoding\":\"NEC address 0x02, active-low pulse envelope\",";
    json += "\"commands\":[";
    for (size_t i = 0; i < klipschCommandCount(); i++) {
        const KlipschCommand *c = klipschCommandAt(i);
        if (i) json += ",";
        json += "{\"path\":\"/klipsch/" + String(c->path) + "\",";
        json += "\"label\":\"" + String(c->label) + "\",";
        json += "\"command\":\"" + hexCommand(c->command) + "\",";
        json += "\"raw\":\"" + hexRaw(c->command) + "\",";
        json += "\"repeatable\":" + String(c->repeatable ? "true" : "false") + ",";
        json += "\"source\":\"" + String(c->source) + "\"}";
    }
    json += "],\"missing\":[\"media/next: original remote supports it, exact code unverified\"]}";
    httpServer.send(200, "application/json", json);
}

void handleKlipschRemote()
{
    String page = "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
    page += "<title>Shop Klipsch Remote</title><style>body{font:16px system-ui;background:#141820;color:#eee;margin:24px;max-width:700px;}";
    page += "h1{font-size:24px}a{display:inline-block;background:#303c50;color:white;text-decoration:none;padding:12px 14px;margin:5px;border-radius:9px}";
    page += "a:active{background:#5776a3}small{color:#b6c0d0}hr{border:1px solid #303c50;margin-top:20px}</style></head><body>";
    page += "<h1>Klipsch R-15PM Wired Control</h1><p>WIRED NEC, demodulated active-low output; logical addr 0x02</p>";
    for (size_t i = 0; i < klipschCommandCount(); ++i) {
        const KlipschCommand *c = klipschCommandAt(i);
        if (i == 2 || i == 4 || i == 7 || i == 8 || i == 10) page += "<hr>";
        page += "<a href=\"/klipsch/" + String(c->path) + "\">" + String(c->label) + "</a>";
    }
    page += "<hr><p><small>Volume control example: /klipsch/volume/up?count=3. ";
    page += "Next Track code not yet verified. All operations are toggles/relative controls; no amplifier-volume readback.</small></p>";
    page += "<p><a href=\"/klipsch/commands\">JSON command catalog</a><a href=\"/klipsch/status\">Status</a></p></body></html>";
    httpServer.send(200, "text/html", page);
}

void handleKlipschStatus()
{
    String json = "{\"ok\":true,\"tx_gpio\":25,\"rx_gpio\":26,\"address\":\"0x02\",";
    json += "\"transport\":\"wired_demodulated_nec\",\"active_low_at_speaker\":true,";
    json += "\"carrier_enabled\":false,\"output_ready\":" + String(klipschTxReady() ? "true" : "false") + ",";
    json += "\"encoding\":\"bit_reversed_logical_nec\",\"queue_depth\":" + String(klipschQueueDepth());
    json += ",\"commands_sent\":" + String(klipschTotalSent());
    json += ",\"repeat_frames_sent\":" + String(klipschRepeatFramesSent());
    json += ",\"last_sent_cmd\":\"" + hexCommand(klipschLastSentCommand()) + "\"";
    json += ",\"last_sent_ms\":" + String(klipschLastSentMillis()) + "}";
    httpServer.send(200, "application/json", json);
}

void handleKlipschRequest()
{
    if (httpServer.method() != HTTP_GET && httpServer.method() != HTTP_POST) {
        jsonError(405, "Use GET or POST"); return;
    }
    const String uri = httpServer.uri();
    if (uri == "/klipsch/commands") { handleKlipschCommands(); return; }
    if (uri == "/klipsch/remote") { handleKlipschRemote(); return; }
    if (uri == "/klipsch/status") { handleKlipschStatus(); return; }
    if (uri == "/klipsch/media/next" || uri == "/klipsch/next") {
        jsonError(501, "R-15PM next-track code not verified; use /klipsch/nec?cmd=0xNN for manual tests");
        return;
    }
    uint8_t logicalCmd = 0;
    const KlipschCommand *matched = nullptr;
    bool manual = false;
    String key;
    if (uri == "/klipsch/nec") {
        if (!httpServer.hasArg("cmd")) { jsonError(400, "Missing cmd; e.g. 0x1C"); return; }
        uint32_t parsed = 0;
        if (!parseUInt(httpServer.arg("cmd"), 255, parsed)) { jsonError(400, "cmd must be 0..255 or 0x00..0xFF"); return; }
        logicalCmd = static_cast<uint8_t>(parsed);
        key = "nec";
        manual = true;
    } else {
        if (uri == "/klipsch/send" || uri == "/klipsch/steps" || uri == "/klipsch/hold") {
            if (!httpServer.hasArg("cmd")) { jsonError(400, "Missing cmd name"); return; }
            key = httpServer.arg("cmd");
        } else if (uri.startsWith("/klipsch/")) {
            key = uri.substring(9);
        } else {
            jsonError(404, "not found"); return;
        }
        matched = findKlipschCommand(key);
        if (!matched) { jsonError(404, "Unknown Klipsch command. GET /klipsch/commands"); return; }
        logicalCmd = matched->command;
    }

    uint32_t count = 1, repeats = 0;
    if (httpServer.hasArg("count") && !parseUInt(httpServer.arg("count"), 10, count)) {
        jsonError(400, "count must be 1..10"); return;
    }
    if (httpServer.hasArg("repeats") && !parseUInt(httpServer.arg("repeats"), 5, repeats)) {
        jsonError(400, "repeats must be 0..5"); return;
    }
    if (!count || ((count > 1 || repeats) && matched && !matched->repeatable)) {
        jsonError(400, "Multiple presses/repeats permitted only on volume, sub-level, previous-track");
        return;
    }
    if (manual && (count > 3 || repeats > 2)) {
        jsonError(400, "Manual NEC experiments limited to count<=3, repeats<=2"); return;
    }
    if (!klipschTxReady()) { jsonError(503, "Wired NEC output not initialized"); return; }
    if (!queueKlipschWired(logicalCmd, static_cast<uint8_t>(count), static_cast<uint8_t>(repeats))) {
        jsonError(429, "Wired NEC queue full or invalid request"); return;
    }
    String json = "{\"ok\":true,\"action\":\"klipsch/";
    json += manual ? "nec" : String(matched->path);
    json += "\",\"command\":\"" + hexCommand(logicalCmd) + "\",";
    json += "\"raw\":\"" + hexRaw(logicalCmd) + "\",";
    json += "\"queued\":" + String(count) + ",\"repeats\":" + String(repeats) + ",";
    json += "\"queue_depth\":" + String(klipschQueueDepth()) + "}";
    httpServer.send(200, "application/json", json);
}

enum class StartupStage
{
    WaitForHdmiSwitch,
    WaitForDellReassert,
    Complete
};

StartupStage startupStage = StartupStage::WaitForHdmiSwitch;
uint32_t startupDeadlineMs = 0;

bool deadlineReached(uint32_t deadline)
{
    return static_cast<int32_t>(millis() - deadline) >= 0;
}

void beginStartupSequence()
{
    startupStage = StartupStage::WaitForHdmiSwitch;
    startupDeadlineMs = millis() + STARTUP_HDMI_DELAY_MS;
}

void processStartupSequence()
{
    if (startupStage == StartupStage::Complete || !deadlineReached(startupDeadlineMs))
        return;

    switch (startupStage)
    {
        case StartupStage::WaitForHdmiSwitch:
            Serial.println("Startup: HDMI switch -> Dell / Input 1");
            selectHdmiDell();
            startupStage = StartupStage::WaitForDellReassert;
            startupDeadlineMs = millis() + STARTUP_HDMI_REASSERT_DELAY_MS;
            break;

        case StartupStage::WaitForDellReassert:
            Serial.println("Startup: HDMI switch -> Dell / Input 1 (reassert)");
            selectHdmiDell();
            startupStage = StartupStage::Complete;
            Serial.println("Startup source selection complete");
            break;

        case StartupStage::Complete:
            break;
    }
}

void configureHttpRoutes()
{
    httpServer.on("/health", HTTP_GET, []()
    {
        String json = "{";
        json += "\"ok\":true,";
        json += "\"device\":\"promethean-shop\",";
        json += "\"role\":\"shopshell-hardware-controller\",";
        json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
        json += "\"uptime_ms\":" + String(millis()) + ",";
        json += "\"serial_bridge_baud\":" + String(SERIAL_BAUD) + ",";
        json += "\"firmware\":\"ShopShell_Hardware_Controller_v2.1\"";
        json += "}";
        httpServer.send(200, "application/json", json);
    });

    httpServer.on("/hdmi/status", HTTP_GET, []()
    {
        String json = "{\"ok\":true,";
        json += "\"transport\":\"open_drain_button_emulation\",";
        json += "\"button_ms\":" + String(HDMI_PRESS_MS) + ",";
        json += "\"input1\":{\"name\":\"Dell / ShopShell\",\"gpio\":27,\"remote_pin\":1},";
        json += "\"input2\":{\"name\":\"PS4\",\"gpio\":32,\"remote_pin\":3},";
        json += "\"input3\":{\"name\":\"Apple TV\",\"gpio\":33,\"remote_pin\":2},";
        json += "\"cycle\":{\"gpio\":21,\"remote_pin\":4},";
        json += "\"common_remote_pin\":5,";
        json += "\"last_action\":\"" + String(hdmiLastAction) + "\",";
        json += "\"last_command_ms\":" + String(hdmiLastCommandMs) + ",";
        json += "\"command_count\":" + String(hdmiCommandCount);
        json += "}";
        httpServer.send(200, "application/json", json);
    });

    httpServer.on("/hdmi/input/1", HTTP_POST, []() { selectHdmiDell(); sendJsonOk("hdmi/input/1"); });
    httpServer.on("/hdmi/dell", HTTP_POST, []() { selectHdmiDell(); sendJsonOk("hdmi/dell"); });
    httpServer.on("/hdmi/input/2", HTTP_POST, []() { selectHdmiPs4(); sendJsonOk("hdmi/input/2"); });
    httpServer.on("/hdmi/ps4", HTTP_POST, []() { selectHdmiPs4(); sendJsonOk("hdmi/ps4"); });
    httpServer.on("/hdmi/input/3", HTTP_POST, []() { selectHdmiAppleTv(); sendJsonOk("hdmi/input/3"); });
    httpServer.on("/hdmi/apple-tv", HTTP_POST, []() { selectHdmiAppleTv(); sendJsonOk("hdmi/apple-tv"); });
    httpServer.on("/hdmi/cycle", HTTP_POST, []() { cycleHdmiInput(); sendJsonOk("hdmi/cycle"); });

    httpServer.on("/ir/last", HTTP_GET, []()
    {
        String json = "{\"rx_gpio\":26,\"count\":" + String(irReceiveCount);
        json += ",\"protocol\":\"" + irLastProtocol + "\"";
        json += ",\"code\":\"" + irLastCode + "\"";
        json += ",\"address\":\"" + irLastAddress + "\"";
        json += ",\"command\":\"" + irLastCommand + "\"";
        json += ",\"bits\":" + String(irLastBits);
        json += ",\"last_seen_ms\":" + String(irLastSeenMs);
        json += "}";
        httpServer.send(200, "application/json", json);
    });

    httpServer.onNotFound([]()
    {
        if (httpServer.uri().startsWith("/klipsch/")) handleKlipschRequest();
        else jsonError(404, "not found");
    });
}

void beginWiFi()
{
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOSTNAME);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void processNetworkState()
{
    if (WiFi.status() == WL_CONNECTED && !networkServicesStarted)
    {
        tcpServer.begin();
        tcpServer.setNoDelay(true);
        httpServer.begin();
        networkServicesStarted = true;

        Serial.printf("Hostname: %s\n", HOSTNAME);
        Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("RS-232 TCP bridge listening on port %u\n", TCP_PORT);
        Serial.printf("HTTP API listening on port %u\n", HTTP_PORT);
    }
}

void processTcpBridge()
{
    if (!networkServicesStarted)
        return;

    if (!tcpClient || !tcpClient.connected())
    {
        if (tcpClient)
            tcpClient.stop();

        WiFiClient incoming = tcpServer.available();
        if (incoming)
        {
            tcpClient = incoming;
            tcpClient.setNoDelay(true);
        }
    }

    if (tcpClient && tcpClient.connected())
    {
        while (tcpClient.available())
            SerialBridge.write(static_cast<uint8_t>(tcpClient.read()));
    }
}

void processUsbPassthrough()
{
    while (Serial.available())
        SerialBridge.write(static_cast<uint8_t>(Serial.read()));
}

void processSerialResponses()
{
    while (SerialBridge.available())
    {
        uint8_t value = static_cast<uint8_t>(SerialBridge.read());
        Serial.write(value);

        if (tcpClient && tcpClient.connected())
            tcpClient.write(value);
    }
}

void setup()
{
    Serial.begin(115200);
    delay(100);

    SerialBridge.begin(SERIAL_BAUD, SERIAL_8N1, SERIAL_RX, SERIAL_TX);
    beginKlipschWired(); // GPIO25 via 2N2222; active-low demodulated NEC on Klipsch data wire.
    beginHdmiSwitch();
    irReceiver.enableIRIn(); // GPIO26 optional optical 38 kHz receiver OUT (not wire loopback).
    Serial.println("Optional OPTICAL diagnostic receiver on GPIO26 (independent of wired TX)");

    Serial.println();
    Serial.println("ShopShell hardware controller: HDMI switch + Klipsch R-15PM wired NEC v2.1");

    // Arm before Wi-Fi so startup never depends on network availability.
    beginStartupSequence();

    configureHttpRoutes();
    beginWiFi();
}

void loop()
{
    // Local HDMI default-source startup has priority and works even with Wi-Fi down.
    processStartupSequence();

    processNetworkState();

    if (networkServicesStarted)
        httpServer.handleClient();

    processTcpBridge();
    processUsbPassthrough();
    processSerialResponses();
    processIrReceiver();
    processKlipschWired(); // ESP32 RMT: carrier-free demodulated pulse envelope

    delay(1);
}
