# Security Policy

This policy applies to the public `0.8.3` release and later revisions unless a
newer policy is published.

## Scope

ESP CodexGauge handles Wi-Fi credentials, OAuth tokens, and a ChatGPT account identifier on a physical ESP8266. The usage endpoint is undocumented and may change without notice.

The refresh token and account ID are stored as plaintext in LittleFS. Anyone with physical flash access may be able to recover them.

The embedded OAuth client ID is a public application identifier used by the
device-login flow; it is not a replacement for a user secret. User Wi-Fi and
OAuth credentials must remain local.

## Do not disclose secrets

Never publish or attach:

- `secrets.h`
- Wi-Fi passwords
- access tokens or refresh tokens
- device-login codes
- account IDs or email addresses
- complete HTTP response bodies
- unredacted Serial Monitor logs

The repository ignores `secrets.h`, but local Git ignore rules are not a substitute for checking the staged files before a commit.

## Reporting a vulnerability

Do not open a public issue for exposed credentials or token-handling vulnerabilities. Use GitHub's private security reporting if enabled for the repository. If private reporting is unavailable, contact the repository owner privately and include only the minimum information needed to reproduce the problem.

Please allow time for validation before publicly disclosing a security issue.
