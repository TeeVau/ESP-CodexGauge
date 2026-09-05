# Troubleshooting

Start with the Serial Monitor at `115200 baud`. The firmware reports the failing stage as WLAN, NTP, TLS, AUTH, USAGE, or OLED.

## Compilation

### `secrets.h: No such file or directory`

Copy `secrets.example.h` to `secrets.h` in the same directory as the `.ino` file. Keep the names exactly as shown:

```cpp
constexpr char WIFI_SSID[] = "your-wifi-name";
constexpr char WIFI_PASSWORD[] = "your-wifi-password";
```

Never commit `secrets.h` or replace the placeholders in `secrets.example.h` with real credentials.

### ArduinoJson or Adafruit headers are missing

Install these libraries through the Arduino IDE Library Manager:

- ArduinoJson 6.x
- Adafruit GFX Library
- Adafruit SSD1306

Also verify that the ESP8266 board package is installed and that the selected board is `LOLIN(WEMOS) D1 R2 & mini`.

## Upload and boot

### Upload cannot connect

Select the correct USB port, use a data-capable USB cable, and close other serial tools. If necessary, hold the D1 mini reset/boot control during the upload connection phase.

### The OLED is blank

Check:

1. OLED VCC is on `3V3` and GND on `G`.
2. OLED SDA is on `D2` / GPIO4.
3. OLED SCL is on `D1` / GPIO5.
4. The module uses a compatible SSD1306 128×32 controller.
5. The module responds at `0x3C` or `0x3D`.

The firmware continues without an OLED. Use Serial output to distinguish a display problem from a network or authentication problem.

## Network, time, and TLS

### WLAN timeout

Check the case-sensitive SSID and password in `secrets.h`, 2.4 GHz availability, signal strength, and USB power. The firmware waits up to 30 seconds for a connection and reconnects later when possible.

### NTP timeout or TLS certificate failure

TLS certificate validation requires a plausible system clock. Keep the ESP8266 connected to the Internet and check that DNS can resolve the authentication host. Do not disable certificate verification as a workaround.

## Device login and usage

### The device code is rejected or times out

Use the current code shown by the device, open the current URL printed in Serial output, and complete the login within the 15-minute polling window. Check DNS and network access if the device receives no response.

### A stored login no longer works

The refresh token may have expired, been revoked, or been reused. Enable `FORCE_RELOGIN`, flash once, complete a new login, disable the flag, and flash again. See [AUTHENTICATION.md](AUTHENTICATION.md).

### The gauge still shows the previous value

Temporary HTTP/API failures preserve the last valid value. If it is older than 15 minutes, the `5H` and `WK` labels are inverted to mark stale data. Check the Serial Monitor for the HTTP status and failure count.

### The value does not change exactly at reset

The countdown is calculated locally. Once `reset_at` is reached, the firmware waits 30 seconds and performs one special usage refresh. This avoids waiting for the next regular five-minute poll while allowing the backend reset to settle.

### The gauge shows `0%`

`0%` means the returned remaining quota is empty. When a reset timestamp is available, the OLED shows the next availability time inside the tank. This is not the same as stale data or missing data; those states are reported separately.

## Reporting a bug

Before opening an issue, record the firmware version, board, ESP8266 core version, library versions, wiring, and the relevant redacted Serial output. Never include credentials, tokens, account IDs, email addresses, device codes, or complete HTTP responses.
