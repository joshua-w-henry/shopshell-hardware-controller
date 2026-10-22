#include <Arduino.h>
#include <driver/rmt.h>
#include <driver/gpio.h>
#include "KlipschWired.h"

// =========================================
// ACTIVE-LOW WIRED NEC, NOT INFRARED PWM
// =========================================
// GPIO25 HIGH -> base resistor -> NPN ON -> collector pulls IR_DATA LOW.
// GPIO25 LOW  -> NPN OFF -> factory circuit pulls IR_DATA HIGH.
// Keep NPN off on boot or firmware failure (external ~100k base pulldown recommended).
// Isolate/protect any factory IR receiver output before tying in; many receivers
// actively drive HIGH and can contend with a pull-down transistor.
namespace {
constexpr gpio_num_t TX_PIN = GPIO_NUM_25;
constexpr rmt_channel_t TX_CHANNEL = RMT_CHANNEL_0;
constexpr uint16_t NEC_ADDRESS = 0x02;
constexpr uint8_t MAX_QUEUE = 96;
constexpr uint32_t FIRST_TO_NEXT_REPEAT_MS = 110;
constexpr uint32_t INTER_PRESS_MS = 135;
constexpr uint16_t NEC_LEADER_MARK_US = 9000;
constexpr uint16_t NEC_LEADER_SPACE_US = 4500;
constexpr uint16_t NEC_BIT_MARK_US = 560;
constexpr uint16_t NEC_ZERO_SPACE_US = 560;
constexpr uint16_t NEC_ONE_SPACE_US = 1690;
constexpr uint16_t NEC_REPEAT_SPACE_US = 2250;

// SOURCE: inherited v1.0 command list. Logical NEC address 0x02;
// original R-15PM next-track command is unverified and intentionally omitted.
const KlipschCommand commands[] = {
    {"power",            "Power toggle",         0x0A, false, "klipsch"},
    {"mute",             "Mute toggle",          0x0D, false, "klipsch"},
    {"volume/up",        "Volume up",            0x1C, true,  "klipsch"},
    {"volume/down",      "Volume down",          0x07, true,  "klipsch"},
    {"sub/up",           "Subwoofer level up",   0x0C, true,  "klipsch"},
    {"sub/down",         "Subwoofer level down", 0x04, true,  "klipsch"},
    {"sub/reset",        "Subwoofer reset",      0x03, false, "klipsch"},
    {"led",              "LED indicator toggle", 0x08, false, "klipsch"},
    {"media/play-pause", "Play / pause",         0x1E, false, "klipsch"},
    {"media/previous",   "Previous track",       0x02, true,  "flipper_r15pm"},
    {"input/phono",      "Phono source",         0x01, false, "klipsch"},
    {"input/aux",        "Auxiliary source",     0x09, false, "klipsch"},
    {"input/optical",    "Optical source",       0x00, false, "klipsch"},
    {"input/usb",        "USB source",           0x48, false, "klipsch"},
    {"input/bluetooth",  "Bluetooth source",     0x5C, false, "klipsch"},
};
static_assert(sizeof(commands) / sizeof(commands[0]) == 15, "Unexpected Klipsch command count");

struct QueuedNEC { uint8_t command; uint8_t repeats; };
QueuedNEC queue[MAX_QUEUE];
uint8_t qHead = 0, qTail = 0, qSize = 0;
bool txReady = false;
uint32_t nextEligibleMs = 0;
uint32_t nextRepeatMs = 0;
uint8_t repeatsRemaining = 0;
uint32_t pressesSent = 0;
uint32_t repeatFramesSent = 0;
uint32_t lastTxMs = 0;
uint8_t lastTxCmd = 0;

// NEC address/command logical bytes to same raw representation as
// IRremoteESP8266::encodeNEC(0x02, cmd). sendNEC sends raw MSB first.
uint8_t reverse8(uint8_t b) {
    b = static_cast<uint8_t>((b >> 4) | (b << 4));
    b = static_cast<uint8_t>(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
    b = static_cast<uint8_t>(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
    return b;
}

// RMT configured for 1 microsecond per tick, carrier OFF, GPIO25 idle LOW.
// A MARK means output HIGH to enable NPN, producing active LOW at the speaker.
rmt_item32_t item(uint16_t markUs, uint16_t spaceUs) {
    rmt_item32_t v = {};
    v.level0 = 1;
    v.duration0 = markUs;
    v.level1 = 0;
    v.duration1 = spaceUs;
    return v;
}

bool hardwareIdle() {
    return rmt_wait_tx_done(TX_CHANNEL, 0) == ESP_OK;
}

bool transmitFull(uint8_t cmd) {
    rmt_item32_t waveform[34];
    waveform[0] = item(NEC_LEADER_MARK_US, NEC_LEADER_SPACE_US);
    const uint32_t raw = klipschRawNEC(cmd);
    for (uint8_t i = 0; i < 32; ++i) {
        const bool bit = (raw >> (31U - i)) & 1U;
        waveform[i + 1] = item(NEC_BIT_MARK_US,
                              bit ? NEC_ONE_SPACE_US : NEC_ZERO_SPACE_US);
    }
    waveform[33] = item(NEC_BIT_MARK_US, 0); // trailing mark, then idle low
    return rmt_write_items(TX_CHANNEL, waveform, 34, false) == ESP_OK;
}

bool transmitRepeat() {
    const rmt_item32_t waveform[] = {
        item(NEC_LEADER_MARK_US, NEC_REPEAT_SPACE_US),
        item(NEC_BIT_MARK_US, 0)
    };
    return rmt_write_items(TX_CHANNEL, waveform, 2, false) == ESP_OK;
}
} // namespace

uint32_t klipschRawNEC(uint8_t command) {
    const uint8_t addr = reverse8(static_cast<uint8_t>(NEC_ADDRESS));
    const uint8_t cmd  = reverse8(command);
    return (static_cast<uint32_t>(addr) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(~addr)) << 16) |
           (static_cast<uint32_t>(cmd) << 8) |
           static_cast<uint8_t>(~cmd);
}

bool beginKlipschWired() {
    // Keep the base LOW while configuring the peripheral.
    digitalWrite(static_cast<uint8_t>(TX_PIN), LOW);
    pinMode(static_cast<uint8_t>(TX_PIN), OUTPUT);
    rmt_config_t config = {};
    config.rmt_mode = RMT_MODE_TX;
    config.channel = TX_CHANNEL;
    config.gpio_num = TX_PIN;
    config.mem_block_num = 1;  // 64 RMT items, NEC needs only 34
    config.clk_div = 80;      // ESP32 80 MHz APB -> 1 us/tick
    config.tx_config.loop_en = false;
    config.tx_config.carrier_en = false;  // CRITICAL: demodulated wire signal
    config.tx_config.idle_output_en = true;
    config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
    const esp_err_t c = rmt_config(&config);
    const esp_err_t i = c == ESP_OK ? rmt_driver_install(TX_CHANNEL, 0, 0) : c;
    txReady = (c == ESP_OK && i == ESP_OK);
    Serial.printf("Klipsch WIRED NEC GPIO25 active-high NPN, 38k carrier OFF, RMT %s\n",
                  txReady ? "ready" : "FAILED");
    Serial.printf("Klipsch example logical 0x02/0x0A -> raw 0x%08lX\n",
                  static_cast<unsigned long>(klipschRawNEC(0x0A)));
    return txReady;
}

size_t klipschCommandCount() { return sizeof(commands) / sizeof(commands[0]); }
const KlipschCommand *klipschCommandAt(size_t index) {
    return index < klipschCommandCount() ? &commands[index] : nullptr;
}
const KlipschCommand *findKlipschCommand(String key) {
    key.toLowerCase();
    key.replace('_', '-');
    if (key.startsWith("/klipsch/")) key.remove(0, 9);
    if (key == "power-toggle") key = "power";
    else if (key == "volume-up" || key == "vol-up" || key == "vol+") key = "volume/up";
    else if (key == "volume-down" || key == "vol-down" || key == "vol-") key = "volume/down";
    else if (key == "sub-up") key = "sub/up";
    else if (key == "sub-down") key = "sub/down";
    else if (key == "sub-reset") key = "sub/reset";
    else if (key == "led-toggle") key = "led";
    else if (key == "play" || key == "pause" || key == "play-pause" || key == "playpause") key = "media/play-pause";
    else if (key == "previous" || key == "prev") key = "media/previous";
    else if (key == "optical" || key == "digital" || key == "source/optical") key = "input/optical";
    else if (key == "bluetooth" || key == "bt" || key == "source/bluetooth") key = "input/bluetooth";
    else if (key == "phono" || key == "source/phono") key = "input/phono";
    else if (key == "aux" || key == "source/aux") key = "input/aux";
    else if (key == "usb" || key == "source/usb") key = "input/usb";
    for (const auto &c : commands) if (key == c.path) return &c;
    return nullptr;
}

bool queueKlipschWired(uint8_t command, uint8_t count, uint8_t repeats) {
    if (!txReady || !count || count > 10 || repeats > 5 || count > MAX_QUEUE - qSize) return false;
    for (uint8_t i = 0; i < count; ++i) {
        queue[qTail] = {command, repeats};
        qTail = (qTail + 1) % MAX_QUEUE;
        ++qSize;
    }
    return true;
}

void processKlipschWired() {
    if (!txReady) return;
    const uint32_t now = millis();
    if (repeatsRemaining) {
        if (static_cast<int32_t>(now - nextRepeatMs) < 0 || !hardwareIdle()) return;
        if (transmitRepeat()) {
            --repeatsRemaining;
            ++repeatFramesSent;
            nextRepeatMs = now + FIRST_TO_NEXT_REPEAT_MS;
            if (repeatsRemaining == 0) nextEligibleMs = now + INTER_PRESS_MS;
        }
        return;
    }
    if (!qSize || static_cast<int32_t>(now - nextEligibleMs) < 0 || !hardwareIdle()) return;
    const QueuedNEC tx = queue[qHead];
    if (!transmitFull(tx.command)) return;
    qHead = (qHead + 1) % MAX_QUEUE;
    --qSize;
    ++pressesSent;
    lastTxCmd = tx.command;
    lastTxMs = now;
    repeatsRemaining = tx.repeats;
    nextRepeatMs = now + FIRST_TO_NEXT_REPEAT_MS;
    nextEligibleMs = now + INTER_PRESS_MS;
    Serial.printf("WIRED NEC addr=0x02 cmd=0x%02X raw=0x%08lX repeats=%u queued=%u\n",
                  tx.command, static_cast<unsigned long>(klipschRawNEC(tx.command)),
                  tx.repeats, qSize);
}

uint8_t klipschQueueDepth() { return qSize; }
uint32_t klipschTotalSent() { return pressesSent; }
uint32_t klipschRepeatFramesSent() { return repeatFramesSent; }
uint8_t klipschLastSentCommand() { return lastTxCmd; }
uint32_t klipschLastSentMillis() { return lastTxMs; }
bool klipschTxReady() { return txReady; }
