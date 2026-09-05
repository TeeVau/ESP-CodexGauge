# Authentication and Stored Credentials

ESP CodexGauge uses the OpenAI device-login flow directly from the ESP8266. No API key is used.
This description applies to firmware release `0.8.3`.

## First login

1. Upload the firmware and open the Serial Monitor at `115200 baud`.
2. Wait for Wi-Fi, NTP time synchronization, and TLS initialization.
3. Open <https://auth.openai.com/codex/device>.
4. Sign in with the account whose ChatGPT Work / Codex usage should be displayed.
5. Enter the device code shown on the OLED or Serial Monitor.
6. Leave the device powered while it polls for authorization.

The device-login polling window is up to 15 minutes. After the OAuth exchange succeeds, the firmware extracts the account ID from the ID token and stores the account ID together with the refresh token in LittleFS.

## What is stored

The firmware stores an internal file at `/openai_auth.txt` containing:

- the file format version
- the ChatGPT account ID
- the refresh token

The access token is kept in RAM only. A temporary OAuth response file is removed after parsing. Refresh-token rotation is supported and a newly returned refresh token replaces the old one.

The stored refresh token is plaintext in ESP8266 flash. Physical flash access must therefore be treated as access to the monitored account.

## Reboots and recovery

After a reboot, the device loads the stored authentication and refreshes the access token. It automatically retries temporary failures. If the refresh token is invalid, revoked, reused, or expired, the stored authentication is cleared and a new device login is started.

Temporary refresh failures do not immediately delete the stored login. The
main loop retries them after about 60 seconds. A successful delayed refresh
immediately starts a usage cycle; a permanent refresh failure clears the
stored file and falls back to a fresh device login.

## Force a new login

In `ESP-CodexGauge.ino`, temporarily change:

```cpp
constexpr bool FORCE_RELOGIN = true;
```

Flash once and complete the login. Then set the value back to `false` and flash again. Do not leave it enabled for normal operation.

`FORCE_RELOGIN` is a compile-time setting. It does not expose a menu or a
runtime command, so remember to upload the normal `false` build afterwards.

## Safe diagnostics

Serial output may contain an abbreviated account identifier and device-login code. With `DEBUG_HTTP` enabled it may also contain HTTP response data. Before sharing logs, remove:

- device-login codes
- account IDs and email addresses
- access tokens and refresh tokens
- complete HTTP response bodies
- Wi-Fi information

For security reports, follow [SECURITY.md](../SECURITY.md) instead of opening a public issue.
