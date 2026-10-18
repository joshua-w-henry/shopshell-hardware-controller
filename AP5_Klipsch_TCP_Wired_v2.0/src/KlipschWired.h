#pragma once
#include <Arduino.h>

// Drives a 2N2222 base on GPIO25: HIGH = transistor ON = Klipsch IR data LOW.
// Signal is demodulated NEC pulse envelope: NO 38 kHz carrier.
struct KlipschCommand {
    const char *path;
    const char *label;
    uint8_t command;
    bool repeatable;
    const char *source;
};

bool beginKlipschWired();
void processKlipschWired();
size_t klipschCommandCount();
const KlipschCommand *klipschCommandAt(size_t index);
const KlipschCommand *findKlipschCommand(String name);
uint32_t klipschRawNEC(uint8_t command);
bool queueKlipschWired(uint8_t command, uint8_t count = 1, uint8_t repeats = 0);
uint8_t klipschQueueDepth();
uint32_t klipschTotalSent();
uint32_t klipschRepeatFramesSent();
uint8_t klipschLastSentCommand();
uint32_t klipschLastSentMillis();
bool klipschTxReady();
