# METAR Panel (M5Stack Core2), N110VT

Source: `metar_panel/metar_panel.ino`. Every push that changes it triggers GitHub Actions,
which compiles the sketch (M5Stack board package, board `M5Core2`, libraries M5Unified +
ArduinoJson) and commits two images to `build/`:

| File | Flash address | Use |
|---|---|---|
| `metar_panel_update_vX.bin` | `0x10000` | Normal updates. Keeps Wi-Fi and settings. |
| `metar_panel_core2_full_vX.bin` | `0x0` | Blank or messed-up Core2 only. Wipes settings. |

Flash from Chrome with https://espressif.github.io/esptool-js/.

The working copy also lives in `~/Documents/Metar project` on Dan's Mac.
