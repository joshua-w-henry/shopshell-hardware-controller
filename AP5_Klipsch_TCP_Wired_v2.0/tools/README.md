# ShopShell Hardware Controller test console

Open `ShopShell_Hardware_Controller_Test_Console.html` directly in Edge or Chrome on the ShopShell Dell after flashing firmware v2.1.

Default controller address: `192.168.87.42`.

The console provides manual acceptance tests for:

- HDMI Input 1 / Dell / ShopShell — GPIO27, remote pin 1.
- HDMI Input 2 / PS4 — GPIO32, remote pin 3.
- HDMI Input 3 / Apple TV — GPIO33, remote pin 2.
- HDMI Cycle — GPIO21, remote pin 4.
- Existing Klipsch R-15PM wired NEC controls.
- `/health`, `/hdmi/status`, Klipsch status/catalog, and optional GPIO26 IR diagnostics.
- General RS-232 TCP listener check on port 2323.

The HTML is intentionally standalone: no CDN, no local server, no Home Assistant dependency. Button commands are sent with ordinary HTML POST forms into an iframe because a `file://` page cannot reliably `fetch()` cross-origin ESP32 JSON without CORS headers.

`ok:true` means the ESP32 accepted the request. HDMI selection and Klipsch behavior still require visual/audible observation because neither device provides feedback to this controller.