#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsServer.h>
#include <WiFiClient.h>
#include <HttpClient.h>
#include <lua/lua.hpp>

// random json debugging utils
static void printTabs(int tab);
static void printValue(int tab, const JsonVariant &value);
static void printArray(int tab, const JsonArray &arr);
static void printObject(int tab, const JsonObject &obj);

static WiFiClient net;
static WebSocketsServer luaWebSocket(81);
static lua_State *runningLua = nullptr;

static void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {}

static void sendError(String errmsg) {
    JsonDocument doc;
    doc["type"] = "error";
    doc["data"] = errmsg;

    String output;
    serializeJson(doc, output);

    luaWebSocket.broadcastTXT(output);
}

static void sendData(String data) {
    JsonDocument doc;
    doc["type"] = "data";
    doc["data"] = data;

    String output;
    serializeJson(doc, output);

    luaWebSocket.broadcastTXT(output);
}

// TODO
static void checkGPIO(lua_State *L, int pin) {
    if (pin < 20 || 29 > pin) {
        lua_pushstring(L, "Only GPIO 20 - 29 can be accessed!");
        lua_error(L); // never returns
    }
}

static int lua_print(lua_State *L) {
    int n = lua_gettop(L);
    String out = "";
    for (int i = 1; i <= n; i++) {
        const char *s = lua_tostring(L, i);
        if (s) out += s;
        if (i < n) out += "\t";
    }
    out += "\n";
    sendData(out);
    return 0;
}

static int lua_digitalWrite(lua_State *L) {
    int pin = luaL_checkinteger(L, 1);
    checkGPIO(L, pin);

    int val = luaL_checkinteger(L, 2);
    digitalWrite(pin, val);

    return 0;
}

static int lua_analogWrite(lua_State *L) {
    int pin = luaL_checkinteger(L, 1);
    checkGPIO(L, pin);

    int val = luaL_checkinteger(L, 2);
    analogWrite(pin, val);

    return 0;
}

static int lua_digitalRead(lua_State *L) {
    int pin = luaL_checkinteger(L, 1);
    checkGPIO(L, pin);

    int val = digitalRead(pin);
    lua_pushinteger(L, val);

    return 1;
}

static int lua_analogRead(lua_State *L) {
    int pin = luaL_checkinteger(L, 1);
    checkGPIO(L, pin);

    int val = analogRead(pin);
    lua_pushinteger(L, val);

    return 1;
}

static int lua_pinMode(lua_State *L) {
    int pin = luaL_checkinteger(L, 1);
    checkGPIO(L, pin);

    int mode = luaL_checkinteger(L, 2);
    pinMode(pin, mode);

    return 0;
}

static int lua_delay(lua_State *L) {
    int ms = luaL_checkinteger(L, 1);
    delay(ms);
    return 0;
}

// pushes 1 table to the stack
// static void jsonDocToLuaTable(lua_State *L, const JsonDocument &doc) {
//     lua_newtable(L);
//     for (auto key : doc) { // pseudocode
//         switch (key.type) {
//             case ARRAY:
//                 {
//                     break;
//                 }
//             case JSON:
//                 {
//                     break;
//                 }
//             default:
//                 {
//                     break;
//                 }
//         }
//     }
// }

void stopLua() {
    if (runningLua) {
        lua_close(runningLua);
        runningLua = nullptr;

        for (int i = 20; i <= 29; i++) {
            digitalWrite(i, LOW);
            pinMode(i, INPUT);
        }
    }
}

void runLua(String code) {
    stopLua();

    runningLua = luaL_newstate();
    if (!runningLua) {
        Serial.print("Failed to initialize lua\n");
        sendError("Failed to initialize lua");
        return;
    }

    auto L = runningLua;
    int err;

    luaL_openlibs(L);

    lua_register(L, "print", lua_print);
    lua_register(L, "digitalWrite", lua_digitalWrite);
    lua_register(L, "analogWrite", lua_analogWrite);
    lua_register(L, "digitalRead", lua_digitalRead);
    lua_register(L, "analogRead", lua_analogRead);
    lua_register(L, "pinMode", lua_pinMode);
    lua_register(L, "delay", lua_delay);

    lua_pushinteger(L, HIGH); lua_setglobal(L, "HIGH");
    lua_pushinteger(L, LOW); lua_setglobal(L, "LOW");
    lua_pushinteger(L, LED_BUILTIN); lua_setglobal(L, "LED_BUILTIN");
    lua_pushinteger(L, INPUT); lua_setglobal(L, "INPUT");
    lua_pushinteger(L, OUTPUT); lua_setglobal(L, "OUTPUT");
    lua_pushinteger(L, INPUT_PULLUP); lua_setglobal(L, "INPUT_PULLUP");
    lua_pushinteger(L, INPUT_PULLDOWN); lua_setglobal(L, "INPUT_PULLDOWN");

    // agentic stuff
    createTable(0, 0);
    pushFunction(zlua.wrap(LuaLib.close)); setField(-2, "close");
    pushFunction(zlua.wrap(LuaLib.render)); setField(-2, "render");
    pushFunction(zlua.wrap(LuaLib.wait)); setField(-2, "wait");
    pushFunction(zlua.wrap(LuaLib.show)); setField(-2, "show");
    lua_setglobal(L, "agentic");

    err = luaL_dostring(L, code.c_str());
    if (err) {
        sendError(lua_tostring(L, -1));
        stopLua();
        return;
    }

    // Calls setup()
    // can still block the thread bruh
    if (lua_getglobal(L, "setup") == LUA_TFUNCTION) {
        err = lua_pcall(L, 0, 0, 0);
        if (err) {
            sendError(lua_tostring(L, -1));
            stopLua();
            return;
        }
    }
}

