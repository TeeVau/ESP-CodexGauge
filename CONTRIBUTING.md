# Contributing

Thanks for helping improve ESP CodexGauge. Documentation corrections, wiring feedback, compatibility reports, and small firmware fixes are welcome.

## Before opening an issue or pull request

- Search existing issues first.
- State the board model, ESP8266 core version, library versions, and wiring.
- Describe the expected and actual behavior.
- Include only relevant Serial Monitor output at 115200 baud.
- Remove Wi-Fi credentials, OAuth tokens, refresh tokens, account IDs, email addresses, device codes, and HTTP response bodies.

## Pull requests

1. Fork the repository and create a focused branch.
2. Keep changes small and explain the user-visible effect.
3. Do not commit `secrets.h`, build output, firmware binaries, or private logs.
4. Verify that the sketch compiles for `esp8266:esp8266:d1_mini`.
5. Update the README or troubleshooting documentation when behavior changes.
6. Explain hardware and authentication testing in the pull request.

The GitHub Actions compile check must pass before a pull request can be merged. Hardware-dependent behavior should be clearly identified when it cannot be tested by CI.
