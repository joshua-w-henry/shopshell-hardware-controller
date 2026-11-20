#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <stdlib.h>
#include <IRrecv.h>
#include <IRutils.h>

#include "secrets.h"
#include "KlipschWired.h"
#include "ViewSonicLan.h"
#include "ControllerConfig.h"

static constexpr char HOSTNAME[] = "esp32-shopshell";
static constexpr uint16_t TCP_PORT = 2323;
static constexpr uint16_t HTTP_PORT = 80;
static constexpr uint32_t WIFI_DIAG_INTERVAL_MS = 10000;

#ifdef LED_BUILTIN
static constexpr int WIFI_STATUS_LED_PIN = LED_BUILTIN;
static constexpr bool WIFI_STATUS_LED_AVAILABLE = true;
#else
static constexpr int WIFI_STATUS_LED_PIN = -1;
static constexpr bool WIFI_STATUS_LED_AVAILABLE = false;
#endif

// General RS-232 bridge retained from the AP5 installation. It is no longer
// used for display source selection, but remains available for future shop
// hardware on the existing MAX3232 interface.
static constexpr int SERIAL_RX = 16;
static constexpr int SERIAL_TX = 17;
static constexpr uint32_t SERIAL_BAUD = 38400;

static constexpr uint16_t IR_RX_PIN = 26; // optional optical receiver diagnostic

// Appliance boot audio policy:
// 1) leave ViewSonic / Windows mute state completely untouched,
// 2) saturate the Klipsch internal gain at maximum,
// 3) select Optical after the gain ramp has drained.
//
// ViewSonic mute/unmute is intentionally excluded from automatic boot because
// it can interact with the Windows audio endpoint/mixer state.
static constexpr uint16_t KLIPSCH_BOOT_MAX_STEPS = 70;
static constexpr uint8_t KLIPSCH_BOOT_QUEUE_WATERMARK = 20;
static constexpr uint32_t AUDIO_BOOT_NOT_BEFORE_MS = 7000;

HardwareSerial SerialBridge(2);
WiFiServer tcpServer(TCP_PORT);
WiFiClient tcpClient;
WebServer httpServer(HTTP_PORT);
ViewSonicLanControl viewSonic(VIEWSONIC_IP, VIEWSONIC_TCP_PORT);

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
int lastWiFiStatus = -1;
uint32_t lastWiFiDiagMs = 0;

const char *wifiStatusName(wl_status_t status)
{
    switch (status)
    {
        case WL_IDLE_STATUS: return "idle";
        case WL_NO_SSID_AVAIL: return "no_ssid_available";
        case WL_SCAN_COMPLETED: return "scan_completed";
        case WL_CONNECTED: return "connected";
        case WL_CONNECT_FAILED: return "connect_failed";
        case WL_CONNECTION_LOST: return "connection_lost";
        case WL_DISCONNECTED: return "disconnected";
        default: return "unknown";
    }
}

void setWiFiStatusLed(bool connected)
{
    if (!WIFI_STATUS_LED_AVAILABLE)
        return;

    digitalWrite(WIFI_STATUS_LED_PIN, connected ? HIGH : LOW);
}

void sendJsonOk(const char *action)
{
    String json = "{\"ok\":true,\"action\":\"";
    json += action;
    json += "\"}";
    httpServer.send(200, "application/json", json);
}

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

bool deadlineReached(uint32_t deadline);

enum class AudioBootstrapStage
{
    WaitingForStartDelay,
    WaitingForKlipschIdle,
    QueueingKlipschNormalization,
    WaitingForKlipschNormalization,
    SelectingKlipschOptical,
    WaitingForKlipschOptical,
    Complete,
    Failed
};

AudioBootstrapStage audioBootstrapStage = AudioBootstrapStage::WaitingForStartDelay;
uint32_t audioBootstrapDeadlineMs = 0;
uint16_t audioBootstrapKlipschStepsQueued = 0;
String audioBootstrapError = "none";

const char *audioBootstrapStageName()
{
    switch (audioBootstrapStage)
    {
        case AudioBootstrapStage::WaitingForStartDelay: return "waiting_for_start_delay";
        case AudioBootstrapStage::WaitingForKlipschIdle: return "waiting_for_klipsch_idle";
        case AudioBootstrapStage::QueueingKlipschNormalization: return "queueing_klipsch_normalization";
        case AudioBootstrapStage::WaitingForKlipschNormalization: return "waiting_for_klipsch_normalization";
        case AudioBootstrapStage::SelectingKlipschOptical: return "selecting_klipsch_optical";
        case AudioBootstrapStage::WaitingForKlipschOptical: return "waiting_for_klipsch_optical";
        case AudioBootstrapStage::Complete: return "complete";
        case AudioBootstrapStage::Failed: return "failed";
    }
    return "unknown";
}

