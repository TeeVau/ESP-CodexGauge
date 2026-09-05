/*
  ESP CodexGauge v0.8.3
  Standalone ESP8266 fuel gauge for ChatGPT Work / Codex usage limits.

  Hardware:
    - Wemos D1 mini V3.0.0 / ESP8266
    - 0.91" SSD1306 OLED, 128x32, I2C

  Features:
    - Direct OpenAI Device OAuth; no PC, proxy or server required
    - Persistent refresh token in LittleFS with token rotation support
    - TLS 1.2 with X.509 certificate verification
    - 5-hour and weekly remaining quota shown as fuel gauges
    - Automatic CET/CEST handling for Europe/Berlin
    - 5-minute polling with local reset countdown
    - Automatic Wi-Fi, token and authentication recovery

  Security:
    - Wi-Fi credentials live in secrets.h (excluded from Git)
    - Refresh token is stored in plaintext in ESP8266 LittleFS
    - The usage endpoint is undocumented and may change

  License: MIT
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include <stdlib.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "secrets.h"

constexpr char PROJECT_NAME[] = "ESP CodexGauge";
constexpr char PROJECT_VERSION[] = "0.8.3";

// ============================================================
// Runtime configuration
// ============================================================

// Einmal auf true setzen, wenn die gespeicherte OpenAI-Anmeldung geloescht
// und ein neuer Device-Login erzwungen werden soll. Danach wieder false.
constexpr bool FORCE_RELOGIN = false;

// Diagnoseoptionen fuer die weitere Erprobung.
constexpr bool DEBUG_HEAP = false;
constexpr bool DEBUG_HTTP = false;

// OpenAI alle 5 Minuten abfragen. Reset-Countdown wird lokal aus reset_at
// berechnet und kann spaeter auf dem Display sekunden-/minutengenau laufen.
constexpr unsigned long USAGE_INTERVAL_MS = 300000UL;

// Wenn ein vom Backend gemeldeter Reset erreicht ist, wird 30 Sekunden
// spaeter einmalig ein zusaetzlicher Usage-Abruf ausgefuehrt. So muss die
// Anzeige nach einem Reset nicht im Worst Case fast 5 Minuten warten.
constexpr uint32_t RESET_REFRESH_DELAY_SEC = 30UL;

// Temporaere Auth-/Netzwerkfehler: nach 60 Sekunden erneut versuchen.
constexpr unsigned long AUTH_RETRY_MS = 60000UL;

// Nach einer frischen WLAN-Verbindung kurz warten und DNS pruefen, bevor
// der erste HTTPS/OAuth-Zugriff erfolgt. Das reduziert Timing-Probleme
// direkt nach einem Kaltstart oder WLAN-Reconnect.
constexpr unsigned long NETWORK_SETTLE_MS = 1500UL;
constexpr unsigned long NETWORK_DNS_TIMEOUT_MS = 4000UL;

// Access Token 5 Minuten vor Ablauf erneuern.
constexpr uint32_t TOKEN_REFRESH_MARGIN_SEC = 300UL;

// Ab diesem Unix-Zeitpunkt betrachten wir NTP als plausibel.
constexpr time_t MIN_VALID_UNIX_TIME = 1700000000;

// POSIX-Zeitzone fuer Deutschland / Europe-Berlin:
// CET = UTC+1, CEST = UTC+2; Umschaltung jeweils letzter Sonntag
// im Maerz um 02:00 und letzter Sonntag im Oktober um 03:00.
constexpr char TIMEZONE_POSIX[] = "CET-1CEST,M3.5.0/2,M10.5.0/3";

// ============================================================
// OLED 0.91" / 128x32 / I2C
// ============================================================

// Wemos D1 mini: D2 = SDA (GPIO4), D1 = SCL (GPIO5)
constexpr uint8_t OLED_SDA_PIN = D2;
constexpr uint8_t OLED_SCL_PIN = D1;
constexpr uint8_t OLED_ADDRESS_PRIMARY = 0x3C;
constexpr uint8_t OLED_ADDRESS_FALLBACK = 0x3D;
constexpr uint8_t OLED_WIDTH = 128;
constexpr uint8_t OLED_HEIGHT = 32;
constexpr unsigned long OLED_REFRESH_MS = 1000UL;

// Tank-Geometrie: Label | Tank | Rest-%
constexpr int16_t OLED_LABEL_X = 0;
constexpr int16_t OLED_BAR_X = 15;
constexpr int16_t OLED_BAR_W = 83;
constexpr int16_t OLED_BAR_H = 11;
constexpr int16_t OLED_ROW1_Y = 2;
constexpr int16_t OLED_ROW2_Y = 18;

// Warnschwellen fuer verbleibendes Kontingent
constexpr uint8_t OLED_WARNING_PERCENT = 25;
constexpr uint8_t OLED_CRITICAL_PERCENT = 10;

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
bool oledReady = false;
uint8_t oledAddress = 0;
unsigned long lastOledRefreshMs = 0;
bool oledBlinkPhase = false;

// ============================================================
// OpenAI / Codex
// ============================================================

// Public OAuth client ID used by the Codex device-login flow.
// This is not a user credential or secret.
constexpr char OPENAI_CLIENT_ID[] = "app_EMoamEEZ73f0CkXaXp7hrann";
constexpr char OPENAI_AUTH_HOST[] = "auth.openai.com";

const char* DEVICE_USERCODE_URL =
  "https://auth.openai.com/api/accounts/deviceauth/usercode";

const char* DEVICE_TOKEN_URL =
  "https://auth.openai.com/api/accounts/deviceauth/token";

const char* DEVICE_VERIFY_URL =
  "https://auth.openai.com/codex/device";

const char* OAUTH_TOKEN_URL =
  "https://auth.openai.com/oauth/token";

const char* DEVICE_REDIRECT_URI =
  "https://auth.openai.com/deviceauth/callback";

const char* USAGE_URL =
  "https://chatgpt.com/backend-api/wham/usage";

const char* AUTH_FILE = "/openai_auth.txt";
const char* OAUTH_TMP_FILE = "/oauth_response.tmp";

// ============================================================
// TLS Root CAs (PROGMEM)
// ============================================================

static const char ISRG_ROOT_X1[] PROGMEM = R"CERT(-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)CERT";

static const char ISRG_ROOT_X2[] PROGMEM = R"CERT(-----BEGIN CERTIFICATE-----
MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw
CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg
R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00
MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT
ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw
EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW
+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9
ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T
AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI
zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW
tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1
/q4AaOeMSQ+2b1tbFfLn
-----END CERTIFICATE-----
)CERT";

static const char GTS_ROOT_R4[] PROGMEM = R"CERT(-----BEGIN CERTIFICATE-----
MIICCTCCAY6gAwIBAgINAgPlwGjvYxqccpBQUjAKBggqhkjOPQQDAzBHMQswCQYD
VQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEUMBIG
A1UEAxMLR1RTIFJvb3QgUjQwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAwMDAw
WjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2Vz
IExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjQwdjAQBgcqhkjOPQIBBgUrgQQAIgNi
AATzdHOnaItgrkO4NcWBMHtLSZ37wWHO5t5GvWvVYRg1rkDdc/eJkTBa6zzuhXyi
QHY7qca4R9gq55KRanPpsXI5nymfopjTX15YhmUPoYRlBtHci8nHc8iMai/lxKvR
HYqjQjBAMA4GA1UdDwEB/wQEAwIBhjAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQW
BBSATNbrdP9JNqPV2Py1PsVq8JQdjDAKBggqhkjOPQQDAwNpADBmAjEA6ED/g94D
9J+uHXqnLrmvT/aDHQ4thQEd0dlq7A/Cr8deVl5c1RxYIigL9zC2L7F8AjEA8GE8
p/SgguMh1YQdc4acLa/KNJvxn7kjNuK8YAOdgLOaVsjh4rsUecrNIdSUtUlD
-----END CERTIFICATE-----
)CERT";

BearSSL::X509List* openaiTrustAnchors = nullptr;

// ============================================================
// Display-unabhaengiges Datenmodell
// ============================================================

struct UsageWindow {
  bool valid = false;
  float usedPercent = 0.0f;
  float remainingPercent = 0.0f;
  uint32_t windowSeconds = 0;
  time_t resetAt = 0;
};

struct UsageData {
  bool valid = false;                 // mindestens ein erfolgreicher Abruf
  String plan;
  bool allowed = false;
  bool limitReached = false;

  UsageWindow fiveHour;
  UsageWindow weekly;
  UsageWindow otherPrimary;
  UsageWindow otherSecondary;

  bool hasCredits = false;
  bool unlimitedCredits = false;
  String creditBalance;

  int resetCreditsAvailable = 0;
  int resetCreditsApplicable = 0;

  time_t fetchedAt = 0;               // letzter erfolgreicher Abruf
  time_t lastAttemptAt = 0;           // letzter Versuch
  int lastHttpCode = 0;
  uint16_t consecutiveFailures = 0;
};

enum class RefreshResult {
  OK,
  TEMPORARY_FAILURE,
  PERMANENT_FAILURE
};

// ============================================================
// Globale Laufzeitdaten
// ============================================================

String accessToken;
String refreshToken;
String accountId;
uint32_t accessTokenExpiresAt = 0;

UsageData usage;

unsigned long lastUsagePollMs = 0;
unsigned long lastAuthRetryMs = 0;
uint32_t usageCycle = 0;

// Verhindert wiederholte Sonderabrufe fuer denselben reset_at-Wert.
// Wenn OpenAI nach dem Reset ein neues Fenster liefert, aendert sich resetAt
// und der naechste Reset kann wieder genau einmal getriggert werden.
time_t lastResetRefreshFiveHourAt = 0;
time_t lastResetRefreshWeeklyAt = 0;

// OLED function prototypes
bool initOLED();
void oledStatus(const String& line1, const String& line2 = "");
void oledShowStartupBanner();
void oledShowLoginCode(const String& userCode);
void updateOLED(bool force = false);

// ============================================================
// Hilfsfunktionen: Heap / Zeit / Strings
// ============================================================

void printHeapStats(const __FlashStringHelper* label) {
  if (!DEBUG_HEAP) return;

  Serial.println();
  Serial.print(F("[HEAP] "));
  Serial.println(label);
  Serial.print(F("  Free Heap:       "));
  Serial.println(ESP.getFreeHeap());
  Serial.print(F("  Max Free Block:  "));
  Serial.println(ESP.getMaxFreeBlockSize());
  Serial.print(F("  Fragmentierung:  "));
  Serial.print(ESP.getHeapFragmentation());
  Serial.println(F(" %"));
}

bool timeIsValid() {
  return time(nullptr) >= MIN_VALID_UNIX_TIME;
}

String formatUnixTime(time_t timestamp) {
  if (timestamp <= 0) return "unbekannt";

  struct tm timeinfo;
  localtime_r(&timestamp, &timeinfo);

  char buffer[32];
  strftime(buffer, sizeof(buffer), "%d.%m.%Y %H:%M:%S", &timeinfo);
  return String(buffer);
}

String formatDuration(uint32_t seconds) {
  uint32_t days = seconds / 86400UL;
  seconds %= 86400UL;
  uint32_t hours = seconds / 3600UL;
  seconds %= 3600UL;
  uint32_t minutes = seconds / 60UL;

  String result;
  result.reserve(32);

  if (days > 0) {
    result += String(days);
    result += F("d ");
  }
  if (hours > 0 || days > 0) {
    result += String(hours);
    result += F("h ");
  }
  result += String(minutes);
  result += F("m");
  return result;
}

uint32_t secondsUntil(time_t target) {
  if (target <= 0 || !timeIsValid()) return 0;
  time_t now = time(nullptr);
  if (target <= now) return 0;
  return (uint32_t)(target - now);
}

String urlEncode(const String& input) {
  String encoded;
  encoded.reserve(input.length() * 2);
  const char hex[] = "0123456789ABCDEF";

  for (size_t i = 0; i < input.length(); i++) {
    unsigned char c = input[i];
    if ((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += (char)c;
    } else {
      encoded += '%';
      encoded += hex[c >> 4];
      encoded += hex[c & 0x0F];
    }
    yield();
  }
  return encoded;
}

// ============================================================
// JWT Base64URL
// ============================================================

int base64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

String base64UrlDecode(const String& input) {
  String output;
  output.reserve((input.length() * 3) / 4 + 4);

  int val = 0;
  int valb = -8;

  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c == '=') break;

    int d = base64Value(c);
    if (d < 0) continue;

    val = (val << 6) | d;
    valb += 6;

    if (valb >= 0) {
      output += char((val >> valb) & 0xFF);
      valb -= 8;
    }
    yield();
  }

  return output;
}

String getJwtPayload(const String& jwt) {
  int firstDot = jwt.indexOf('.');
  if (firstDot < 0) return "";

  int secondDot = jwt.indexOf('.', firstDot + 1);
  if (secondDot < 0) return "";

  return base64UrlDecode(jwt.substring(firstDot + 1, secondDot));
}

String extractAccountId(const String& jwt) {
  String payload = getJwtPayload(jwt);
  if (payload.length() == 0) return "";

  StaticJsonDocument<256> filter;
  filter["https://api.openai.com/auth"]["chatgpt_account_id"] = true;
  filter["chatgpt_account_id"] = true;

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(
    doc, payload, DeserializationOption::Filter(filter));
  payload = "";

  if (error) return "";

  const char* id = doc["https://api.openai.com/auth"]["chatgpt_account_id"];
  if (id && strlen(id) > 0) return String(id);

  id = doc["chatgpt_account_id"];
  if (id && strlen(id) > 0) return String(id);

  return "";
}

uint32_t extractJwtExpiry(const String& jwt) {
  String payload = getJwtPayload(jwt);
  if (payload.length() == 0) return 0;

  StaticJsonDocument<32> filter;
  filter["exp"] = true;

  DynamicJsonDocument doc(128);
  DeserializationError error = deserializeJson(
    doc, payload, DeserializationOption::Filter(filter));
  payload = "";

  if (error) return 0;
  return doc["exp"] | 0UL;
}

// ============================================================
// LittleFS: Refresh-Token + Account-ID
// ============================================================

bool initFilesystem() {
  Serial.println();
  Serial.println(F("========================================"));
  Serial.println(F(" LITTLEFS"));
  Serial.println(F("========================================"));

  if (LittleFS.begin()) {
    Serial.println(F("LittleFS: OK"));
    return true;
  }

  Serial.println(F("LittleFS konnte nicht gemountet werden."));
  Serial.println(F("Formatiere Dateisystem..."));

  if (!LittleFS.format() || !LittleFS.begin()) {
    Serial.println(F("FEHLER: LittleFS nicht verfuegbar"));
    return false;
  }

  Serial.println(F("LittleFS formatiert und gemountet."));
  return true;
}

void clearStoredAuth() {
  if (LittleFS.exists(AUTH_FILE)) LittleFS.remove(AUTH_FILE);

  refreshToken = "";
  accountId = "";
  accessToken = "";
  accessTokenExpiresAt = 0;

  Serial.println(F("[AUTH] Gespeicherte Anmeldung geloescht."));
}

bool saveStoredAuth() {
  if (refreshToken.length() == 0 || accountId.length() == 0) {
    Serial.println(F("[AUTH] Speichern abgebrochen: Daten unvollstaendig."));
    return false;
  }

  File f = LittleFS.open(AUTH_FILE, "w");
  if (!f) {
    Serial.println(F("[AUTH] FEHLER: Auth-Datei nicht schreibbar."));
    return false;
  }

  f.println(F("version=1"));
  f.print(F("account_id="));
  f.println(accountId);
  f.print(F("refresh_token="));
  f.println(refreshToken);
  f.close();

  Serial.println(F("[AUTH] Refresh-Token und Account-ID gespeichert."));
  return true;
}

bool loadStoredAuth() {
  if (!LittleFS.exists(AUTH_FILE)) {
    Serial.println(F("[AUTH] Keine gespeicherte Anmeldung vorhanden."));
    return false;
  }

  File f = LittleFS.open(AUTH_FILE, "r");
  if (!f) {
    Serial.println(F("[AUTH] Auth-Datei kann nicht gelesen werden."));
    return false;
  }

  String loadedAccountId;
  String loadedRefreshToken;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();

    if (line.startsWith("account_id=")) {
      loadedAccountId = line.substring(strlen("account_id="));
    } else if (line.startsWith("refresh_token=")) {
      loadedRefreshToken = line.substring(strlen("refresh_token="));
    }
    yield();
  }
  f.close();

  if (loadedAccountId.length() == 0 || loadedRefreshToken.length() == 0) {
    Serial.println(F("[AUTH] Auth-Datei ist unvollstaendig."));
    return false;
  }

  accountId = loadedAccountId;
  refreshToken = loadedRefreshToken;
  loadedAccountId = "";
  loadedRefreshToken = "";

  Serial.print(F("[AUTH] Gespeicherte Anmeldung geladen. Account: "));
  if (accountId.length() > 8) {
    Serial.print(accountId.substring(0, 8));
    Serial.println(F("..."));
  } else {
    Serial.println(F("[vorhanden]"));
  }

  return true;
}

// ============================================================
// WLAN / Zeit
// ============================================================

bool waitForAuthNetworkReady() {
  if (WiFi.status() != WL_CONNECTED) return false;

  Serial.print(F("[NET] Stabilisiere Netzwerk"));
  delay(NETWORK_SETTLE_MS);

  const unsigned long start = millis();
  IPAddress resolvedIp;

  do {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println(F(" -> WLAN verloren"));
      return false;
    }

    if (WiFi.hostByName(OPENAI_AUTH_HOST, resolvedIp) == 1) {
      Serial.print(F(" -> DNS OK ("));
      Serial.print(resolvedIp);
      Serial.println(F(")"));
      return true;
    }

    Serial.print('.');
    delay(250);
    yield();
  } while (millis() - start < NETWORK_DNS_TIMEOUT_MS);

  Serial.println(F(" -> DNS noch nicht bereit; Recovery uebernimmt."));
  return false;
}

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.println();
  Serial.println(F("========================================"));
  Serial.println(F(" WLAN"));
  Serial.println(F("========================================"));

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print(F("Verbinde"));
  unsigned long start = millis();

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');

    if (millis() - start > 30000UL) {
      Serial.println();
      Serial.println(F("FEHLER: WLAN Timeout"));
      return false;
    }
  }

  Serial.println();
  Serial.print(F("Verbunden: "));
  Serial.println(WiFi.SSID());
  Serial.print(F("IP:        "));
  Serial.println(WiFi.localIP());
  Serial.print(F("RSSI:      "));
  Serial.print(WiFi.RSSI());
  Serial.println(F(" dBm"));

  // Ein DNS-Fehler hier macht die WLAN-Verbindung nicht ungueltig.
  // Der bestehende Auth-Recovery-Mechanismus bleibt die Rueckfallebene.
  waitForAuthNetworkReady();

  return true;
}

void applyBerlinTimezone() {
  // Wichtig: Zeitzone immer setzen, auch wenn die Unix-Zeit nach einem
  // Reboot bereits gueltig ist. Sonst bleibt localtime() ggf. auf UTC.
  setenv("TZ", TIMEZONE_POSIX, 1);
  tzset();
}

bool setupTime() {
  // Zeitzone MUSS vor dem Early-Return gesetzt werden.
  applyBerlinTimezone();

  if (timeIsValid()) {
    Serial.print(F("[TIME] Systemzeit bereits gueltig: "));
    Serial.println(formatUnixTime(time(nullptr)));
    return true;
  }

  Serial.println();
  Serial.println(F("[NTP] Zeitsynchronisation..."));

  // Der TZ-Overload setzt zusaetzlich die POSIX-Zeitzone und startet SNTP.
  configTime(
    TIMEZONE_POSIX,
    "pool.ntp.org",
    "time.cloudflare.com"
  );

  unsigned long start = millis();
  while (!timeIsValid()) {
    delay(250);
    yield();

    if (millis() - start > 15000UL) {
      Serial.println(F("[NTP] Timeout - TLS kann ohne gueltige Zeit nicht validieren."));
      return false;
    }
  }

  // Vorsichtshalber nach der SNTP-Initialisierung erneut anwenden.
  applyBerlinTimezone();

  Serial.print(F("[NTP] OK (Europe/Berlin): "));
  Serial.println(formatUnixTime(time(nullptr)));
  return true;
}

// ============================================================
// TLS / X.509
// ============================================================

bool initTrustAnchors() {
  if (openaiTrustAnchors) return true;

  Serial.println();
  Serial.println(F("========================================"));
  Serial.println(F(" TLS / X.509"));
  Serial.println(F("========================================"));

  openaiTrustAnchors = new BearSSL::X509List(ISRG_ROOT_X1);
  if (!openaiTrustAnchors) {
    Serial.println(F("[TLS] FEHLER: Trust-Anchor-Speicher konnte nicht reserviert werden."));
    return false;
  }

  bool okX2 = openaiTrustAnchors->append(ISRG_ROOT_X2);
  bool okGts = openaiTrustAnchors->append(GTS_ROOT_R4);

  if (!okX2 || !okGts) {
    Serial.println(F("[TLS] FEHLER: Root-CAs konnten nicht vollstaendig geladen werden."));
    delete openaiTrustAnchors;
    openaiTrustAnchors = nullptr;
    return false;
  }

  Serial.println(F("[TLS] Zertifikatspruefung AKTIV"));
  Serial.println(F("[TLS] Trust Anchors: ISRG Root X1, ISRG Root X2, GTS Root R4"));
  Serial.println(F("[TLS] TLS-Version: 1.2"));
  printHeapStats(F("nach TLS Trust Anchors"));
  return true;
}

bool configureSecureClient(BearSSL::WiFiClientSecure& client) {
  if (!timeIsValid()) {
    Serial.println(F("[TLS] Keine gueltige Systemzeit; Zertifikatspruefung nicht moeglich."));
    return false;
  }
  if (!openaiTrustAnchors) {
    Serial.println(F("[TLS] Keine Trust Anchors initialisiert."));
    return false;
  }

  client.setTrustAnchors(openaiTrustAnchors);
  client.setX509Time(time(nullptr));
  client.setSSLVersion(BR_TLS12, BR_TLS12);
  return true;
}

void printTlsError(BearSSL::WiFiClientSecure& client, const __FlashStringHelper* context) {
  char sslError[160] = {0};
  int code = client.getLastSSLError(sslError, sizeof(sslError));
  if (code != 0) {
    Serial.print(F("[TLS] "));
    Serial.print(context);
    Serial.print(F(": BearSSL Fehler "));
    Serial.print(code);
    Serial.print(F(" - "));
    Serial.println(sslError);
  }
}

// ============================================================
// HTTP-Helfer
// ============================================================

int httpsPostJson(const String& url, const String& json, String& response) {
  BearSSL::WiFiClientSecure client;
  if (!configureSecureClient(client)) return -120;

  HTTPClient https;
  https.setTimeout(20000);
  https.setReuse(false);

  if (!https.begin(client, url)) return -100;

  https.addHeader("Content-Type", "application/json");
  https.addHeader("Accept", "application/json");

  int httpCode = https.POST(json);

  if (httpCode > 0) response = https.getString();
  else {
    response = "";
    Serial.print(F("[HTTP] POST Fehler: "));
    Serial.println(HTTPClient::errorToString(httpCode));
    printTlsError(client, F("POST"));
  }

  https.end();
  return httpCode;
}

int copyHttpBodyToFile(HTTPClient& https, File& out, uint32_t idleTimeoutMs = 8000) {
  WiFiClient* stream = https.getStreamPtr();
  if (!stream) return -6;

  const int contentLength = https.getSize();
  int remaining = contentLength;
  int total = 0;
  uint32_t lastDataAt = millis();
  uint8_t buffer[256];

  if (DEBUG_HTTP) {
    Serial.print(F("[HTTP] Content-Length: "));
    if (contentLength >= 0) Serial.println(contentLength);
    else Serial.println(F("unbekannt"));
  }

  while (true) {
    int available = stream->available();

    if (available > 0) {
      size_t toRead = (size_t)available;
      if (toRead > sizeof(buffer)) toRead = sizeof(buffer);
      if (remaining >= 0 && toRead > (size_t)remaining) toRead = (size_t)remaining;

      int n = stream->read(buffer, toRead);
      if (n > 0) {
        size_t written = out.write(buffer, (size_t)n);
        if (written != (size_t)n) return -10;

        total += n;
        if (remaining >= 0) remaining -= n;
        lastDataAt = millis();
        if (remaining == 0) break;
      }
    } else {
      if (remaining == 0) break;

      if (!stream->connected()) {
        if (contentLength < 0) break;
        return -5;
      }

      if (millis() - lastDataAt > idleTimeoutMs) return -11;
      delay(1);
      yield();
    }
  }

  out.flush();
  return total;
}

int httpsPostFormToFile(const String& url, const String& form, const char* path) {
  LittleFS.remove(path);
  File out = LittleFS.open(path, "w");
  if (!out) return -110;

  int httpCode = -100;
  int bytesWritten = -1;

  {
    BearSSL::WiFiClientSecure client;
    if (!configureSecureClient(client)) {
      out.close();
      LittleFS.remove(path);
      return -120;
    }

    HTTPClient https;
    https.setTimeout(20000);
    https.setReuse(false);
    https.useHTTP10(true);

    if (!https.begin(client, url)) {
      out.close();
      LittleFS.remove(path);
      return -100;
    }

    https.addHeader("Content-Type", "application/x-www-form-urlencoded");
    https.addHeader("Accept", "application/json");

    httpCode = https.POST(form);

    if (httpCode > 0) {
      bytesWritten = copyHttpBodyToFile(https, out);
    } else {
      Serial.print(F("[HTTP] OAuth POST Fehler: "));
      Serial.println(HTTPClient::errorToString(httpCode));
      printTlsError(client, F("OAuth POST"));
    }

    https.end();
  }

  out.flush();
  out.close();

  if (DEBUG_HTTP) {
    Serial.print(F("[HTTP] OAuth Response Bytes gespeichert: "));
    Serial.println(bytesWritten);
  }

  if (httpCode == 200 && bytesWritten < 0) return bytesWritten;
  return httpCode;
}

int httpsPostJsonToFile(const String& url, const String& json, const char* path) {
  LittleFS.remove(path);
  File out = LittleFS.open(path, "w");
  if (!out) return -110;

  int httpCode = -100;
  int bytesWritten = -1;

  {
    BearSSL::WiFiClientSecure client;
    if (!configureSecureClient(client)) {
      out.close();
      LittleFS.remove(path);
      return -120;
    }

    HTTPClient https;
    https.setTimeout(20000);
    https.setReuse(false);
    https.useHTTP10(true);

    if (!https.begin(client, url)) {
      out.close();
      LittleFS.remove(path);
      return -100;
    }

    https.addHeader("Content-Type", "application/json");
    https.addHeader("Accept", "application/json");

    httpCode = https.POST(json);

    if (httpCode > 0) {
      bytesWritten = copyHttpBodyToFile(https, out);
    } else {
      Serial.print(F("[HTTP] OAuth JSON POST Fehler: "));
      Serial.println(HTTPClient::errorToString(httpCode));
      printTlsError(client, F("OAuth JSON POST"));
    }

    https.end();
  }

  out.flush();
  out.close();

  if (DEBUG_HTTP) {
    Serial.print(F("[HTTP] OAuth Response Bytes gespeichert: "));
    Serial.println(bytesWritten);
  }

  if (httpCode == 200 && bytesWritten < 0) return bytesWritten;
  return httpCode;
}

String readSmallFile(const char* path, size_t maxBytes = 1000) {
  File f = LittleFS.open(path, "r");
  if (!f) return "";

  String result;
  size_t size = f.size();
  if (size > maxBytes) size = maxBytes;
  result.reserve(size + 1);

  while (f.available() && result.length() < maxBytes) {
    result += (char)f.read();
    yield();
  }
  f.close();
  return result;
}

// ============================================================
// Device OAuth: Erstmalige Anmeldung
// ============================================================

bool requestDeviceCode(String& deviceAuthId, String& userCode, unsigned long& interval) {
  String body = "{\"client_id\":\"" + String(OPENAI_CLIENT_ID) + "\"}";
  String response;

  Serial.println();
  Serial.println(F("========================================"));
  Serial.println(F(" OPENAI DEVICE LOGIN"));
  Serial.println(F("========================================"));
  Serial.println(F("[1] Device Code anfordern..."));

  int httpCode = httpsPostJson(DEVICE_USERCODE_URL, body, response);
  Serial.print(F("HTTP: "));
  Serial.println(httpCode);

  if (httpCode != 200) {
    Serial.println(F("Device-Code-Anforderung fehlgeschlagen."));
    if (response.length() && response.length() < 1200) Serial.println(response);
    return false;
  }

  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, response);
  response = "";

  if (error) {
    Serial.print(F("JSON Fehler: "));
    Serial.println(error.c_str());
    return false;
  }

  deviceAuthId = doc["device_auth_id"].as<String>();
  if (doc["user_code"]) userCode = doc["user_code"].as<String>();
  else userCode = doc["usercode"].as<String>();

  interval = doc["interval"] | 5;
  if (interval < 2) interval = 5;

  if (deviceAuthId.length() == 0 || userCode.length() == 0) return false;

  Serial.println();
  Serial.println(F("========================================"));
  Serial.println(F(" JETZT IM BROWSER OEFFNEN"));
  Serial.println(F("========================================"));
  Serial.println();
  Serial.println(DEVICE_VERIFY_URL);
  Serial.println();
  Serial.print(F("CODE: "));
  Serial.println(userCode);
  Serial.println();
  Serial.println(F("Bei OpenAI anmelden und Code bestaetigen."));
  Serial.println();

  oledShowLoginCode(userCode);
  return true;
}

bool waitForAuthorization(
  const String& deviceAuthId,
  const String& userCode,
  unsigned long interval,
  String& authorizationCode,
  String& codeVerifier
) {
  Serial.println(F("[2] Warte auf Autorisierung"));

  unsigned long started = millis();
  const unsigned long timeout = 15UL * 60UL * 1000UL;

  while (millis() - started < timeout) {
    String body;
    body.reserve(deviceAuthId.length() + userCode.length() + 64);
    body = "{\"device_auth_id\":\"" + deviceAuthId +
           "\",\"user_code\":\"" + userCode + "\"}";

    String response;
    int httpCode = httpsPostJson(DEVICE_TOKEN_URL, body, response);

    Serial.print(F("Poll -> HTTP "));
    Serial.println(httpCode);

    if (httpCode == 403 || httpCode == 404) {
      unsigned long waitStart = millis();
      while (millis() - waitStart < interval * 1000UL) {
        delay(100);
        yield();
      }
      continue;
    }

    if (httpCode == 200) {
      DynamicJsonDocument doc(4096);
      DeserializationError error = deserializeJson(doc, response);
      response = "";

      if (error) {
        Serial.print(F("JSON Fehler: "));
        Serial.println(error.c_str());
        return false;
      }

      authorizationCode = doc["authorization_code"].as<String>();
      codeVerifier = doc["code_verifier"].as<String>();

      if (authorizationCode.length() == 0 || codeVerifier.length() == 0) return false;

      Serial.println(F("Autorisierung erfolgreich!"));
      oledStatus(F("OPENAI"), F("Login OK"));
      return true;
    }

    Serial.println(F("Unerwartete Device-Auth-Antwort."));
    if (response.length() && response.length() < 1200) Serial.println(response);
    return false;
  }

  Serial.println(F("Device Auth Timeout"));
  return false;
}

bool exchangeAuthorizationCode(const String& authorizationCode, const String& codeVerifier) {
  Serial.println();
  Serial.println(F("[3] OAuth Token Exchange"));
  printHeapStats(F("vor OAuth Token Exchange"));

  String form;
  form.reserve(authorizationCode.length() + codeVerifier.length() + 300);
  form = "grant_type=authorization_code";
  form += "&code=" + urlEncode(authorizationCode);
  form += "&redirect_uri=" + urlEncode(DEVICE_REDIRECT_URI);
  form += "&client_id=" + urlEncode(OPENAI_CLIENT_ID);
  form += "&code_verifier=" + urlEncode(codeVerifier);

  int httpCode = httpsPostFormToFile(OAUTH_TOKEN_URL, form, OAUTH_TMP_FILE);
  form = "";

  Serial.print(F("HTTP: "));
  Serial.println(httpCode);

  if (httpCode != 200) {
    Serial.println(F("Token Exchange fehlgeschlagen."));
    String errorBody = readSmallFile(OAUTH_TMP_FILE, 1000);
    if (errorBody.length()) Serial.println(errorBody);
    LittleFS.remove(OAUTH_TMP_FILE);
    return false;
  }

  File tokenFile = LittleFS.open(OAUTH_TMP_FILE, "r");
  if (!tokenFile) {
    LittleFS.remove(OAUTH_TMP_FILE);
    return false;
  }

  StaticJsonDocument<128> filter;
  filter["access_token"] = true;
  filter["refresh_token"] = true;
  filter["id_token"] = true;

  DynamicJsonDocument doc(10240);
  DeserializationError error = deserializeJson(
    doc, tokenFile, DeserializationOption::Filter(filter));

  tokenFile.close();
  LittleFS.remove(OAUTH_TMP_FILE);

  if (error) {
    Serial.print(F("Token JSON Fehler: "));
    Serial.println(error.c_str());
    return false;
  }

  accessToken = doc["access_token"].as<String>();
  refreshToken = doc["refresh_token"].as<String>();
  String idToken = doc["id_token"].as<String>();

  if (accessToken.length() == 0 || refreshToken.length() == 0 || idToken.length() == 0) {
    Serial.println(F("FEHLER: Ein erforderlicher OAuth-Token fehlt."));
    return false;
  }

  accountId = extractAccountId(idToken);
  idToken = "";
  if (accountId.length() == 0) return false;

  accessTokenExpiresAt = extractJwtExpiry(accessToken);

  Serial.println(F("OAuth Tokens erhalten."));
  Serial.print(F("Access Token Laenge:  "));
  Serial.println(accessToken.length());
  Serial.print(F("Refresh Token Laenge: "));
  Serial.println(refreshToken.length());
  Serial.print(F("Account ID:            "));
  Serial.print(accountId.substring(0, accountId.length() > 8 ? 8 : accountId.length()));
  Serial.println(F("..."));

  if (accessTokenExpiresAt > 0) {
    Serial.print(F("Access Token gueltig bis: "));
    Serial.println(formatUnixTime(accessTokenExpiresAt));
  }

  printHeapStats(F("nach Token-Parsing"));
  return saveStoredAuth();
}

bool performDeviceLogin() {
  String deviceAuthId;
  String userCode;
  unsigned long pollInterval = 5;

  if (!requestDeviceCode(deviceAuthId, userCode, pollInterval)) return false;

  String authorizationCode;
  String codeVerifier;

  if (!waitForAuthorization(
        deviceAuthId, userCode, pollInterval,
        authorizationCode, codeVerifier)) {
    return false;
  }

  deviceAuthId = "";
  userCode = "";

  bool ok = exchangeAuthorizationCode(authorizationCode, codeVerifier);
  authorizationCode = "";
  codeVerifier = "";
  return ok;
}

// ============================================================
// OAuth Refresh
// ============================================================

bool responseIndicatesPermanentRefreshFailure(const String& response) {
  return response.indexOf("refresh_token_invalidated") >= 0 ||
         response.indexOf("refresh_token_reused") >= 0 ||
         response.indexOf("refresh_token_expired") >= 0 ||
         response.indexOf("invalid_grant") >= 0;
}

RefreshResult refreshAccessToken() {
  if (refreshToken.length() == 0 || accountId.length() == 0) {
    return RefreshResult::PERMANENT_FAILURE;
  }

  Serial.println();
  Serial.println(F("[AUTH] Access Token erneuern..."));
  printHeapStats(F("vor OAuth Refresh"));

  String body;
  body.reserve(refreshToken.length() + 160);
  body = "{\"client_id\":\"";
  body += OPENAI_CLIENT_ID;
  body += "\",\"grant_type\":\"refresh_token\",\"refresh_token\":\"";
  body += refreshToken;
  body += "\"}";

  int httpCode = httpsPostJsonToFile(OAUTH_TOKEN_URL, body, OAUTH_TMP_FILE);
  body = "";

  Serial.print(F("[AUTH] Refresh HTTP: "));
  Serial.println(httpCode);

  if (httpCode != 200) {
    String response = readSmallFile(OAUTH_TMP_FILE, 1000);
    LittleFS.remove(OAUTH_TMP_FILE);

    bool permanent = (httpCode == 401) ||
                     (httpCode == 400 && responseIndicatesPermanentRefreshFailure(response));

    if (permanent) {
      Serial.println(F("[AUTH] Refresh-Token ist ungueltig/revoziert."));
      return RefreshResult::PERMANENT_FAILURE;
    }

    Serial.println(F("[AUTH] Temporaerer Refresh-Fehler."));
    if (DEBUG_HTTP && response.length()) Serial.println(response);
    return RefreshResult::TEMPORARY_FAILURE;
  }

  File tokenFile = LittleFS.open(OAUTH_TMP_FILE, "r");
  if (!tokenFile) {
    LittleFS.remove(OAUTH_TMP_FILE);
    return RefreshResult::TEMPORARY_FAILURE;
  }

  StaticJsonDocument<96> filter;
  filter["access_token"] = true;
  filter["refresh_token"] = true;

  DynamicJsonDocument doc(8192);
  DeserializationError error = deserializeJson(
    doc, tokenFile, DeserializationOption::Filter(filter));

  tokenFile.close();
  LittleFS.remove(OAUTH_TMP_FILE);

  if (error) {
    Serial.print(F("[AUTH] Refresh JSON Fehler: "));
    Serial.println(error.c_str());
    return RefreshResult::TEMPORARY_FAILURE;
  }

  String newAccessToken = doc["access_token"].as<String>();
  String newRefreshToken = doc["refresh_token"].as<String>();

  if (newAccessToken.length() == 0) {
    Serial.println(F("[AUTH] Refresh-Antwort enthaelt keinen Access Token."));
    return RefreshResult::TEMPORARY_FAILURE;
  }

  accessToken = newAccessToken;
  newAccessToken = "";

  bool refreshTokenRotated = false;
  if (newRefreshToken.length() > 0 && newRefreshToken != refreshToken) {
    refreshToken = newRefreshToken;
    refreshTokenRotated = true;
  }
  newRefreshToken = "";

  accessTokenExpiresAt = extractJwtExpiry(accessToken);

  if (refreshTokenRotated) {
    Serial.println(F("[AUTH] Refresh-Token rotiert -> speichere neuen Token."));
    if (!saveStoredAuth()) {
      Serial.println(F("[AUTH] WARNUNG: Neuer Refresh-Token konnte nicht gespeichert werden."));
    }
  }

  Serial.println(F("[AUTH] Refresh erfolgreich."));
  if (accessTokenExpiresAt > 0) {
    Serial.print(F("[AUTH] Access Token gueltig bis: "));
    Serial.println(formatUnixTime(accessTokenExpiresAt));
  }

  printHeapStats(F("nach OAuth Refresh"));
  return RefreshResult::OK;
}

bool accessTokenNeedsRefresh() {
  if (accessToken.length() == 0) return true;
  if (accessTokenExpiresAt == 0 || !timeIsValid()) return false;
  return (uint32_t)time(nullptr) + TOKEN_REFRESH_MARGIN_SEC >= accessTokenExpiresAt;
}

bool ensureAccessToken() {
  if (!accessTokenNeedsRefresh()) return true;

  RefreshResult result = refreshAccessToken();
  if (result == RefreshResult::OK) return true;

  if (result == RefreshResult::PERMANENT_FAILURE) {
    Serial.println(F("[AUTH] Gespeicherte Anmeldung nicht mehr nutzbar."));
    clearStoredAuth();
    return performDeviceLogin();
  }

  return false;
}

// ============================================================
// Usage: Parsing / Modell
// ============================================================

UsageWindow parseUsageWindow(JsonVariant window) {
  UsageWindow result;
  if (window.isNull()) return result;

  result.valid = true;
  result.usedPercent = window["used_percent"] | 0.0f;
  if (result.usedPercent < 0.0f) result.usedPercent = 0.0f;
  if (result.usedPercent > 100.0f) result.usedPercent = 100.0f;
  result.remainingPercent = 100.0f - result.usedPercent;
  result.windowSeconds = window["limit_window_seconds"] | 0UL;
  result.resetAt = window["reset_at"] | 0UL;

  // Fallback, falls OpenAI irgendwann nur reset_after_seconds liefert.
  if (result.resetAt == 0) {
    uint32_t after = window["reset_after_seconds"] | 0UL;
    if (after > 0 && timeIsValid()) result.resetAt = time(nullptr) + after;
  }

  return result;
}

bool isFiveHourWindow(const UsageWindow& w) {
  return w.valid && w.windowSeconds >= 4UL * 3600UL && w.windowSeconds <= 6UL * 3600UL;
}

bool isWeeklyWindow(const UsageWindow& w) {
  return w.valid && w.windowSeconds >= 6UL * 86400UL && w.windowSeconds <= 8UL * 86400UL;
}

void classifyWindows(const UsageWindow& primary, const UsageWindow& secondary) {
  usage.fiveHour = UsageWindow();
  usage.weekly = UsageWindow();
  usage.otherPrimary = UsageWindow();
  usage.otherSecondary = UsageWindow();

  if (isFiveHourWindow(primary)) usage.fiveHour = primary;
  else if (isWeeklyWindow(primary)) usage.weekly = primary;
  else if (primary.valid) usage.otherPrimary = primary;

  if (isFiveHourWindow(secondary)) usage.fiveHour = secondary;
  else if (isWeeklyWindow(secondary)) usage.weekly = secondary;
  else if (secondary.valid) usage.otherSecondary = secondary;
}

int fetchUsageOnce() {
  usage.lastAttemptAt = timeIsValid() ? time(nullptr) : 0;

  if (accessToken.length() == 0 || accountId.length() == 0) return -101;
  if (WiFi.status() != WL_CONNECTED) return -103;
  if (!timeIsValid()) return -104;

  String response;
  int httpCode = -100;

  {
    BearSSL::WiFiClientSecure client;
    if (!configureSecureClient(client)) return -120;

    HTTPClient https;
    https.setTimeout(20000);
    https.setReuse(false);

    if (!https.begin(client, USAGE_URL)) return -100;

    https.addHeader("Authorization", "Bearer " + accessToken);
    https.addHeader("ChatGPT-Account-Id", accountId);
    https.addHeader("Accept", "application/json");
    https.addHeader("OpenAI-Beta", "codex-1");
    https.addHeader("originator", "codex_cli_rs");
    https.addHeader("Origin", "https://chatgpt.com");
    https.addHeader("Referer", "https://chatgpt.com/");
    https.setUserAgent("codex-cli");

    httpCode = https.GET();
    Serial.print(F("[USAGE] HTTP: "));
    Serial.println(httpCode);

    if (httpCode > 0) response = https.getString();
    else {
      Serial.print(F("[USAGE] HTTPClient Fehler: "));
      Serial.println(HTTPClient::errorToString(httpCode));
      printTlsError(client, F("Usage GET"));
    }

    https.end();
  }

  usage.lastHttpCode = httpCode;

  if (httpCode != 200) {
    usage.consecutiveFailures++;
    if (DEBUG_HTTP && response.length() > 0 && response.length() < 1200) {
      Serial.println(response);
    }
    return httpCode;
  }

  StaticJsonDocument<512> filter;
  filter["plan_type"] = true;
  filter["rate_limit"]["allowed"] = true;
  filter["rate_limit"]["limit_reached"] = true;
  filter["rate_limit"]["primary_window"] = true;
  filter["rate_limit"]["secondary_window"] = true;
  filter["credits"]["has_credits"] = true;
  filter["credits"]["unlimited"] = true;
  filter["credits"]["balance"] = true;
  filter["rate_limit_reset_credits"]["available_count"] = true;
  filter["rate_limit_reset_credits"]["applicable_available_count"] = true;

  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(
    doc, response, DeserializationOption::Filter(filter));
  response = "";

  if (error) {
    usage.consecutiveFailures++;
    Serial.print(F("[USAGE] JSON Fehler: "));
    Serial.println(error.c_str());
    return -102;
  }

  UsageWindow primary = parseUsageWindow(doc["rate_limit"]["primary_window"]);
  UsageWindow secondary = parseUsageWindow(doc["rate_limit"]["secondary_window"]);

  usage.plan = doc["plan_type"] | "unbekannt";
  usage.allowed = doc["rate_limit"]["allowed"] | true;
  usage.limitReached = doc["rate_limit"]["limit_reached"] | false;
  classifyWindows(primary, secondary);

  usage.hasCredits = doc["credits"]["has_credits"] | false;
  usage.unlimitedCredits = doc["credits"]["unlimited"] | false;
  usage.creditBalance = doc["credits"]["balance"].as<String>();

  usage.resetCreditsAvailable =
    doc["rate_limit_reset_credits"]["available_count"] | 0;
  usage.resetCreditsApplicable =
    doc["rate_limit_reset_credits"]["applicable_available_count"] | 0;

  usage.fetchedAt = time(nullptr);
  usage.lastHttpCode = 200;
  usage.consecutiveFailures = 0;
  usage.valid = true;
  return 200;
}

// ============================================================
// Usage: serial output / model access
// ============================================================

void printWindow(const __FlashStringHelper* name, const UsageWindow& window) {
  if (!window.valid) return;

  Serial.println();
  Serial.print(F("--- "));
  Serial.print(name);
  Serial.println(F(" ---"));

  Serial.print(F("Verbraucht:   "));
  Serial.print(window.usedPercent, 1);
  Serial.println(F(" %"));

  Serial.print(F("Verbleibend:  "));
  Serial.print(window.remainingPercent, 1);
  Serial.println(F(" %"));

  if (window.resetAt > 0) {
    Serial.print(F("Reset in:     "));
    Serial.println(formatDuration(secondsUntil(window.resetAt)));
    Serial.print(F("Reset:        "));
    Serial.println(formatUnixTime(window.resetAt));
  }
}

void printUsage() {
  Serial.println();
  Serial.println(F("========================================"));
  Serial.println(F(" OPENAI WORK / CODEX USAGE"));
  Serial.println(F("========================================"));

  Serial.print(F("Plan:           "));
  Serial.println(usage.plan);
  Serial.print(F("Allowed:        "));
  Serial.println(usage.allowed ? F("JA") : F("NEIN"));
  Serial.print(F("Limit erreicht: "));
  Serial.println(usage.limitReached ? F("JA") : F("NEIN"));

  printWindow(F("5-Stunden-Limit"), usage.fiveHour);
  printWindow(F("Wochenlimit"), usage.weekly);
  printWindow(F("Weitere Primary Window"), usage.otherPrimary);
  printWindow(F("Weitere Secondary Window"), usage.otherSecondary);

  Serial.println();
  Serial.println(F("--- Credits ---"));
  Serial.print(F("Balance:        "));
  Serial.println(usage.creditBalance.length() ? usage.creditBalance : String("0"));
  Serial.print(F("Reset Credits:  "));
  Serial.print(usage.resetCreditsAvailable);
  Serial.print(F(" verfuegbar / "));
  Serial.print(usage.resetCreditsApplicable);
  Serial.println(F(" anwendbar"));

  Serial.println();
  Serial.print(F("Letztes Update: "));
  Serial.println(formatUnixTime(usage.fetchedAt));
  Serial.print(F("Fehlerfolge:    "));
  Serial.println(usage.consecutiveFailures);
}

// Read-only access to the current usage model.
const UsageData& getUsageData() {
  return usage;
}

bool usageIsFresh(uint32_t maxAgeSeconds = 900UL) {
  if (!usage.valid || usage.fetchedAt <= 0 || !timeIsValid()) return false;
  time_t now = time(nullptr);
  return now >= usage.fetchedAt && (uint32_t)(now - usage.fetchedAt) <= maxAgeSeconds;
}


// ============================================================
// OLED 128x32: Initialisierung und Tankanzeige
// ============================================================

bool oledAddressResponds(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool initOLED() {
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  Wire.setClock(400000UL);

  if (oledAddressResponds(OLED_ADDRESS_PRIMARY)) {
    oledAddress = OLED_ADDRESS_PRIMARY;
  } else if (oledAddressResponds(OLED_ADDRESS_FALLBACK)) {
    oledAddress = OLED_ADDRESS_FALLBACK;
  } else {
    Serial.println(F("[OLED] Kein Display auf 0x3C/0x3D gefunden; Betrieb ohne OLED."));
    oledReady = false;
    return false;
  }

  if (!display.begin(SSD1306_SWITCHCAPVCC, oledAddress)) {
    Serial.println(F("[OLED] SSD1306 Initialisierung fehlgeschlagen; Betrieb ohne OLED."));
    oledReady = false;
    return false;
  }

  display.clearDisplay();
  display.setTextWrap(false);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.display();

  oledReady = true;
  Serial.print(F("[OLED] SSD1306 128x32 aktiv, I2C-Adresse 0x"));
  Serial.println(oledAddress, HEX);

  oledShowStartupBanner();
  oledStatus(F("OPENAI MONITOR"), F("Starte..."));
  return true;
}

void oledPrintCentered(const String& text, int16_t y) {
  if (!oledReady) return;
  int16_t x = (OLED_WIDTH - (int16_t)text.length() * 6) / 2;
  if (x < 0) x = 0;
  display.setCursor(x, y);
  display.print(text);
}

void oledStatus(const String& line1, const String& line2) {
  if (!oledReady) return;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  oledPrintCentered(line1, 4);
  if (line2.length()) oledPrintCentered(line2, 19);
  display.display();
}

void oledShowStartupBanner() {
  if (!oledReady) return;

  display.clearDisplay();
  display.setTextWrap(false);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Startup splash: compact frame, project name and firmware version.
  display.drawRoundRect(0, 0, OLED_WIDTH, OLED_HEIGHT, 3, SSD1306_WHITE);
  display.drawFastHLine(18, 16, OLED_WIDTH - 36, SSD1306_WHITE);

  oledPrintCentered(PROJECT_NAME, 5);

  String versionText = String(F("v")) + PROJECT_VERSION;
  oledPrintCentered(versionText, 21);

  display.display();
  delay(1600);
}

void oledShowLoginCode(const String& userCode) {
  if (!oledReady) return;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  oledPrintCentered(F("OPENAI LOGIN"), 2);
  oledPrintCentered(String(F("CODE ")) + userCode, 19);
  display.display();
}

void oledDrawRightAlignedPercent(uint8_t percent, int16_t textY, bool warning, bool critical) {
  String text = String(percent) + "%";
  int16_t textWidth = (int16_t)text.length() * 6;
  int16_t x = OLED_WIDTH - textWidth;

  // Unter 25% invertierte Prozentzahl. Unter 10% wechselt die Darstellung
  // jede Sekunde zwischen normal und invertiert (monochrome Warnwirkung).
  bool inverted = warning;
  if (critical && !oledBlinkPhase) inverted = false;

  if (inverted) {
    display.fillRect(x - 1, textY - 1, textWidth + 1, 10, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.setTextColor(SSD1306_WHITE);
  }

  display.setCursor(x, textY);
  display.print(text);
  display.setTextColor(SSD1306_WHITE);
}


String oledEmptyTankText(time_t resetAt) {
  // LEER bleibt immer sichtbar. Wenn eine Reset-Zeit bekannt ist, wird
  // direkt angezeigt, ab wann das Kontingent wieder verfuegbar ist.
  //
  // Gleicher Kalendertag:  "LEER 14:31"
  // Anderer Kalendertag:   "LEER DI 16:29"
  //
  // Beide Varianten passen in den 83-px-Tankrahmen bei TextSize(1).

  if (resetAt <= 0 || !timeIsValid()) {
    return F("LEER");
  }

  time_t now = time(nullptr);
  struct tm nowTm;
  struct tm resetTm;
  localtime_r(&now, &nowTm);
  localtime_r(&resetAt, &resetTm);

  const bool sameDay =
    nowTm.tm_year == resetTm.tm_year &&
    nowTm.tm_yday == resetTm.tm_yday;

  char buffer[16];

  if (sameDay) {
    snprintf(
      buffer,
      sizeof(buffer),
      "LEER %02d:%02d",
      resetTm.tm_hour,
      resetTm.tm_min
    );
  } else {
    static const char* const WEEKDAYS_DE[] = {
      "SO", "MO", "DI", "MI", "DO", "FR", "SA"
    };

    snprintf(
      buffer,
      sizeof(buffer),
      "LEER %s %02d:%02d",
      WEEKDAYS_DE[resetTm.tm_wday],
      resetTm.tm_hour,
      resetTm.tm_min
    );
  }

  return String(buffer);
}

void oledDrawTankRow(const char* label, const UsageWindow& window, int16_t rowY, bool stale) {
  const int16_t textY = rowY + 1;

  // Label: bei alten Daten invertiert, damit der letzte bekannte Tankstand
  // sichtbar bleibt, aber als nicht mehr frisch erkennbar ist.
  if (stale) {
    display.fillRect(0, rowY, 13, OLED_BAR_H, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
  } else {
    display.setTextColor(SSD1306_WHITE);
  }
  display.setCursor(OLED_LABEL_X, textY);
  display.print(label);
  display.setTextColor(SSD1306_WHITE);

  // Tankrahmen
  display.drawRect(OLED_BAR_X, rowY, OLED_BAR_W, OLED_BAR_H, SSD1306_WHITE);

  if (!window.valid) {
    // Noch nie Daten erhalten.
    display.setCursor(OLED_BAR_X + 29, textY);
    display.print(F("--"));
    String noData = F("--%");
    display.setCursor(OLED_WIDTH - (int16_t)noData.length() * 6, textY);
    display.print(noData);
    return;
  }

  float remaining = window.remainingPercent;
  if (remaining < 0.0f) remaining = 0.0f;
  if (remaining > 100.0f) remaining = 100.0f;
  uint8_t percent = (uint8_t)(remaining + 0.5f);

  // Innenraum des Tanks: 2 px Rand links/rechts, 2 px oben/unten.
  const int16_t innerX = OLED_BAR_X + 2;
  const int16_t innerY = rowY + 2;
  const int16_t innerW = OLED_BAR_W - 4;
  const int16_t innerH = OLED_BAR_H - 4;
  int16_t fillW = (int16_t)((innerW * (uint16_t)percent + 50U) / 100U);

  if (fillW > 0) {
    display.fillRect(innerX, innerY, fillW, innerH, SSD1306_WHITE);
  }

  // Bei leerem Tank zeigen wir LEER plus Reset-Zeit direkt im Rahmen.
  // Beispiele:
  //   LEER 14:31
  //   LEER DI 16:29
  //
  // Der Adafruit-Standardfont ist effektiv 7 px hoch. Im 11-px-Tankrahmen
  // ergibt rowY + 2 oben und unten jeweils 1 px sichtbaren Innenabstand.
  if (percent == 0) {
    const String emptyText = oledEmptyTankText(window.resetAt);
    const int16_t emptyTextWidth = (int16_t)emptyText.length() * 6;
    const int16_t emptyX =
      OLED_BAR_X + (OLED_BAR_W - emptyTextWidth) / 2;
    const int16_t emptyTextY = rowY + 2;

    display.setCursor(emptyX, emptyTextY);
    display.print(emptyText);
  }

  bool warning = percent <= OLED_WARNING_PERCENT;
  bool critical = percent <= OLED_CRITICAL_PERCENT;
  oledDrawRightAlignedPercent(percent, textY, warning, critical);
}

void updateOLED(bool force) {
  if (!oledReady) return;

  unsigned long nowMs = millis();
  if (!force && nowMs - lastOledRefreshMs < OLED_REFRESH_MS) return;
  lastOledRefreshMs = nowMs;
  oledBlinkPhase = !oledBlinkPhase;

  // Solange noch nie Usage-Daten vorhanden waren, Statusseite anzeigen.
  if (!usage.valid) {
    oledStatus(F("OPENAI"), F("warte auf Daten"));
    return;
  }

  bool stale = !usageIsFresh(900UL);  // >15 Minuten ohne guten Abruf

  display.clearDisplay();
  display.setTextWrap(false);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Pixelgenaues Hauptlayout:
  // 0..12   Label (5H / WK)
  // 15..97  Tankrahmen 83 px
  // 104..127 rechtsbuendig Prozentwert (bis 100%)
  // Zeile 1: y=2..12, Zeile 2: y=18..28
  oledDrawTankRow("5H", usage.fiveHour, OLED_ROW1_Y, stale);
  oledDrawTankRow("WK", usage.weekly, OLED_ROW2_Y, stale);

  display.display();
}

// ============================================================
// Usage + Auth-Recovery
// ============================================================

bool fetchUsageWithAuthRecovery() {
  if (!ensureAccessToken()) return false;

  int httpCode = fetchUsageOnce();
  if (httpCode == 200) {
    printUsage();
    return true;
  }

  if (httpCode == 401 || httpCode == 403) {
    Serial.println(F("[USAGE] Auth-Fehler -> Token erneuern und einmal wiederholen."));

    RefreshResult refreshResult = refreshAccessToken();

    if (refreshResult == RefreshResult::PERMANENT_FAILURE) {
      clearStoredAuth();
      if (!performDeviceLogin()) return false;
    } else if (refreshResult != RefreshResult::OK) {
      return false;
    }

    httpCode = fetchUsageOnce();
    if (httpCode == 200) {
      printUsage();
      return true;
    }
  }

  return false;
}

bool runResetAwareUsageRefreshIfDue() {
  if (!usage.valid || !timeIsValid() || accessToken.length() == 0) {
    return false;
  }

  const time_t now = time(nullptr);

  bool fiveHourDue = false;
  bool weeklyDue = false;

  if (usage.fiveHour.valid && usage.fiveHour.resetAt > 0) {
    const time_t triggerAt =
      usage.fiveHour.resetAt + (time_t)RESET_REFRESH_DELAY_SEC;

    fiveHourDue =
      now >= triggerAt &&
      usage.fiveHour.resetAt != lastResetRefreshFiveHourAt;
  }

  if (usage.weekly.valid && usage.weekly.resetAt > 0) {
    const time_t triggerAt =
      usage.weekly.resetAt + (time_t)RESET_REFRESH_DELAY_SEC;

    weeklyDue =
      now >= triggerAt &&
      usage.weekly.resetAt != lastResetRefreshWeeklyAt;
  }

  if (!fiveHourDue && !weeklyDue) {
    return false;
  }

  // Vor dem Request markieren, damit ein langsamer/fehlgeschlagener Request
  // nicht in jeder loop()-Iteration erneut angestossen wird. Bei einem
  // temporaeren Fehler greift spaetestens der normale 5-Minuten-Poll.
  if (fiveHourDue) {
    lastResetRefreshFiveHourAt = usage.fiveHour.resetAt;
  }
  if (weeklyDue) {
    lastResetRefreshWeeklyAt = usage.weekly.resetAt;
  }

  Serial.println();
  Serial.print(F("[USAGE] Reset-Sonderabruf +"));
  Serial.print(RESET_REFRESH_DELAY_SEC);
  Serial.print(F("s fuer "));

  if (fiveHourDue && weeklyDue) {
    Serial.println(F("5H + WK"));
  } else if (fiveHourDue) {
    Serial.println(F("5H"));
  } else {
    Serial.println(F("WK"));
  }

  runUsageCycle();

  // Der Sonderabruf wird zum neuen Taktanker. Damit folgt der naechste
  // regulaere Poll erst wieder 5 Minuten spaeter und es entstehen keine
  // nahezu doppelten Requests.
  lastUsagePollMs = millis();

  return true;
}

void runUsageCycle() {
  usageCycle++;

  Serial.println();
  Serial.print(F("========== USAGE CYCLE #"));
  Serial.print(usageCycle);
  Serial.println(F(" =========="));

  printHeapStats(F("vor Usage Cycle"));

  if (!connectWiFi()) {
    Serial.println(F("[USAGE] Kein WLAN; Cycle abgebrochen."));
    printHeapStats(F("nach Usage Cycle"));
    updateOLED(true);
    return;
  }

  if (!setupTime()) {
    Serial.println(F("[USAGE] Keine gueltige Zeit; TLS-Verifikation nicht moeglich."));
    printHeapStats(F("nach Usage Cycle"));
    updateOLED(true);
    return;
  }

  bool ok = fetchUsageWithAuthRecovery();
  if (!ok) Serial.println(F("[USAGE] Cycle fehlgeschlagen; letzter guter Wert bleibt erhalten."));

  printHeapStats(F("nach Usage Cycle"));
  updateOLED(true);
}

// ============================================================
// Setup / Loop
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(1500);

  // Reserven reduzieren String-Reallokationen / Heap-Fragmentierung.
  accessToken.reserve(2200);
  refreshToken.reserve(512);
  accountId.reserve(64);
  usage.plan.reserve(16);
  usage.creditBalance.reserve(24);

  Serial.println();
  Serial.println();
  Serial.println(F("########################################"));
  Serial.print(F("# "));
  Serial.print(PROJECT_NAME);
  Serial.print(F(" v"));
  Serial.println(PROJECT_VERSION);
  Serial.println(F("# ESP8266 / OLED 128x32 / Secure TLS"));
  Serial.println(F("########################################"));

  printHeapStats(F("Start"));

  initOLED();  // Bei fehlendem OLED laeuft der Monitor trotzdem ueber Serial weiter.

  if (!initFilesystem()) {
    Serial.println(F("STOP: LittleFS nicht verfuegbar."));
    return;
  }

  if (FORCE_RELOGIN) {
    clearStoredAuth();
    Serial.println(F("HINWEIS: FORCE_RELOGIN ist TRUE. Danach wieder FALSE setzen."));
  }

  oledStatus(F("WLAN"), F("verbinde..."));
  if (!connectWiFi()) {
    oledStatus(F("WLAN"), F("FEHLER"));
    Serial.println(F("STOP: WLAN nicht verfuegbar."));
    return;
  }

  // X.509-Pruefung braucht zwingend eine korrekte Uhrzeit.
  oledStatus(F("ZEIT / NTP"), F("synchronisiere"));
  if (!setupTime()) {
    oledStatus(F("NTP"), F("FEHLER"));
    Serial.println(F("STOP: Keine gueltige Zeit fuer TLS-Zertifikatspruefung."));
    return;
  }

  oledStatus(F("TLS"), F("pruefe..."));
  if (!initTrustAnchors()) {
    oledStatus(F("TLS"), F("FEHLER"));
    Serial.println(F("STOP: TLS Trust Anchors konnten nicht initialisiert werden."));
    return;
  }

  bool haveStoredAuth = loadStoredAuth();

  if (haveStoredAuth) {
    oledStatus(F("OPENAI"), F("authentifiziere"));
    RefreshResult result = refreshAccessToken();

    if (result == RefreshResult::OK) {
      // Auth ist abgeschlossen; diese Anzeige bleibt sichtbar, bis
      // der unmittelbar folgende Usage-Cycle die Tankanzeige uebernimmt.
      oledStatus(F("OPENAI"), F("AUTH OK"));

    } else if (result == RefreshResult::PERMANENT_FAILURE) {
      clearStoredAuth();
      haveStoredAuth = false;

    } else if (result == RefreshResult::TEMPORARY_FAILURE) {
      Serial.println(F("[AUTH] Temporaerer Fehler; Loop versucht spaeter erneut."));
    }
  }

  if (!haveStoredAuth) {
    if (!performDeviceLogin()) {
      Serial.println(F("STOP: Device Login fehlgeschlagen."));
      return;
    }

    oledStatus(F("OPENAI"), F("AUTH OK"));
  }

  if (accessToken.length() > 0) {
    runUsageCycle();
    lastUsagePollMs = millis();
  }

  Serial.println();
  Serial.println(F("Dauerlauf aktiv: OpenAI-Abruf alle 5 Minuten."));
  Serial.println(F("Reset-Countdown wird lokal aus reset_at berechnet."));
  Serial.print(F("Reset-Sonderabruf: "));
  Serial.print(RESET_REFRESH_DELAY_SEC);
  Serial.println(F("s nach reset_at."));
  Serial.println(F("TLS-Zertifikatspruefung ist aktiv; kein setInsecure()."));
  Serial.println(F("Zeitzone: Europe/Berlin (CET/CEST automatisch)."));
  Serial.println(F("OLED: Restkontingent 100% = voll, 0% = leer."));
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  // Falls Zeit verloren/ungueltig ist, vor dem naechsten HTTPS-Versuch NTP.
  if (!timeIsValid() && WiFi.status() == WL_CONNECTED) {
    setupTime();
  }

  // Temporaerer Refresh-Fehler beim Boot: spaeter erneut versuchen.
  if (accessToken.length() == 0 && refreshToken.length() > 0) {
    if (millis() - lastAuthRetryMs >= AUTH_RETRY_MS) {
      lastAuthRetryMs = millis();

      if (timeIsValid()) {
        RefreshResult result = refreshAccessToken();

        if (result == RefreshResult::OK) {
          Serial.println(F("[AUTH] Retry erfolgreich -> Usage sofort abrufen."));
          oledStatus(F("OPENAI"), F("AUTH OK"));
          runUsageCycle();
          lastUsagePollMs = millis();

        } else if (result == RefreshResult::PERMANENT_FAILURE) {
          clearStoredAuth();

          if (performDeviceLogin()) {
            Serial.println(F("[AUTH] Neuer Login erfolgreich -> Usage sofort abrufen."));
            oledStatus(F("OPENAI"), F("AUTH OK"));
            runUsageCycle();
            lastUsagePollMs = millis();
          }
        }
      }
    }
  }

  // reset_at ist bereits bekannt. 30 Sekunden nach einem Reset einmalig
  // sofort nachfassen, statt bis zum naechsten 5-Minuten-Takt zu warten.
  // Wenn ein Sonderabruf gelaufen ist, beginnt der regulaere Takt ab dort neu.
  bool resetRefreshRan = runResetAwareUsageRefreshIfDue();

  if (!resetRefreshRan &&
      accessToken.length() > 0 &&
      millis() - lastUsagePollMs >= USAGE_INTERVAL_MS) {
    lastUsagePollMs = millis();
    runUsageCycle();
  }

  // OLED wird lokal aktualisiert; kein zusaetzlicher OpenAI-Request.
  updateOLED(false);

  delay(50);
  yield();
}
