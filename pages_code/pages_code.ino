#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "page.h"

const char* ssid = "GAME DROP";
const char* password = "";
#define USE_PUBLIC_IP 1

WebServer server(80);
DNSServer dns;

#if USE_PUBLIC_IP
const IPAddress apIP(4, 3, 2, 1);
#else
const IPAddress apIP(192, 168, 4, 1);
#endif
String apHost;
String portalUrl;


void handleRedirect() {
  server.sendHeader("Location", portalUrl, true);
  server.sendHeader("Cache-Control", "no-store");
  server.send(302, "text/plain", "");
}
void handlePage() {
  if (server.hostHeader().length() && server.hostHeader() != apHost) { handleRedirect(); return; }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html; charset=utf-8", page);
}
void handleAssets() {
  server.sendHeader("Cache-Control", "public, max-age=86400");
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/html; charset=utf-8", page);
}
void handleNothing() { server.send(404, "text/plain", ""); }


void setup() {
  Serial.begin(115200);
  delay(200);

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(ssid, password);
  apHost = apIP.toString();
  portalUrl = "http://" + apHost + "/";

  #if defined(ESP_IDF_VERSION_VAL) && ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 2)
    WiFi.AP.enableDhcpCaptivePortal();
  #endif

  dns.start(53, "*", apIP);

  server.on("/", handlePage);
  server.on("/generate_204", handlePage);
  server.on("/gen_204", handlePage);
  server.on("/hotspot-detect.html", handlePage);
  server.on("/ncsi.txt", handlePage);
  server.on("/connecttest.txt", handlePage);
  server.on("/redirect", handleRedirect);
  server.on("/success.txt", handleRedirect);
  server.on("/canonical.html", handleRedirect);
  server.on("/favicon.ico", handleNothing);
  server.on("/wpad.dat", handleNothing);
  server.onNotFound(handleRedirect);

  server.begin();
}

void loop() {
  dns.processNextRequest();
  server.handleClient();
}