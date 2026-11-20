#pragma once
#include <WiFi.h>
// Panel-confirmed ViewSonic DHCP address; reserved as ViewSonic Shop in Google Home.
// Verify the TCP control protocol at port 5000 before using power commands.
static const IPAddress VIEWSONIC_IP(192, 168, 86, 81);
static constexpr uint16_t VIEWSONIC_TCP_PORT = 5000;
static constexpr char FIRMWARE_ID[] = "ShopShell_ViewSonic_Klipsch_v3.2";