bool audioBootstrapInProgress()
{
    return audioBootstrapStage != AudioBootstrapStage::Complete &&
           audioBootstrapStage != AudioBootstrapStage::Failed;
}

void beginAudioBootstrap(bool immediate = false)
{
    audioBootstrapStage = AudioBootstrapStage::WaitingForStartDelay;
    audioBootstrapDeadlineMs = millis() + (immediate ? 0 : AUDIO_BOOT_NOT_BEFORE_MS);
    audioBootstrapKlipschStepsQueued = 0;
    audioBootstrapError = "none";
}

void failAudioBootstrap(const char *reason)
{
    audioBootstrapError = reason;
    audioBootstrapStage = AudioBootstrapStage::Failed;
    Serial.printf("Audio bootstrap FAILED SAFE: %s\n", reason);
}

bool queueNextKlipschBootVolumeChunk()
{
    const KlipschCommand *volumeUp = findKlipschCommand("volume/up");
    if (!volumeUp || !klipschTxReady())
        return false;

    if (audioBootstrapKlipschStepsQueued >= KLIPSCH_BOOT_MAX_STEPS)
        return true;

    // Feed the existing NEC queue incrementally instead of assuming a specific
    // queue capacity. This keeps the bootstrap nonblocking and leaves headroom
    // for the transmitter state machine while the 70-step ramp is in progress.
    if (klipschQueueDepth() >= KLIPSCH_BOOT_QUEUE_WATERMARK)
        return true;

    const uint16_t remaining =
        KLIPSCH_BOOT_MAX_STEPS - audioBootstrapKlipschStepsQueued;
    const uint8_t chunk =
        remaining > 10 ? 10 : static_cast<uint8_t>(remaining);

    if (!queueKlipschWired(volumeUp->command, chunk, 0))
        return true; // Queue may be temporarily busy; retry on the next loop.

    audioBootstrapKlipschStepsQueued += chunk;
    Serial.printf("Audio bootstrap: queued %u/%u Volume Up presses\n",
                  audioBootstrapKlipschStepsQueued,
                  KLIPSCH_BOOT_MAX_STEPS);
    return true;
}

bool queueKlipschBootOptical()
{
    const KlipschCommand *optical = findKlipschCommand("input/optical");
    return optical &&
           klipschTxReady() &&
           queueKlipschWired(optical->command, 1, 0);
}

void processAudioBootstrap()
{
    switch (audioBootstrapStage)
    {
        case AudioBootstrapStage::WaitingForStartDelay:
            if (!deadlineReached(audioBootstrapDeadlineMs))
                return;
            audioBootstrapStage = AudioBootstrapStage::WaitingForKlipschIdle;
            break;

        case AudioBootstrapStage::WaitingForKlipschIdle:
            if (klipschQueueDepth() != 0)
                return;
            audioBootstrapStage = AudioBootstrapStage::QueueingKlipschNormalization;
            break;

        case AudioBootstrapStage::QueueingKlipschNormalization:
            if (!klipschTxReady() ||
                findKlipschCommand("volume/up") == nullptr)
            {
                failAudioBootstrap("klipsch_volume_command_unavailable");
                return;
            }

            if (!queueNextKlipschBootVolumeChunk())
            {
                failAudioBootstrap("klipsch_volume_queue_unavailable");
                return;
            }

            if (audioBootstrapKlipschStepsQueued >= KLIPSCH_BOOT_MAX_STEPS)
                audioBootstrapStage = AudioBootstrapStage::WaitingForKlipschNormalization;
            return;

        case AudioBootstrapStage::WaitingForKlipschNormalization:
            if (klipschQueueDepth() != 0)
                return;
            Serial.println("Audio bootstrap: Klipsch max-volume ramp complete");
            audioBootstrapStage = AudioBootstrapStage::SelectingKlipschOptical;
            break;

        case AudioBootstrapStage::SelectingKlipschOptical:
            if (!queueKlipschBootOptical())
            {
                if (!klipschTxReady() ||
                    findKlipschCommand("input/optical") == nullptr)
                {
                    failAudioBootstrap("klipsch_optical_command_unavailable");
                }
                return;
            }
            Serial.println("Audio bootstrap: Klipsch Optical queued");
            audioBootstrapStage = AudioBootstrapStage::WaitingForKlipschOptical;
            return;

        case AudioBootstrapStage::WaitingForKlipschOptical:
            if (klipschQueueDepth() != 0)
                return;
            audioBootstrapStage = AudioBootstrapStage::Complete;
            audioBootstrapError = "none";
            Serial.println("Audio bootstrap complete: Klipsch max + Optical; ViewSonic audio state untouched");
            return;

        case AudioBootstrapStage::Complete:
        case AudioBootstrapStage::Failed:
            return;
    }
}

