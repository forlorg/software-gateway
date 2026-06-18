#include "system/ota_manager.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include <ctype.h>
#include <string.h>

#include "config/ota_config.h"
#include "system/gateway_context.h"
#include "system/version.h"

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "mbedtls/sha256.h"
#include "mbedtls/version.h"

namespace gateway::ota {
namespace {

static constexpr const char* kTag = "OTA";

struct OtaManifest {
  bool update_available = false;
  String project;
  String hw;
  String channel;
  String version;
  uint32_t build = 0;
  bool mandatory = false;
  String firmware_url;
  String firmware_path;
  size_t size = 0;
  String md5;
  String sha256;
  String release_notes;
};

struct Sha256Context {
  mbedtls_sha256_context ctx;
};

portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;

State g_state = State::Disabled;
bool g_check_requested = false;
bool g_upgrade_requested = false;
bool g_auto_first_check_done = false;
bool g_have_manifest = false;
bool g_update_available = false;
uint8_t g_progress = 0;
uint32_t g_latest_build = 0;
String g_latest_version;
String g_last_error;
OtaManifest g_manifest;

uint32_t g_started_ms = 0;
uint32_t g_last_check_ms = 0;
uint32_t g_next_retry_ms = 0;
uint32_t g_retry_backoff_ms = gateway::ota_config::kRetryBackoffMinMs;

void set_state(State s) {
  portENTER_CRITICAL(&g_lock);
  g_state = s;
  portEXIT_CRITICAL(&g_lock);
}

void set_progress(uint8_t p) {
  portENTER_CRITICAL(&g_lock);
  g_progress = p;
  portEXIT_CRITICAL(&g_lock);
}

void set_last_error(const char* err) {
  portENTER_CRITICAL(&g_lock);
  g_last_error = err ? err : "";
  portEXIT_CRITICAL(&g_lock);
  Serial.printf("[%s] error=%s\n", kTag, err ? err : "");
}

void clear_last_error() {
  portENTER_CRITICAL(&g_lock);
  g_last_error = "";
  portEXIT_CRITICAL(&g_lock);
}

bool is_busy_state(State s) {
  return s == State::CheckManifest ||
         s == State::Downloading ||
         s == State::Verifying ||
         s == State::Applying ||
         s == State::Rebooting;
}

bool is_hex_string(const String& s, size_t expected_len) {
  if (s.length() != expected_len) {
    return false;
  }
  for (size_t i = 0; i < s.length(); ++i) {
    if (!isxdigit(static_cast<unsigned char>(s[i]))) {
      return false;
    }
  }
  return true;
}

bool is_valid_version(const String& version) {
  if (version.length() == 0 || version.length() > 32) {
    return false;
  }
  for (size_t i = 0; i < version.length(); ++i) {
    const char c = version[i];
    const bool ok = isalnum(static_cast<unsigned char>(c)) ||
                    c == '.' ||
                    c == '_' ||
                    c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

bool equals_ignore_case(const String& a, const String& b) {
  if (a.length() != b.length()) {
    return false;
  }
  for (size_t i = 0; i < a.length(); ++i) {
    if (tolower(static_cast<unsigned char>(a[i])) !=
        tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

String url_encode(const String& value) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  out.reserve(value.length() * 3);
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if ((c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

String mac_compact() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.toLowerCase();
  if (mac.length() == 0) {
    return "unknown";
  }
  return mac;
}

String product_id_or_unknown() {
  char product_id[9]{};
  if (gateway::ctx::has_product_id()) {
    gateway::ctx::get_product_id_hex(product_id);
  }
  if (product_id[0] == '\0') {
    return "unknown";
  }
  return String(product_id);
}

String build_base_url() {
  String base = String(gateway::ota_config::kApiScheme) + "://" +
                gateway::ota_config::kApiHost;

  const uint16_t port = gateway::ota_config::kApiPort;
  const bool default_http =
      strcmp(gateway::ota_config::kApiScheme, "http") == 0 && port == 80;
  const bool default_https =
      strcmp(gateway::ota_config::kApiScheme, "https") == 0 && port == 443;

  if (!default_http && !default_https) {
    base += ":" + String(port);
  }

  return base;
}

String expected_firmware_path(const String& version) {
  return String("/api/v1/ota/") +
         gateway::version::project_name() +
         "/" +
         version +
         "/" +
         gateway::ota_config::kFirmwareFileName;
}

String build_check_url() {
  String url = build_base_url() + gateway::ota_config::kCheckPath;

  url += "?project=" + url_encode(gateway::version::project_name());
  url += "&hw=" + url_encode(gateway::version::hardware());
  url += "&channel=" + url_encode(gateway::version::channel());
  url += "&version=" + url_encode(gateway::version::firmware_version());
  url += "&build=" + String(gateway::version::firmware_build());
  url += "&device_id=" + url_encode(mac_compact());
  url += "&product_id=" + url_encode(product_id_or_unknown());
  url += "&mac=" + url_encode(WiFi.macAddress());

  return url;
}

bool begin_http(HTTPClient& http,
                WiFiClient& plain,
                WiFiClientSecure& secure,
                const String& url) {
  http.setTimeout(gateway::ota_config::kHttpTimeoutMs);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);

  if (gateway::ota_config::kUseTls) {
    secure.setCACert(gateway::ota_config::kRootCaPem);
    return http.begin(secure, url);
  }

  return http.begin(plain, url);
}

void sha256_init(Sha256Context* c) {
  mbedtls_sha256_init(&c->ctx);
#if MBEDTLS_VERSION_MAJOR >= 3
  mbedtls_sha256_starts(&c->ctx, 0);
#else
  mbedtls_sha256_starts_ret(&c->ctx, 0);
#endif
}

void sha256_update(Sha256Context* c, const uint8_t* data, size_t len) {
#if MBEDTLS_VERSION_MAJOR >= 3
  mbedtls_sha256_update(&c->ctx, data, len);
#else
  mbedtls_sha256_update_ret(&c->ctx, data, len);
#endif
}

String sha256_final_hex(Sha256Context* c) {
  uint8_t out[32];
#if MBEDTLS_VERSION_MAJOR >= 3
  mbedtls_sha256_finish(&c->ctx, out);
#else
  mbedtls_sha256_finish_ret(&c->ctx, out);
#endif
  mbedtls_sha256_free(&c->ctx);

  char hex[65];
  for (int i = 0; i < 32; ++i) {
    snprintf(hex + i * 2, 3, "%02x", out[i]);
  }
  hex[64] = '\0';
  return String(hex);
}

size_t ota_partition_size() {
  const esp_partition_t* p = esp_ota_get_next_update_partition(nullptr);
  if (!p) {
    return 0;
  }
  return p->size;
}

String firmware_url_from_manifest(const OtaManifest& m) {
  const String expected_path = expected_firmware_path(m.version);
  if (m.firmware_path == expected_path) {
    return build_base_url() + m.firmware_path;
  }

  if (m.firmware_path.length() == 0 && m.firmware_url.length() > 0) {
    return m.firmware_url;
  }

  return "";
}

bool is_allowed_firmware_url(const String& url, const String& version) {
  const String expected = build_base_url() + expected_firmware_path(version);
  return url == expected;
}

bool validate_manifest(const OtaManifest& m) {
  if (m.project != gateway::version::project_name()) {
    set_last_error("manifest_project_mismatch");
    return false;
  }

  if (m.hw != gateway::version::hardware()) {
    set_last_error("manifest_hw_mismatch");
    return false;
  }

  if (m.channel != gateway::version::channel()) {
    set_last_error("manifest_channel_mismatch");
    return false;
  }

  if (!is_valid_version(m.version)) {
    set_last_error("manifest_version_invalid");
    return false;
  }

  if (m.build <= gateway::version::firmware_build()) {
    set_last_error("manifest_build_not_newer");
    return false;
  }

  if (m.size == 0) {
    set_last_error("manifest_size_invalid");
    return false;
  }

  const size_t part_size = ota_partition_size();
  if (part_size > 0 && m.size > part_size) {
    set_last_error("manifest_size_too_large");
    return false;
  }

  if (!is_hex_string(m.md5, 32)) {
    set_last_error("manifest_md5_invalid");
    return false;
  }

  if (!is_hex_string(m.sha256, 64)) {
    set_last_error("manifest_sha256_invalid");
    return false;
  }

  const String expected_path = expected_firmware_path(m.version);
  if (m.firmware_path.length() > 0 && m.firmware_path != expected_path) {
    set_last_error("manifest_firmware_path_invalid");
    return false;
  }

  if (m.firmware_path.length() == 0 && m.firmware_url.length() == 0) {
    set_last_error("manifest_firmware_path_invalid");
    return false;
  }

  const String url = firmware_url_from_manifest(m);
  if (!is_allowed_firmware_url(url, m.version)) {
    set_last_error("firmware_url_not_allowed");
    return false;
  }

  return true;
}

bool fetch_manifest(OtaManifest* out) {
  if (WiFi.status() != WL_CONNECTED) {
    set_last_error("wifi_not_connected");
    return false;
  }

  set_state(State::CheckManifest);
  set_progress(0);
  clear_last_error();

  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;

  const String url = build_check_url();
  Serial.printf("[%s] check url: %s\n", kTag, url.c_str());

  if (!begin_http(http, plain, secure, url)) {
    set_last_error("manifest_http_begin_failed");
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[%s] manifest http code=%d\n", kTag, code);
    set_last_error("manifest_http_not_ok");
    http.end();
    return false;
  }

  const int content_length = http.getSize();
  if (content_length > static_cast<int>(gateway::ota_config::kManifestMaxBytes)) {
    set_last_error("manifest_body_too_large");
    http.end();
    return false;
  }

  const String body = http.getString();
  http.end();

  if (body.length() > gateway::ota_config::kManifestMaxBytes) {
    set_last_error("manifest_body_too_large");
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    set_last_error("manifest_json_parse_failed");
    return false;
  }

  if ((doc["ok"] | false) != true) {
    set_last_error("manifest_ok_false");
    return false;
  }

  out->update_available = doc["update_available"] | false;
  if (!out->update_available) {
    const char* reason = doc["reason"] | "";
    Serial.printf("[%s] no update reason=%s\n", kTag, reason);
    return true;
  }

  out->project = doc["project"] | "";
  out->hw = doc["hw"] | "";
  out->channel = doc["channel"] | "";
  out->version = doc["version"] | "";
  out->build = doc["build"] | 0;
  out->mandatory = doc["mandatory"] | false;
  out->firmware_path = doc["firmware_path"] | "";
  out->firmware_url = doc["firmware_url"] | "";
  out->size = doc["size"] | 0;
  out->md5 = doc["md5"] | "";
  out->sha256 = doc["sha256"] | "";
  out->release_notes = doc["release_notes"] | "";

  if (!validate_manifest(*out)) {
    return false;
  }

  Serial.printf("[%s] manifest ok, update available version=%s build=%lu size=%u\n",
                kTag,
                out->version.c_str(),
                static_cast<unsigned long>(out->build),
                static_cast<unsigned>(out->size));
  return true;
}

void store_manifest(const OtaManifest& m) {
  portENTER_CRITICAL(&g_lock);
  g_manifest = m;
  g_have_manifest = m.update_available;
  g_update_available = m.update_available;
  g_latest_version = m.update_available ? m.version : "";
  g_latest_build = m.update_available ? m.build : 0;
  portEXIT_CRITICAL(&g_lock);
}

void enter_backoff() {
  set_state(State::Backoff);
  const uint32_t now = millis();
  g_next_retry_ms = now + g_retry_backoff_ms;
  if (g_retry_backoff_ms < gateway::ota_config::kRetryBackoffMaxMs / 2) {
    g_retry_backoff_ms *= 2;
  } else {
    g_retry_backoff_ms = gateway::ota_config::kRetryBackoffMaxMs;
  }
}

void reset_backoff() {
  g_retry_backoff_ms = gateway::ota_config::kRetryBackoffMinMs;
  g_next_retry_ms = 0;
}

void update_progress(size_t written, size_t total) {
  uint8_t p = 0;
  if (total > 0) {
    p = static_cast<uint8_t>((written * 100UL) / total);
    if (p > 100) {
      p = 100;
    }
  }
  set_progress(p);
  static uint8_t last_printed = 255;
  if (p != last_printed && (p == 100 || p % 10 == 0)) {
    last_printed = p;
    Serial.printf("[%s] download progress=%u\n", kTag, static_cast<unsigned>(p));
  }
}

bool download_and_apply(const OtaManifest& manifest) {
  const String firmware_url = firmware_url_from_manifest(manifest);

  if (!is_allowed_firmware_url(firmware_url, manifest.version)) {
    set_last_error("firmware_url_not_allowed");
    return false;
  }

  Serial.printf("[%s] firmware url: %s\n", kTag, firmware_url.c_str());

  set_state(State::Downloading);
  set_progress(0);

  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;

  if (!begin_http(http, plain, secure, firmware_url)) {
    set_last_error("firmware_http_begin_failed");
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[%s] firmware http code=%d\n", kTag, code);
    set_last_error("firmware_http_not_ok");
    http.end();
    return false;
  }

  const int content_length = http.getSize();
  if (content_length <= 0 ||
      static_cast<size_t>(content_length) != manifest.size) {
    set_last_error("firmware_size_mismatch");
    http.end();
    return false;
  }

  if (!Update.begin(content_length, U_FLASH)) {
    set_last_error("update_begin_failed");
    http.end();
    return false;
  }

  Update.setMD5(manifest.md5.c_str());

  WiFiClient* stream = http.getStreamPtr();
  uint8_t buffer[gateway::ota_config::kDownloadBufferSize];

  Sha256Context sha_ctx;
  sha256_init(&sha_ctx);

  size_t written = 0;
  uint32_t last_rx_ms = millis();

  while (http.connected() && written < static_cast<size_t>(content_length)) {
    const size_t available = stream->available();

    if (available == 0) {
      if (millis() - last_rx_ms > gateway::ota_config::kDownloadIdleTimeoutMs) {
        Update.abort();
        set_last_error("download_idle_timeout");
        http.end();
        return false;
      }

      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    size_t to_read = available;
    if (to_read > sizeof(buffer)) {
      to_read = sizeof(buffer);
    }

    const int read_len = stream->readBytes(buffer, to_read);
    if (read_len <= 0) {
      Update.abort();
      set_last_error("stream_read_failed");
      http.end();
      return false;
    }

    last_rx_ms = millis();

    sha256_update(&sha_ctx, buffer, static_cast<size_t>(read_len));

    const size_t write_len = Update.write(buffer, static_cast<size_t>(read_len));
    if (write_len != static_cast<size_t>(read_len)) {
      Update.abort();
      set_last_error("flash_write_failed");
      http.end();
      return false;
    }

    written += write_len;
    update_progress(written, static_cast<size_t>(content_length));

    vTaskDelay(pdMS_TO_TICKS(1));
  }

  if (written != static_cast<size_t>(content_length)) {
    Update.abort();
    set_last_error("download_incomplete");
    http.end();
    return false;
  }

  set_state(State::Verifying);
  const String actual_sha256 = sha256_final_hex(&sha_ctx);
  if (!equals_ignore_case(actual_sha256, manifest.sha256)) {
    Update.abort();
    set_last_error("sha256_mismatch");
    http.end();
    return false;
  }

  Serial.printf("[%s] sha256 ok\n", kTag);

  set_state(State::Applying);
  if (!Update.end(true)) {
    Update.printError(Serial);
    set_last_error("update_end_failed");
    http.end();
    return false;
  }

  if (!Update.isFinished()) {
    set_last_error("update_not_finished");
    http.end();
    return false;
  }

  http.end();

  Serial.printf("[%s] update end ok, rebooting\n", kTag);
  set_state(State::Rebooting);
  delay(500);
  ESP.restart();
  return true;
}

void do_check(bool upgrade_after_check) {
  OtaManifest m;
  const bool ok = fetch_manifest(&m);
  g_last_check_ms = millis();

  if (!ok) {
    enter_backoff();
    return;
  }

  reset_backoff();

  if (!m.update_available) {
    store_manifest(m);
    set_state(State::UpToDate);
    return;
  }

  store_manifest(m);
  set_state(State::UpdateAvailable);

  if (upgrade_after_check || m.mandatory) {
    if (!download_and_apply(m)) {
      enter_backoff();
    }
  }
}

}  // namespace

void begin() {
  g_started_ms = millis();
  set_state(State::Idle);
  clear_last_error();
  Serial.printf("[%s] begin project=%s version=%s build=%lu hw=%s server=%s\n",
                kTag,
                gateway::version::project_name(),
                gateway::version::firmware_version(),
                static_cast<unsigned long>(gateway::version::firmware_build()),
                gateway::version::hardware(),
                build_base_url().c_str());
}

void loop() {
  State current;
  bool check_req = false;
  bool upgrade_req = false;

  portENTER_CRITICAL(&g_lock);
  current = g_state;
  check_req = g_check_requested;
  upgrade_req = g_upgrade_requested;
  g_check_requested = false;
  g_upgrade_requested = false;
  portEXIT_CRITICAL(&g_lock);

  if (is_busy_state(current)) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    if (current != State::WaitingNetwork) {
      Serial.printf("[%s] waiting network\n", kTag);
    }
    set_state(State::WaitingNetwork);
    return;
  }

  const uint32_t now = millis();

  if (current == State::Backoff && g_next_retry_ms != 0 && now < g_next_retry_ms) {
    return;
  }

  bool auto_due = false;
  if (!g_auto_first_check_done &&
      now - g_started_ms >= gateway::ota_config::kFirstCheckDelayMs) {
    g_auto_first_check_done = true;
    auto_due = true;
  } else if (g_last_check_ms != 0 &&
             now - g_last_check_ms >= gateway::ota_config::kCheckIntervalMs) {
    auto_due = true;
  }

  if (upgrade_req) {
    if (g_have_manifest && g_update_available) {
      OtaManifest m = g_manifest;
      if (!download_and_apply(m)) {
        enter_backoff();
      }
    } else {
      do_check(true);
    }
    return;
  }

  // 自动检查（首次/周期）：直接下载升级；手动 check / backoff 仅查询不升级
  if (auto_due) {
    do_check(true);
    return;
  }

  if (check_req || current == State::Backoff) {
    do_check(false);
    return;
  }

  if (current == State::WaitingNetwork || current == State::Backoff) {
    set_state(g_update_available ? State::UpdateAvailable : State::Idle);
  }
}

bool request_check_now() {
  const uint32_t now = millis();

  portENTER_CRITICAL(&g_lock);
  const bool busy = is_busy_state(g_state);
  const bool too_soon =
      g_last_check_ms != 0 &&
      now - g_last_check_ms < gateway::ota_config::kManualCheckMinIntervalMs;
  if (!busy && !too_soon) {
    g_check_requested = true;
  }
  portEXIT_CRITICAL(&g_lock);

  return !busy && !too_soon;
}

bool request_upgrade_now() {
  portENTER_CRITICAL(&g_lock);
  const bool busy = is_busy_state(g_state);
  if (!busy) {
    g_upgrade_requested = true;
  }
  portEXIT_CRITICAL(&g_lock);

  return !busy;
}

Status status() {
  Status s{};
  portENTER_CRITICAL(&g_lock);
  s.state = g_state;
  s.update_available = g_update_available;
  s.update_in_progress = is_busy_state(g_state);
  s.progress = g_progress;
  s.current_build = gateway::version::firmware_build();
  s.latest_build = g_latest_build;
  s.current_version = gateway::version::firmware_version();
  s.latest_version = g_latest_version.c_str();
  s.last_error = g_last_error.c_str();
  portEXIT_CRITICAL(&g_lock);
  return s;
}

bool update_in_progress() {
  portENTER_CRITICAL(&g_lock);
  const bool busy = is_busy_state(g_state);
  portEXIT_CRITICAL(&g_lock);
  return busy;
}

const char* state_string(State state) {
  switch (state) {
    case State::Disabled:
      return "disabled";
    case State::Idle:
      return "idle";
    case State::WaitingNetwork:
      return "waiting_network";
    case State::CheckManifest:
      return "check_manifest";
    case State::UpToDate:
      return "up_to_date";
    case State::UpdateAvailable:
      return "update_available";
    case State::Downloading:
      return "downloading";
    case State::Verifying:
      return "verifying";
    case State::Applying:
      return "applying";
    case State::Backoff:
      return "backoff";
    case State::Rebooting:
      return "rebooting";
    case State::Failed:
      return "failed";
    default:
      return "unknown";
  }
}

}  // namespace gateway::ota
