#pragma once
#include <WiFi.h>
// Shangri La candidate: verify .86.33 really is the ViewSonic before control.
// Reserve this address in the router before relying on shutdown.
static const IPAddress VIEWSONIC_IP(192, 168, 86, 33);
static constexpr uint16_t VIEWSONIC_TCP_PORT = 5000;
static constexpr char FIRMWARE_ID[] = "ShopShell_ViewSonic_Klipsch_v3.0";
