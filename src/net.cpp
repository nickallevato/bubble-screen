// SPDX-License-Identifier: GPL-3.0-only
#include "net.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static LiveStatus g_live = {};
static char g_host[64] = "", g_psk[80] = "", g_ssid[40] = "", g_pass[72] = "";   // all set over serial
static uint16_t g_port = 8000;
static volatile bool g_reload = false, g_wifiChanged = false;

// Long-poll: the server holds the request until the status changes (<= 25 s), so
// changes arrive within ~1 s. One keep-alive connection is reused across polls, so
// a lossy link only pays for a TCP handshake after a failure. The connect timeout
// leaves room for a couple of lost SYNs; the read outlasts the server's 25 s hold.
static const uint32_t RETRY_MS = 1000, CONNECT_TIMEOUT_MS = 5000, READ_TIMEOUT_MS = 35000, MAX_BODY = 3072;
static WiFiClient g_client;
static HTTPClient g_http;
static uint32_t g_version = 0; static bool g_haveVersion = false;

static uint8_t idxOf(const char *s, const char *const *names, int n, uint8_t dflt) {
  for (int i = 0; s && i < n; i++) if (!strcmp(s, names[i])) return i;
  return dflt;
}

static void load() {
  Preferences p;
  if (p.begin("bubble", true)) {
    if (p.isKey("ssid")) p.getString("ssid", g_ssid, sizeof g_ssid);
    if (p.isKey("pass")) p.getString("pass", g_pass, sizeof g_pass);
    if (p.isKey("host")) p.getString("host", g_host, sizeof g_host);
    if (p.isKey("psk")) p.getString("psk", g_psk, sizeof g_psk);
    g_port = p.getUShort("port", 8000);
    p.end();
  }
}

static bool fetch_once() {
  static const char *STATES[] = {"sleep", "think", "work", "ask", "error"};
  static const char *ICONS[] = {"cal", "list", "cart", "mail"};
  char url[140];
  if (g_haveVersion) snprintf(url, sizeof url, "http://%s:%u/api/now?v=%lu&wait=25", g_host, g_port, (unsigned long)g_version);
  else snprintf(url, sizeof url, "http://%s:%u/api/now", g_host, g_port);
  HTTPClient &http = g_http;
  http.setReuse(true); http.setConnectTimeout(CONNECT_TIMEOUT_MS); http.setTimeout(READ_TIMEOUT_MS);
  if (!http.begin(g_client, url)) return false;
  char auth[96]; snprintf(auth, sizeof auth, "Bearer %s", g_psk);
  http.addHeader("Authorization", auth);
  int code = http.GET();
  if (code != 200) { Serial.printf("[net] http %d from %s\n", code, url); http.end(); g_client.stop(); return false; }
  int len = http.getSize();
  if (len > (int)MAX_BODY) { Serial.printf("[net] body too big (%d)\n", len); http.end(); g_client.stop(); return false; }
  String body = http.getString(); http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body) || !doc["state"].is<const char *>()) { Serial.println("[net] bad json, keeping last state"); return false; }

  LiveStatus s = {};
  s.have = true; s.ok = doc["ok"] | false;
  s.state = idxOf(doc["state"], STATES, 5, 0);
  s.icon = idxOf(doc["icon"], ICONS, 4, 1);
  strlcpy(s.title, doc["title"] | "", sizeof s.title);
  strlcpy(s.detail, doc["detail"] | "", sizeof s.detail);
  strlcpy(s.next, doc["next"] | "", sizeof s.next);
  strlcpy(s.agent, doc["agent"] | "", sizeof s.agent);
  s.since = doc["since"] | 0u; s.updated = doc["updated"] | 0u; s.now = doc["now"] | 0u; s.waiting = doc["waiting"] | 0;
  s.nHelpers = 0;
  for (JsonObject h : doc["helpers"].as<JsonArray>()) {
    if (s.nHelpers >= 4) break;
    Helper &d = s.helpers[s.nHelpers++];
    strlcpy(d.id, h["id"] | "", sizeof d.id); strlcpy(d.name, h["name"] | "HELPER", sizeof d.name);
    const char *m = h["model"] | "sonnet"; d.model = !strcmp(m, "opus") ? 2 : !strcmp(m, "haiku") ? 0 : 1;
    const char *sd = h["side"] | "L"; d.side = sd[0] == 'R';
    const char *st = h["state"] | "working"; d.state = !strcmp(st, "done") ? 1 : !strcmp(st, "failed") ? 2 : 0;
    strlcpy(d.line, h["line"] | "", sizeof d.line); d.at = h["at"] | 0u;
  }
  s.lastGoodMs = millis();
  if (doc["v"].is<uint32_t>()) { g_version = doc["v"].as<uint32_t>(); g_haveVersion = true; }
  static uint8_t lastState = 255; static uint32_t lastLog = 0;
  if (s.state != lastState || millis() - lastLog > 60000) {
    Serial.printf("[net] ok state=%s waiting=%u rssi=%d title=%s\n", STATES[s.state], s.waiting, WiFi.RSSI(), s.title);
    lastState = s.state; lastLog = millis();
  }
  portENTER_CRITICAL(&mux); g_live = s; portEXIT_CRITICAL(&mux);
  return true;
}

static void net_task(void *) {
  WiFi.setAutoReconnect(true);
  if (*g_ssid) WiFi.begin(g_ssid, g_pass); else Serial.println("[net] no wifi ssid saved; use: set ssid <name> / set pass <psk>");
  bool up = false;
  for (;;) {
    if (g_reload) { g_reload = false; load();
      if (g_wifiChanged) { g_wifiChanged = false; WiFi.disconnect(); delay(100); WiFi.begin(g_ssid, g_pass); up = false; } }
    if (WiFi.status() != WL_CONNECTED) {
      if (up) { Serial.println("[net] wifi down"); up = false; }
      vTaskDelay(pdMS_TO_TICKS(500)); continue;
    }
    if (!up) { Serial.printf("[net] wifi up ip=%s\n", WiFi.localIP().toString().c_str()); up = true; }
    if (!*g_host || !*g_psk) { vTaskDelay(pdMS_TO_TICKS(RETRY_MS)); continue; }   // nothing to talk to yet
    if (fetch_once()) vTaskDelay(pdMS_TO_TICKS(20));     // straight back into the next long-poll
    else { g_haveVersion = false; vTaskDelay(pdMS_TO_TICKS(RETRY_MS)); }
  }
}

void net_reserve() { WiFi.mode(WIFI_STA); WiFi.setSleep(false); }

bool net_configured() { load(); return *g_psk && *g_host; }

void net_begin() {
  load();
  Serial.printf("[net] ssid=%s host=%s:%u psk=%s\n", g_ssid, g_host, g_port, *g_psk ? "set" : "MISSING");
  xTaskCreatePinnedToCore(net_task, "net", 8192, nullptr, 1, nullptr, 0);
}

bool net_snapshot(LiveStatus &out) {
  portENTER_CRITICAL(&mux); out = g_live; portEXIT_CRITICAL(&mux);
  return out.have;
}

void net_set(const char *key, const char *val) {
  Preferences p; p.begin("bubble", false);
  if (!strcmp(key, "port")) p.putUShort("port", (uint16_t)atoi(val)); else p.putString(key, val);
  p.end();
  if (!strcmp(key, "ssid") || !strcmp(key, "pass")) g_wifiChanged = true;
  g_reload = true;
}
