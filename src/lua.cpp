#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebSocketsServer.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <lua/lua.hpp>

enum Type {
    NONE,
    ARRAY,
    OBJECT,
};

const String testResponseString = R"---({"id":"chatcmpl-8d6d2cff624cebd2","object":"chat.completion","created":1788526612,"model":"/models/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-NVFP4","choices":[{"index":0,"message":{"role":"assistant","content":"Hello! How can I help you today?","refusal":null,"annotations":null,"audio":null,"function_call":null,"reasoning":"Here's a thinking process:\n\n1.  **Analyze User Input:** The user said \"Hello!\" which is a standard greeting.\n2.  **Identify Intent:** The user is initiating a conversation.\n3.  **Determine Response:** I should respond with a friendly greeting, acknowledge the user, and offer assistance. I'll keep it simple and polite.\n4.  **Formulate Response:** \"Hello! How can I help you today?\" or similar.\n5.  **Check Constraints:** No specific constraints mentioned. Just say hello back and offer help.\n6.  **Final Output Generation:** \"Hello! How can I help you today?\" (or very similar)✅"},"logprobs":null,"finish_reason":"stop","stop_reason":null,"token_ids":null,"routed_experts":null}],"service_tier":null,"system_fingerprint":"vllm-0.26.1rc1.dev1046+gba07e4a48-a9934369","usage":{"prompt_tokens":18,"total_tokens":172,"completion_tokens":154,"prompt_tokens_details":null,"completion_tokens_details":{"reasoning_tokens":143}},"prompt_logprobs":null,"prompt_token_ids":null,"prompt_text":null,"kv_transfer_params":null,"ec_transfer_params":null,"metrics":null}})---";

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
    Serial.printf("Lua error: %s\n", errmsg.c_str());

    JsonDocument doc;
    doc["type"] = "error";
    doc["data"] = errmsg;

    String output;
    serializeJson(doc, output);

    luaWebSocket.broadcastTXT(output);
}

static void sendData(String data) {
    Serial.printf("Lua data: %s\n", data.c_str());

    JsonDocument doc;
    doc["type"] = "data";
    doc["data"] = data;

    String output;
    serializeJson(doc, output);

    luaWebSocket.broadcastTXT(output);
}

// NOTE: assumes number indices in a lua table guarantees the order
static void luaTableToJson(JsonDocument &doc, lua_State *L, int tbl) {
    Type type = NONE;

    lua_pushnil(L);
    while (lua_next(L, tbl) != 0) {
        switch (lua_type(L, -2)) {
            case LUA_TNUMBER:
                {
                    // Technically, it could be a float index
                    // but ehh, stupid edge case
                    if (type == NONE) {
                        type = ARRAY;
                        doc.to<JsonArray>();
                    }
                    if (type != ARRAY) {
                        goto skip;
                    }
                    break;
                }
            case LUA_TSTRING:
                {
                    if (type == NONE) {
                        type = OBJECT;
                        doc.to<JsonObject>();
                    }
                    if (type != OBJECT) {
                        goto skip;
                    }
                    break;
                }
            case LUA_TNIL:
            case LUA_TBOOLEAN:
            case LUA_TTABLE:
            case LUA_TFUNCTION:
            case LUA_TUSERDATA:
            case LUA_TTHREAD:
            case LUA_TLIGHTUSERDATA:
            case LUA_TNONE:
                goto skip;
                break;
        }

#define ASSIGN(value) do { if (type == ARRAY) {doc.add((value));} else {doc[lua_tostring(L, -2)] = (value);} } while (0)
        switch (lua_type(L, -1)) {
            case LUA_TNUMBER:
                {
                    ASSIGN((double)lua_tonumber(L, -1));
                    break;
                }
            case LUA_TBOOLEAN:
                {
                    ASSIGN((bool)lua_toboolean(L, -1));
                    break;
                }
            case LUA_TSTRING:
                {
                    ASSIGN(lua_tostring(L, -1));
                    break;
                }
            case LUA_TTABLE:
                {
                    JsonDocument value;
                    luaTableToJson(value, L, lua_gettop(L));
                    ASSIGN(value);
                    break;
                }
            case LUA_TNIL:
                {
                    // think in lua, nil value means this key doesn't exist
                    break;
                }
            case LUA_TFUNCTION:
            case LUA_TUSERDATA:
            case LUA_TTHREAD:
            case LUA_TLIGHTUSERDATA:
            case LUA_TNONE:
                break; // ignore
        }
#undef ASSIGN

skip:
        lua_pop(L, 1);
    }
}

static void pushJsonVariant(lua_State *L, const JsonVariant &value);
static void pushJsonArray(lua_State *L, const JsonArray &arr);
static void pushJsonObject(lua_State *L, const JsonObject &obj);

static void pushJsonVariant(lua_State *L, const JsonVariant &value) {
    if (value.isNull()) {
        lua_pushnil(L);
    } else if (value.is<long long>()) { // must be before float
        lua_pushinteger(L, value.as<long long>());
    } else if (value.is<double>()) { // must be after int
        lua_pushnumber(L, value.as<double>());
    } else if (value.is<const char*>()) {
        lua_pushstring(L, value.as<const char*>());
    } else if (value.is<bool>()) {
        lua_pushboolean(L, value.as<bool>());
    } else if (value.is<JsonArray>()) {
        pushJsonArray(L, value.as<JsonArray>());
    } else if (value.is<JsonObject>()) {
        pushJsonObject(L, value.as<JsonObject>());
    } else {
        // unreachable
    }
}