bool deadlineReached(uint32_t deadline)
{
    return static_cast<int32_t>(millis() - deadline) >= 0;
}

void configureHttpRoutes()
{
    httpServer.on("/health", HTTP_GET, []()
    {
        String json = "{";
        json += "\"ok\":true,";
        json += "\"device\":\"" + String(HOSTNAME) + "\",";
        json += "\"role\":\"shopshell-hardware-controller\",";
        json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
        json += "\"wifi_status\":\"" + String(wifiStatusName(WiFi.status())) + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
        json += "\"wifi_led_gpio\":" + String(WIFI_STATUS_LED_PIN) + ",";
        json += "\"wifi_led_on\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
        json += "\"uptime_ms\":" + String(millis()) + ",";
        json += "\"viewsonic_ip\":\"" + VIEWSONIC_IP.toString() + "\",";
        json += "\"viewsonic_port\":" + String(VIEWSONIC_TCP_PORT) + ",";
        json += "\"serial_bridge_baud\":" + String(SERIAL_BAUD) + ",";
        json += "\"firmware\":\"" + String(FIRMWARE_ID) + "\",";
        json += "\"audio_bootstrap\":\"" + String(audioBootstrapStageName()) + "\"";
        json += "}";
        httpServer.send(200, "application/json", json);
    });

    httpServer.on("/viewsonic/status", HTTP_GET, []()
    {
        String json = "{\"ok\":true,";
        json += "\"address\":\"" + VIEWSONIC_IP.toString() + ":" + String(VIEWSONIC_TCP_PORT) + "\",";
        json += "\"last_ok\":" + String(viewSonic.lastOk() ? "true" : "false") + ",";
        json += "\"last_command\":\"" + viewSonic.lastCommand() + "\",";
        json += "\"last_reply\":\"" + viewSonic.lastReply() + "\",";
        json += "\"last_attempt_ms\":" + String(viewSonic.lastAttemptMs()) + "}";
        httpServer.send(200, "application/json", json);
    });

    httpServer.on("/viewsonic/input/status", HTTP_GET, []()
    {
        String source;
        String rawValue;
        bool signalDetected = false;

        if (!viewSonic.getInput(source, signalDetected, rawValue))
        {
            jsonError(502, "ViewSonic input query failed");
            return;
        }

        String json = "{\"ok\":true,";
        json += "\"source\":\"" + source + "\",";
        json += "\"signal_detected\":" + String(signalDetected ? "true" : "false") + ",";
        json += "\"raw_value\":\"" + rawValue + "\"}";
        httpServer.send(200, "application/json", json);
    });

    httpServer.on("/viewsonic/input/dp", HTTP_POST, []()
    {
        if (viewSonic.setDisplayPort()) sendJsonOk("viewsonic/input/dp");
        else jsonError(502, "ViewSonic DisplayPort selection failed");
    });

    httpServer.on("/viewsonic/input/hdmi1", HTTP_POST, []()
    {
        if (viewSonic.setHdmi1()) sendJsonOk("viewsonic/input/hdmi1");
        else jsonError(502, "ViewSonic HDMI1 selection failed");
    });

    httpServer.on("/viewsonic/mute/on", HTTP_POST, []()
    {
        if (viewSonic.setMute(true)) sendJsonOk("viewsonic/mute/on");
        else jsonError(502, "ViewSonic mute command failed");
    });

    httpServer.on("/viewsonic/mute/off", HTTP_POST, []()
    {
        if (viewSonic.setMute(false)) sendJsonOk("viewsonic/mute/off");
        else jsonError(502, "ViewSonic unmute command failed");
    });

    httpServer.on("/viewsonic/power/on", HTTP_POST, []()
    {
        if (viewSonic.setPowerOn()) sendJsonOk("viewsonic/power/on");
        else jsonError(502, "ViewSonic power-on command failed");
    });

    httpServer.on("/viewsonic/standby", HTTP_POST, []()
    {
        if (viewSonic.setStandby()) sendJsonOk("viewsonic/standby");
        else jsonError(502, "ViewSonic standby command failed");
    });

    httpServer.on("/system/audio/bootstrap", HTTP_POST, []()
    {
        if (audioBootstrapInProgress())
        {
            jsonError(409, "Audio bootstrap already in progress");
            return;
        }
        beginAudioBootstrap(true);
        sendJsonOk("system/audio/bootstrap");
    });

    httpServer.on("/system/startup/status", HTTP_GET, []()
    {
        String json = "{\"ok\":true,";
        json += "\"audio_stage\":\"" + String(audioBootstrapStageName()) + "\",";
        json += "\"audio_error\":\"" + audioBootstrapError + "\",";
        json += "\"klipsch_queue_depth\":" + String(klipschQueueDepth()) + ",";
        json += "\"viewsonic_last_ok\":" + String(viewSonic.lastOk() ? "true" : "false") + ",";
        json += "\"viewsonic_last_command\":\"" + viewSonic.lastCommand() + "\",";
        json += "\"viewsonic_last_reply\":\"" + viewSonic.lastReply() + "\"}";
        httpServer.send(200, "application/json", json);
    });

    httpServer.on("/system/shutdown", HTTP_POST, []()
    {
        // CEC "display off -> player off" is enabled on the ViewSonic. Putting
        // the panel into standby therefore gives attached CEC-capable sources
        // a clean appliance-style shutdown signal before station AC is removed.
        if (viewSonic.setStandby()) sendJsonOk("system/shutdown");
        else jsonError(502, "ViewSonic standby command failed");
    });

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
    if (WIFI_STATUS_LED_AVAILABLE)
    {
        pinMode(WIFI_STATUS_LED_PIN, OUTPUT);
        setWiFiStatusLed(false);
        Serial.printf("WiFi status LED: GPIO%d (solid ON when connected)\n", WIFI_STATUS_LED_PIN);
    }
    else
    {
        Serial.println("WiFi status LED: no LED_BUILTIN defined for this board");
    }

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOSTNAME);
    WiFi.setAutoReconnect(true);

    Serial.printf("WiFi: connecting to SSID \"%s\" as %s\n", WIFI_SSID, HOSTNAME);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    lastWiFiDiagMs = millis();
}

