#include "ota.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "settings.h"  // FW_VERSION

// Release feed of this project (keep in sync with the git remote / CI).
// The tag is "v<FW_VERSION>" (see release.yml), the asset "firmware.bin"
// is the OTA image (same file the web flasher uses).
#define GH_RELEASE_API "https://api.github.com/repos/kainsamara/hexaddon/releases/latest"
#define REBOOT_DELAY_MS 4000UL   // let the UI pick up the final status

static OtaState st;

// ---------------- version compare ----------------

static bool parseVer(const String &v, int out[3]) {
  return sscanf(v.c_str(), "%d.%d.%d", &out[0], &out[1], &out[2]) > 0;
}

bool otaNewerThan(const String &latest) {
  int la[3] = {0, 0, 0}, cu[3] = {0, 0, 0};
  parseVer(latest, la);
  parseVer(FW_VERSION, cu);
  for (int i = 0; i < 3; i++) {
    if (la[i] != cu[i]) return la[i] > cu[i];
  }
  return false;
}

// ---------------- release check (background) ----------------
// GitHub API answers with transfer-encoding: chunked, which HTTPClient cannot
// decode on a raw stream — useHTTP10() forces a plain body instead.
// TLS without a pinned CA (setInsecure): the downloaded image is verified by
// the bootloader's SHA-256 anyway, so a broken download can never boot.

static void checkTask(void *) {
  WiFiClientSecure cl;
  cl.setInsecure();
  HTTPClient http;
  bool ok = false;
  if (http.begin(cl, GH_RELEASE_API)) {
    http.useHTTP10(true);
    http.setTimeout(12000);
    http.addHeader("Accept", "application/vnd.github+json");
    int code = http.GET();
    if (code == 200) {
      JsonDocument d;
      if (!deserializeJson(d, http.getString())) {
        String tag = d["tag_name"] | "";
        if (tag.length()) {
          if (tag[0] == 'v' || tag[0] == 'V') tag.remove(0, 1);
          String url;
          for (JsonVariant a : d["assets"].as<JsonArray>()) {
            if (String(a["name"] | "") == "firmware.bin") {
              url = a["browser_download_url"] | "";
              break;
            }
          }
          if (url.length()) {
            st.latest = tag;
            st.url = url;
            st.netError = false;
            ok = true;
          }
        }
      }
    } else {
      Serial.printf("[ota] github api http %d\n", code);
    }
    http.end();
  }
  if (!ok) st.netError = true;
  st.checked = true;
  st.checkedAt = millis();
  st.checking = false;
  vTaskDelete(NULL);
}

void otaCheckAsync() {
  if (st.checking) return;
  st.checking = true;
  if (xTaskCreate(checkTask, "otaChk", 16384, nullptr, 5, nullptr) != pdPASS)
    st.checking = false;
}

// ---------------- download + flash (background) ----------------

static void otaFail(const char *what) {
  st.stage = "error";
  st.error = what;
  if (Update.isRunning()) Update.end(false);
  Serial.printf("[ota] %s\n", what);
}

static void downloadTask(void *) {
  String url = st.url;
  st.progress = 0;
  st.stage = "download";

  WiFiClientSecure cl;
  cl.setInsecure();
  cl.setTimeout(15);  // seconds, WiFiClient API
  HTTPClient http;
  if (!http.begin(cl, url)) {
    otaFail("cannot start download");
    vTaskDelete(NULL);
    return;
  }
  http.useHTTP10(true);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(20000);

  int code = http.GET();
  if (code != 200) {
    otaFail("github returned an error");
    http.end();
    vTaskDelete(NULL);
    return;
  }

  int total = http.getSize();  // -1 if unknown; github always sends it
  if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
    otaFail(Update.errorString());
    http.end();
    vTaskDelete(NULL);
    return;
  }

  WiFiClient *s = http.getStreamPtr();
  uint8_t buf[2048];
  uint32_t written = 0, idle = 0;
  while (http.connected() && (total < 0 || written < (uint32_t)total)) {
    int avail = s->available();
    if (avail <= 0) {
      if (++idle > 15000) {  // ~15 s without data
        otaFail("download stalled");
        http.end();
        vTaskDelete(NULL);
        return;
      }
      delay(1);
      continue;
    }
    idle = 0;
    int n = s->readBytes(buf, (size_t)avail < sizeof(buf) ? (size_t)avail : sizeof(buf));
    if (n <= 0) break;
    if (Update.write(buf, n) != n) {
      otaFail(Update.errorString());
      http.end();
      vTaskDelete(NULL);
      return;
    }
    written += n;
    if (total > 0) st.progress = (int)(written * 100 / (uint32_t)total);
  }
  http.end();

  if ((total > 0 && written != (uint32_t)total) || !Update.end(true)) {
    otaFail(written != (uint32_t)total && total > 0 ? "short download"
                                                    : Update.errorString());
    vTaskDelete(NULL);
    return;
  }

  Serial.printf("[ota] github download ok: %u bytes, rebooting\n",
                (unsigned)written);
  st.progress = 100;
  st.stage = "done";
  // give the UI a moment to fetch the final status, then swap slots
  uint32_t t0 = millis();
  while (millis() - t0 < REBOOT_DELAY_MS) delay(100);
  ESP.restart();
}

bool otaStart(String &err) {
  if (st.stage == "download") {
    err = "update is already running";
    return false;
  }
  if (!st.checked || !st.url.length()) {
    err = "no update info, run a check first";
    return false;
  }
  if (xTaskCreate(downloadTask, "otaDl", 16384, nullptr, 5, nullptr) != pdPASS) {
    err = "cannot start task";
    return false;
  }
  return true;
}

const OtaState &otaState() { return st; }
