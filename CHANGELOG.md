# Changelog

## 0.8.3 — 2026-09-05

- Added reset-aware polling based on the backend `reset_at` timestamp.
- A one-time usage refresh is triggered 30 seconds after a known five-hour or weekly quota reset.
- One request covers both windows if both resets become due together.
- The special reset refresh becomes the new anchor for the normal five-minute polling interval.
- Duplicate special refreshes for the same `reset_at` value are suppressed.
- Network hardening and the `AUTH OK` OLED state remain included.
- Completed the public maker documentation for hardware, authentication,
  troubleshooting, contributing, and security.
- Added GitHub Actions compilation for `esp8266:esp8266:d1_mini` and a
  generated GitHub social-preview asset under 1 MB.
- Pinned the CI build to the currently available ESP8266 Arduino Core `3.1.2`.

## 0.8.2 — 2026-09-05

- Added an explicit OLED authentication-success state: `OPENAI / AUTH OK`.
- The state is shown after a successful stored-token refresh, delayed auth recovery, or fresh device login.
- `AUTH OK` remains visible until the first successful usage cycle replaces it with the fuel-gauge screen.
- OAuth, token rotation, TLS, polling cadence, and quota parsing are unchanged.

## 0.8.1 — 2026-09-05

- Added a short network stabilization delay after a fresh Wi-Fi connection.
- Added DNS readiness checking for `auth.openai.com` before the first OAuth attempt.
- DNS readiness failure does not invalidate Wi-Fi; the existing auth retry remains the fallback.
- After a delayed OAuth refresh succeeds, usage is fetched immediately instead of waiting for the next five-minute interval.
- Startup banner, empty-quota reset display, OAuth, TLS, and usage logic otherwise remain unchanged.

## 0.8.0 — 2026-09-05

This entry consolidates the earlier `1.0.0–1.0.2` notes from the pre-publication source package into the consistent `0.8.x` history.

- Initial public ESP CodexGauge release.
- Added standalone OpenAI device OAuth on ESP8266.
- Added persistent LittleFS refresh-token storage and token rotation.
- Added verified TLS 1.2 / X.509 connections.
- Added direct ChatGPT Work / Codex usage retrieval.
- Added five-hour and weekly remaining-quota fuel gauges.
- Empty quota shows its reset time directly inside the tank.
- Added the 0.91-inch SSD1306 128×32 OLED layout and startup banner.
- Added Europe/Berlin CET/CEST handling.
- Added automatic Wi-Fi/auth recovery and five-minute polling.
