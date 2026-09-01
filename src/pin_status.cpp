#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsServer.h>
#include <hardware/gpio.h>
#include <WiFiClient.h>
#include <MQTT.h>

// NOTE: Does not work. Probably cuz casting to uint32_t and & != casting to uint8_t and &
// Get pin mode, stolen from https://github.com/arduino/ArduinoCore-API/issues/179
//
// On success returns: INPUT, INPUT_PULLUP, OUTPUT
// On invalid pins returns: -1
// static int getPinMode(uint8_t pin) {
//     if (pin >= NUM_DIGITAL_PINS) return (-1);
//
//     uint8_t bit = digitalPinToBitMask(pin);
//     uint8_t port = digitalPinToPort(pin);
//     volatile uint8_t *reg = portModeRegister(port);
//     if (*reg & bit) return (OUTPUT);
//
//     volatile uint8_t *out = portOutputRegister(port);
//     return ((*out & bit) ? INPUT_PULLUP : INPUT);
// }

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
            this->d[i].mode = gpio_is_dir_out(pin) ? 1 : 0;
            this->d[i].value = digitalRead(pin);
        }
        for (int i = 0; i < 4; i++) {
            int pin = 26 + i;
            this->a[i].mode = gpio_is_dir_out(pin) ? 1 : 0;
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

    Serial.printf("Sending update: %s\n", output.c_str());

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

static void connectMqtt() {
    // No need to check for WiFi Status
    // WiFiClient's apparently just a glorified TCP wrapper
    // (source: some random ai)

    while (!mqttClient.connect("")) {
        Serial.print("MQTT connecting...\n");
        delay(1000);
    }
    Serial.print("MQTT connected!\n");
}

void initPinWebSocket() {
    pinWebSocket.begin();
    pinWebSocket.onEvent(pinWebSocketEvent);

    mqttClient.begin("192.168.42.16", 1883, net);
    connectMqtt();
}

void loopPinWebSocket() {
    static unsigned long prev = 0;
    auto now = millis();

    pinWebSocket.loop();
    mqttClient.loop();

    if (!mqttClient.connected()) {
        Serial.print("MQTT Disconnected!\n");
        Serial.print("MQTT Reconnecting...\n");
        connectMqtt();
    }

    if (now - prev > 1000) {
        prev = now;

        prevPinState = currPinState;
        currPinState.poll();
        if (!currPinState.equal(prevPinState)) {
            sendUpdate(currPinState);
        }

        mqttClient.publish("A/0", String(currPinState.a[0].value));
        mqttClient.publish("A/1", String(currPinState.a[1].value));
        mqttClient.publish("A/2", String(currPinState.a[2].value));
        mqttClient.publish("A/3", String(currPinState.a[3].value));

        // currPinState.dump();
    }
}
