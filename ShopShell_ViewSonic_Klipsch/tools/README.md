# ShopShell read-only commissioning

From the firmware project directory on ShopDell:

```powershell
.\tools\ReadOnly-Commission.ps1 -ControllerIp 192.168.86.XX
```

Replace XX with the *actual ESP32 address* printed over USB serial. This script sends GET requests only. The legacy external HDMI-switch HTML test console has been retired.
