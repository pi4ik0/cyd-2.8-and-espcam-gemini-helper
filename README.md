# SUD Flasher

Calculator-disguised assistant on an ESP32 CYD display + ESP32-CAM.
The website flashes both boards from the browser (ESP Web Tools).

- `cyd/`  firmware for the CYD ESP32-2432S028 display
- `cam/`  firmware for the ESP32-CAM
- `site/` the flasher page (the firmware binaries are built by GitHub Actions)

No passwords or API keys are stored in the code. Wi-Fi, PIN and the API key
are entered on the device after flashing.

Every push to `main` rebuilds the firmware and redeploys the site.
