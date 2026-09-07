#include <Arduino.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <FreeRTOS.h>
#include <WiFiClient.h>

#include "lua.hpp"
#include "dhcp.hpp"
#include "status.hpp"

#if defined(USE_LAN8651) && !defined(USE_ETHUSB)

#include "phy.hpp"

// PHY Servicing Task
static TaskHandle_t loopPhyHandle = NULL;

static void setupNetwork() {
  // WARN: dunno if constantly servicing PHY in another core will be good
  //  since it also interacts with LWIP even though it should be thread-safe
  //  because LWIP in earlephilhower runs in its own thread.
  //  (LWIP functions call earlephilhower's wrappers which queue calls
  //  to the LWIP thread)
  //
  // 1024-word stack size is random, should probably check if it's too big or too small.
  // Currently works tho
  initPhy();
  xTaskCreate(loopPhy, "loopPhy", 1024, NULL, 1, &loopPhyHandle);

  initDHCP();
}

#elif defined(USE_ETHUSB) && !defined(USE_LAN8651)

#include <NCMEthernetlwIP.h>

static NCMEthernetlwIP eth;

static void setupNetwork() {
  eth.begin();
  while (!eth.connected()) {
    Serial.print("Trying to connect to Ethernet over USB\n");
    delay(1000);
  }
  Serial.print("IP address: ");
  Serial.println(eth.localIP());
}

#else
#error Choose either -DUSE_LAN8651 or -DUSE_ETHUSB
static void setupNetwork() {}
#endif

static WiFiClient net;

static WebServer server(80);

void handleRoot() {
    File f = LittleFS.open("/index.html", "r");
    if (!f) {
      Serial.print("can't open /index.html\n");
      return;
    }

    Serial.print("Sending index.html\n");
    server.streamFile(f, "text/html", 200);
    Serial.print("Sent index.html\n");
}

void handleList() {
    String json = "[";
    Dir root = LittleFS.openDir("/");
    bool first = true;
    while (root.next()) {
        if (!first)
            json += ",";
        json += "{\"name\":\"" + root.fileName() + "\"}";
        first = false;
    }
    json += "]";
    server.send(200, "application/json", json);
}

void handleRead() {
    String path = server.arg("path");
    if (LittleFS.exists(path)) {
        File f = LittleFS.open(path, "r");
        if (f) {
            String content = f.readString();
            f.close();
            server.send(200, "text/plain", content);
        } else {
            server.send(500, "text/plain", "Error opening file");
        }
    } else {
        server.send(200, "text/plain", "");
    }
}

void handleUpload() {
    String path = server.arg("path"); if (path == "") path = "/main.lua";
    File f = LittleFS.open(path, "w");
    if (f) {
      f.print(server.arg("plain"));
      f.close();
      server.send(200, "text/plain", "Saved");
    }
    else server.send(500, "text/plain", "Error");
}

void handleCreate() {
    String path = server.arg("path");
    File f = LittleFS.open(path, "w");
    if (f) {
        f.close();
        server.send(200);
    } else {
        server.send(500);
    }
}

void handleDelete() {
    String path = server.arg("path");
    if (LittleFS.remove(path))
        server.send(200);
    else
        server.send(500);
}

// NOTE: handleRun() and handleStop() probably cannot be called simultaneously?
void handleRun() {
    auto code = server.arg("plain");
    runLua(code);
    server.send(200, "text/plain", "OK");
}

void handleStop() {
    stopLua();
    server.send(200, "text/plain", "Stop Issued");
}

void setup() {
    Serial.begin(115200);
    while (!Serial) {}

    LittleFS.begin();

    Serial.print("/\n");
    Dir root = LittleFS.openDir("/");
    while (root.next()) {
        Serial.printf("|- %s\n", root.fileName().c_str());
    }

    setupNetwork();

    initStatus();

    server.on("/", handleRoot);
    server.on("/list_files", handleList);
    server.on("/read", handleRead);
    server.on("/upload", HTTP_POST, handleUpload);
    server.on("/create", HTTP_POST, handleCreate);
    server.on("/delete", HTTP_POST, handleDelete);
    server.on("/run", HTTP_POST, handleRun);
    server.on("/stop", HTTP_POST, handleStop);
    server.begin();

    initLua();
}

void loop() {
    server.handleClient();
    loopLua();
    loopStatus();
}
