# watchy-firmware

Custom firmware for a Watchy v3 (ESP32-S3): SQFMI 7_SEG face reworked into a solar-ring face (12-hour clock, Fahrenheit, sunrise/sunset, light menus), saved Wi-Fi and clock across resets, USB Flash Mode, and an encrypted ESP-NOW link that receives texts from a T-Deck Pro (see tdeck-pro-nav).

Setup: put your weather key in `src/settings.h` (`OPENWEATHERMAP_APIKEY`) and your location; copy `lib/Watchy/src/WatchyLinkSecret.h.example` to `WatchyLinkSecret.h` and fill in random keys (same file on the T-Deck). Build with `pio run`, flash with `flash.sh`. The Watchy library in `lib/Watchy` is SQFMI's, with my edits, under its own license.