void initLua() {
    luaWebSocket.begin();
    luaWebSocket.onEvent(webSocketEvent);
}

void loopLua() {
    luaWebSocket.loop();
    if (runningLua) {
        auto L = runningLua;
        if (lua_getglobal(L, "loop") == LUA_TFUNCTION) {
            int err = lua_pcall(L, 0, 0, 0);
            if (err) {
                String errmsg = lua_tostring(L, -1);
                sendError(errmsg);
                stopLua();
                return;
            }
        }
    }
}

bool luaIsRunning() {
    return runningLua != nullptr;
}

void doLuaStuff() {
    lua_State *L = luaL_newstate();
    if (!L) {
        Serial.print("Failed to initialize lua\n");
        return;
    }

    String testJson = R"---({"id":"chatcmpl-8d6d2cff624cebd2","object":"chat.completion","created":1788526612,"model":"/models/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-NVFP4","choices":[{"index":0,"message":{"role":"assistant","content":"Hello! How can I help you today?","refusal":null,"annotations":null,"audio":null,"function_call":null,"reasoning":"Here's a thinking process:\n\n1.  **Analyze User Input:** The user said \"Hello!\" which is a standard greeting.\n2.  **Identify Intent:** The user is initiating a conversation.\n3.  **Determine Response:** I should respond with a friendly greeting, acknowledge the user, and offer assistance. I'll keep it simple and polite.\n4.  **Formulate Response:** \"Hello! How can I help you today?\" or similar.\n5.  **Check Constraints:** No specific constraints mentioned. Just say hello back and offer help.\n6.  **Final Output Generation:** \"Hello! How can I help you today?\" (or very similar)✅"},"logprobs":null,"finish_reason":"stop","stop_reason":null,"token_ids":null,"routed_experts":null}],"service_tier":null,"system_fingerprint":"vllm-0.26.1rc1.dev1046+gba07e4a48-a9934369","usage":{"prompt_tokens":18,"total_tokens":172,"completion_tokens":154,"prompt_tokens_details":null,"completion_tokens_details":{"reasoning_tokens":143}},"prompt_logprobs":null,"prompt_token_ids":null,"prompt_text":null,"kv_transfer_params":null,"ec_transfer_params":null,"metrics":null}})---";

    JsonDocument doc;
    deserializeJson(doc, testJson);

    Serial.print("----\n");
    printValue(0, doc.as<JsonVariant>());
    Serial.print("----\n");
}

static void printTabs(int tab) {
    for (int i = 0; i < tab; i++)
        Serial.print("\t");
}

static void printValue(int tab, const JsonVariant &value) {
    if (value.isNull()) {
        Serial.print("null");
    } else if (value.is<int>()) { // must be before float
        Serial.print(value.as<int>());
    } else if (value.is<float>()) { // must be after int
        Serial.print(value.as<float>());
    } else if (value.is<const char*>()) {
        Serial.printf("\"%s\"", value.as<const char*>());
    } else if (value.is<bool>()) {
        Serial.print(value.as<bool>());
    } else if (value.is<JsonArray>()) {
        Serial.print("[\n");
        printArray(tab + 1, value.as<JsonArray>());
        printTabs(tab);
        Serial.print("]");
    } else if (value.is<JsonObject>()) {
        Serial.print("{\n");
        printObject(tab + 1, value.as<JsonObject>());
        printTabs(tab);
        Serial.print("}");
    }
    Serial.print("\n");
}

static void printArray(int tab, const JsonArray &arr) {
    for (JsonVariant value : arr) {
        printTabs(tab);
        printValue(tab, value);
    }
}

static void printObject(int tab, const JsonObject &obj) {
    for (JsonPair kv : obj) {
        const auto key = kv.key();
        const auto value = kv.value();
        printTabs(tab);
        Serial.printf("%s: ", key);
        printValue(tab, value);
    }
}
