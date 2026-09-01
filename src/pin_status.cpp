#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsServer.h>
#include <hardware/gpio.h>
#include <WiFiClient.h>
#include <MQTT.h>

static int getPinMode(uint8_t pin) {
    if (pin >= NUM_DIGITAL_PINS) return -1;

    auto io = pads_bank0_hw->io[pin];
    bool output = gpio_is_dir_out(pin);
    bool pullup = io & (1 << PADS_BANK0_GPIO0_PUE_LSB) ? true : false;
    bool pulldown = io & (1 << PADS_BANK0_GPIO0_PDE_LSB) ? true : false;

    if (output) {
        // fun fact: it's also pulldown
        return OUTPUT;
    } else if (pullup == false && pulldown == false) {
        return INPUT;
    } else if (pullup == true && pulldown == false) {
        return INPUT_PULLUP;
    } else if (pullup == false && pulldown == true) {
        return INPUT_PULLDOWN;
    } else {
        // pullup + pulldown is "bus_keep" mode or something
        return -1;
    }
}

struct PinInfo {
    int mode;
    int value;

    bool equal(PinInfo rhs) {
        return this->mode == rhs.mode && this->value == rhs.value;
    }

    JsonDocument json() {
        JsonDocument doc;
        doc["mode"] = mode;
        doc["value"] = value;
        return doc;
    }
};

struct PinState {
    PinInfo d[6]; // Digital, GPIO 20 - 25
    PinInfo a[4]; // Analog, GPIO 26 - 29

    void poll() {
        for (int i = 0; i < 6; i++) {
            int pin = 20 + i;
            this->d[i].mode = getPinMode(pin);
            this->d[i].value = digitalRead(pin);
        }
        for (int i = 0; i < 4; i++) {
            int pin = 26 + i;
            this->a[i].mode = getPinMode(pin);
            this->a[i].value = analogRead(pin);
        }
    }

    bool equal(PinState rhs) {
        for (int i = 0; i < 6; i++) {
            if (!this->d[i].equal(rhs.d[i]))
                return false;
        }
        for (int i = 0; i < 4; i++) {
            if (!this->a[i].equal(rhs.a[i]))
                return false;
        }
        return true;
    }

    void dump() {
        Serial.print("Pin:\n");
        for (int i = 0; i < 6; i++) {
            Serial.printf("\tD%d = %d\n", i, d[i].value);
        }
        for (int i = 0; i < 4; i++) {
            Serial.printf("\tA%d = %d\n", i, a[i].value);
        }
    }
};

static PinState prevPinState{};
static PinState currPinState{};

static WebSocketsServer pinWebSocket(82);
static WiFiClient net;
static MQTTClient mqttClient;

static void sendUpdate(PinState pinState) {
    JsonDocument doc;

    for (int i = 0; i < 6; i++) {
        String key = "D";
        doc[key + i] = pinState.d[i].json();
    }

    for (int i = 0; i < 4; i++) {
        String key = "A";
        doc[key + i] = pinState.a[i].json();
    }

    String output;
    serializeJson(doc, output);

    Serial.printf("Sending update\n");
    // Serial.printf("Sending update: %s\n", output.c_str());

    pinWebSocket.broadcastTXT(output);
}

static void pinWebSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    switch(type) {
        case WStype_DISCONNECTED:
            {
                Serial.printf("[%u] Disconnected!\n", num);
                break;
            }
        case WStype_CONNECTED:
            {
                IPAddress ip = pinWebSocket.remoteIP(num);
                Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);
                break;
            }
        case WStype_TEXT:
            {
                Serial.printf("[%u] get Text: %s\n", num, payload);
                break;
            }
    }
}

void initPinWebSocket() {
    pinWebSocket.begin();
    pinWebSocket.onEvent(pinWebSocketEvent);

    mqttClient.begin("192.168.42.16", 1883, net);

    // No need to check for WiFi Status
    // WiFiClient's apparently just a glorified TCP wrapper
    // (source: some random ai)

    while (!mqttClient.connect("")) {
        Serial.print("MQTT connecting...\n");
        delay(1000);
    }
    Serial.print("MQTT connected!\n");
}

void loopPinWebSocket() {
    static unsigned long prev = 0;
    auto now = millis();

    pinWebSocket.loop();
    mqttClient.loop();

    if (!mqttClient.connected()) {
        Serial.print("MQTT Reconnecting...\n");
        if (mqttClient.connect("")) {
            Serial.print("MQTT connected!\n");
        }
    }

    if (now - prev > 1000) {
        prev = now;

        prevPinState = currPinState;
        currPinState.poll();
        if (!currPinState.equal(prevPinState)) {
            sendUpdate(currPinState);
        }

        if (mqttClient.connected()) {
            mqttClient.publish("A/0", String(currPinState.a[0].value));
            mqttClient.publish("A/1", String(currPinState.a[1].value));
            mqttClient.publish("A/2", String(currPinState.a[2].value));
            mqttClient.publish("A/3", String(currPinState.a[3].value));
        }

        // currPinState.dump();
    }
}
