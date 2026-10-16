#include "net.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <time.h>

#include "settings.h"

static DNSServer dns;
static bool portal = false;
static bool mdnsUp = false;
static bool connecting = false;
static uint32_t tConnect = 0;
static String apSsid;

static void startConnect() {
  WiFi.setHostname(settings.hostname.c_str());
  WiFi.begin(settings.wifiSsid.c_str(), settings.wifiPass.c_str());
  tConnect = millis();
  connecting = true;
  Serial.printf("[net] connecting to \"%s\"\n", settings.wifiSsid.c_str());
}

static void startPortal() {
  WiFi.mode(WIFI_AP_STA);
  String mac = WiFi.macAddress();
  apSsid = "HEXADDON-" + mac.substring(12, 14) + mac.substring(15, 17);
  WiFi.softAP(apSsid.c_str());
  dns.start(53, "*", WiFi.softAPIP());
  portal = true;
  Serial.printf("[net] setup portal: \"%s\" ip=%s\n", apSsid.c_str(),
                WiFi.softAPIP().toString().c_str());
}

static void stopPortal() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  portal = false;
  apSsid = "";
  Serial.println(F("[net] portal closed, STA connected"));
}

void netBegin() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  // NTP clock (for the Clock display mode): syncs whenever STA is up.
  // The timezone comes from settings.tz (applied in Settings::sanitize).
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  Serial.printf("[net] configured=%d\n", (int)settings.wifiConfigured);
  if (settings.wifiConfigured) {
    startConnect();
  } else {
    startPortal();
  }
}

void netLoop() {
  static int lastSta = -1;
  static uint32_t lastConn = 0;  // last moment STA was connected (0 = never)
  int st = (int)WiFi.status();
  if (st != lastSta) {
    lastSta = st;
    Serial.printf("[net] sta status=%d ip=%s\n", st, WiFi.localIP().toString().c_str());
  }
  if (portal) dns.processNextRequest();

  bool conn = (WiFi.status() == WL_CONNECTED);

  if (conn) {
    connecting = false;
    lastConn = millis();
    if (!mdnsUp) {
      static uint32_t mdnsRetry = 0;  // don't hammer MDNS.begin() every loop
      if ((int32_t)(millis() - mdnsRetry) >= 0) {
        if (MDNS.begin(settings.hostname.c_str())) {
          MDNS.addService("http", "tcp", 80);
          mdnsUp = true;
          Serial.printf("[net] mDNS: http://%s.local\n", settings.hostname.c_str());
        } else {
          mdnsRetry = millis() + 5000;
        }
      }
    }
    if (portal && settings.wifiConfigured) stopPortal();
  } else if (settings.wifiConfigured) {
    if (!portal && connecting && millis() - tConnect > 20000) {
      // credentials are wrong or network is down -> give user the setup portal
      startPortal();
    } else if (!portal && !connecting && lastConn &&
               millis() - lastConn > 20000) {
      // network dropped AFTER a successful connection and the built-in
      // auto-reconnect hasn't helped for 20s -> open the setup portal
      // (STA keeps retrying below), otherwise the device is unreachable
      Serial.println(F("[net] connection lost, opening portal"));
      startPortal();
      tConnect = millis();
    }
    if (portal && millis() - tConnect > 25000) {
      WiFi.reconnect();
      tConnect = millis();
    }
  }
}

bool netIsPortal() {
  return portal;
}