void processNetworkState()
{
    const wl_status_t status = WiFi.status();
    setWiFiStatusLed(status == WL_CONNECTED);

    if (static_cast<int>(status) != lastWiFiStatus)
    {
        Serial.printf("WiFi status: %s (%d)\n",
                      wifiStatusName(status),
                      static_cast<int>(status));
        lastWiFiStatus = static_cast<int>(status);
    }

    if (status != WL_CONNECTED)
    {
        const uint32_t now = millis();
        if (now - lastWiFiDiagMs >= WIFI_DIAG_INTERVAL_MS)
        {
            Serial.printf("WiFi: still waiting for \"%s\"; status=%s (%d)\n",
                          WIFI_SSID,
                          wifiStatusName(status),
                          static_cast<int>(status));
            lastWiFiDiagMs = now;
        }
        return;
    }

    if (!networkServicesStarted)
    {
        ArduinoOTA.setHostname(HOSTNAME);
        ArduinoOTA.begin();

        tcpServer.begin();
        tcpServer.setNoDelay(true);
        httpServer.begin();
        networkServicesStarted = true;

        Serial.printf("Hostname: %s\n", HOSTNAME);
        Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("WiFi RSSI: %d dBm, channel %d\n", WiFi.RSSI(), WiFi.channel());
        Serial.println("Arduino OTA ready");
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
    // External HDMI switch GPIO21/27/32/33 are not driven by this firmware.
    irReceiver.enableIRIn(); // GPIO26 optional optical 38 kHz receiver OUT (not wire loopback).
    Serial.println("Optional OPTICAL diagnostic receiver on GPIO26 (independent of wired TX)");

    Serial.println();
    Serial.printf("ShopShell ViewSonic + Klipsch (%s)\n", FIRMWARE_ID);

    // Klipsch boot audio initializes locally, even without Wi-Fi.
    beginAudioBootstrap();

    configureHttpRoutes();
    beginWiFi();
}

void loop()
{
    processNetworkState();
    processAudioBootstrap();

    if (networkServicesStarted)
    {
        ArduinoOTA.handle();
        httpServer.handleClient();
    }

    processTcpBridge();
    processUsbPassthrough();
    processSerialResponses();
    processIrReceiver();
    processKlipschWired(); // ESP32 RMT: carrier-free demodulated pulse envelope

    delay(1);
}
