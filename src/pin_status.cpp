#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsServer.h>

struct PinState {
    int d[6]; // Digital, GPIO 20 - 25
    int a[4]; // Analog, GPIO 26 - 29

    void poll() {
        // TODO: add pin mode
        for (int i = 0; i < 6; i++) {
            this->d[i] = digitalRead(20 + i);
        }
        for (int i = 0; i < 4; i++) {
            this->a[i] = analogRead(26 + i);
        }
    }

    bool equal(PinState rhs) {
        for (int i = 0; i < 4; i++) {
            if (this->a[i] != rhs.a[i])
                return false;
        }
        for (int i = 0; i < 6; i++) {
            if (this->d[i] != rhs.d[i])
                return false;
        }
        return true;
    }

    void dump() {
        Serial.print("Pin:\n");
        for (int i = 0; i < 4; i++) {
            Serial.printf("\tA%d = %d\n", i, a[i]);
        }
        for (int i = 0; i < 6; i++) {
            Serial.printf("\tD%d = %d\n", i, d[i]);
        }
    }
};

static PinState prevPinState{};
static PinState currPinState{};

static WebSocketsServer pinWebSocket(82);

// Get pin mode, stolen from https://github.com/arduino/ArduinoCore-API/issues/179
//
// On success returns: INPUT, INPUT_PULLUP, OUTPUT
// On invalid pins returns: -1
// static int pinMode(uint8_t pin) {
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

static void sendUpdate(PinState pinState) {
    JsonDocument doc;

    doc["A0"] = pinState.a[0];
    doc["A1"] = pinState.a[1];
    doc["A2"] = pinState.a[2];
    doc["A3"] = pinState.a[3];
    doc["D0"] = pinState.d[0];
    doc["D1"] = pinState.d[1];
    doc["D2"] = pinState.d[2];
    doc["D3"] = pinState.d[3];
    doc["D4"] = pinState.d[4];
    doc["D5"] = pinState.d[5];

    String output;
    serializeJson(doc, output);

    Serial.print("Sending Update");
    Serial.println(output);

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
}

void loopPinWebSocket() {
    static unsigned long prev = 0;
    auto now = millis();

    pinWebSocket.loop();

    if (now - prev > 1000) {
        prev = now;

        prevPinState = currPinState;
        currPinState.poll();
        if (!currPinState.equal(prevPinState)) {
            sendUpdate(currPinState);
        }

        currPinState.dump();
    }
}