static void pushJsonArray(lua_State *L, const JsonArray &arr) {
    lua_newtable(L);
    int tbl = lua_gettop(L);
    int key = 1;
    for (JsonVariant value : arr) {
        lua_pushinteger(L, key);
        pushJsonVariant(L, value);
        lua_settable(L, tbl);
        key++;
    }
}

static void pushJsonObject(lua_State *L, const JsonObject &obj) {
    lua_newtable(L);
    int tbl = lua_gettop(L);
    for (JsonPair kv : obj) {
        const auto key = kv.key();
        const auto value = kv.value();
        pushJsonVariant(L, value);
        lua_setfield(L, tbl, key.c_str());
    }
}

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

static int lua_agentic_send(lua_State *L) {
    const char *prompt = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    // Options
    JsonDocument opts;
    luaTableToJson(opts, L, 2);

    String optsString;
    serializeJson(opts, optsString);
    Serial.printf("Opts: %s\n", optsString.c_str());

    // Request json
    JsonDocument msg;
    msg["role"] = "user";
    msg["content"] = prompt;

    JsonDocument doc;
    doc["model"] = "/models/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-NVFP4";
    doc["messages"].add(msg);
    doc["stream"] = false;

    String request;
    serializeJson(doc, request);
    Serial.printf("Request: %s\n", request.c_str());

    // Response json
    JsonDocument response;
    deserializeJson(response, testResponseString);

    pushJsonVariant(L, response.as<JsonVariant>());

    return 1;
}

static void initLuaLib(lua_State *L) {
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

    lua_newtable(L);
    lua_pushcfunction(L, lua_agentic_send); lua_setfield(L, -2, "send");
    lua_setglobal(L, "agentic");
}

void stopLua() {
    Serial.print("Stopping lua\n");
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
    int err;

    stopLua();

    Serial.print("Initializing lua\n");
    runningLua = luaL_newstate();
    if (!runningLua) {
        sendError("Failed to initialize lua");
        return;
    }

    auto L = runningLua;

    initLuaLib(L);

    Serial.printf("Running lua:\n```\n%s\n```\n", code.c_str());
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
    } else {
        lua_pop(L, 1);
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
        } else {
            lua_pop(L, 1);
        }
    }
}

bool luaIsRunning() {
    return runningLua != nullptr;
}

// TODO: use framework-arduinopico's HTTPClient
// static int readResponse(HTTPClient &client) {
//     int statusCode = client.responseStatusCode();
//     Serial.printf("Status code: %d\n", statusCode);
//
//     Serial.print("Headers:\n");
//     while (client.headerAvailable()) {
//       String name = client.readHeaderName();
//       String value = client.readHeaderValue();
//       Serial.printf("\t%s: %s\n", name.c_str(), value.c_str());
//     }
//
//     auto contentLen = client.contentLength();
//     if (contentLen == HTTPClient::kNoContentLengthHeader) {
//       Serial.print("Response content length is unknown.\n");
//     } else {
//       Serial.printf("Response content length = %d\n", contentLen);
//     }
//
//     if (client.isResponseChunked()) {
//       Serial.print("Response is chunked.\n");
//     }
//
//     int emptyRes = 0;
//     while (true) {
//       String response = client.responseBody();
//       Serial.println("Response: " + response);
//
//       if (response == "") {
//         emptyRes += 1;
//       } else {
//         emptyRes = 0;
//       }
//
//       if (client.completed()) {
//         Serial.println("Completed!");
//         return 0;
//       }
//
//       if (emptyRes > 5) {
//         Serial.println("Server return empty response more than 5 times, stopping...");
//         return 0;
//       }
//     }
// }

// static void testHttpStuff() {
//     const auto endpoint = "/v1/chat/completions";
//     const auto contentType = "application/json";
//     const auto hello = R"---({"model": "/models/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-NVFP4", "messages": [{"role": "user", "content": "Hello!"}], "stream": false})---";
//     const auto meow = R"---({"model": "/models/NVIDIA-Nemotron-3.5-Lightning-30B-A3B-NVFP4", "messages": [{"role": "user", "content": "Meow!"}], "stream": false})---";
//     int err = 0;
//
//     Serial.print("--- READING 1ST REQUEST ---\n");
//     HTTPClient req1(net, "vaam01.3bbddns.com", 43954);
//     err = req1.post(endpoint, contentType, hello);
//     if (err) {
//       Serial.printf("Failed to send http post: %d\n", err);
//       while (true) {}
//     }
//
//     Serial.print("--- READING 2ND REQUEST ---\n");
//     HTTPClient req2(net, "vaam01.3bbddns.com", 43954);
//     err = req2.post(endpoint, contentType, meow);
//     if (err) {
//       Serial.printf("Failed to send http post: %d\n", err);
//       while (true) {}
//     }
//
//     Serial.print("--- READING 2ND RESPONSE ---\n");
//     readResponse(req2);
//
//     Serial.print("--- READING 1ST RESPONSE ---\n");
//     readResponse(req1);
//
//     Serial.print("Stopping...\n");
//     while (true) {}
// }

void doLuaStuff() {
    runLua("print(agentic.send('hello'))");
    while (true) {}
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
