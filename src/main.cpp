#include <Arduino.h>
#include <FreeRTOS.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <NCMEthernetlwIP.h>
#include <lua/lua.hpp>
#include <lwip/netif.h>
#include <dhcpserver/dhcpserver.h>
#include <WebSocketsServer.h>
#include <GFXMatrix.h>
#include "phy.hpp"
#include "ip.hpp"

// --- HUB75 Configuration ---
struct Hub75Pins {
    int* rgb = nullptr;
    int* addr = nullptr;
    int clk = -1;
    int lat = -1;
    int oe = -1;
    int rgb_count = 0;
    int addr_count = 0;
} hub75_pins;

GFXMatrix* matrix = nullptr;

// --- Configuration ---
WebServer server(80);
WebSocketsServer webSocket(81);
dhcp_server_t my_dhcp_server;

// --- State Management ---
String current_file = "main.lua";
String lua_code_pending = "";
bool run_requested = false;
bool stop_requested = false;
bool is_running = false;
unsigned long last_execution_time = 0;
unsigned long last_yield_time = 0;
String web_serial_buffer = "";
int used_pins[40];
int used_pins_count = 0;

// --- Helpers ---
String jsonEscape(String s) {
    String res = "";
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '\"') res += "\\\"";
        else if (c == '\\') res += "\\\\";
        else if (c == '\n') res += "\\n";
        else if (c == '\r') res += "\\r";
        else if (c == '\t') res += "\\t";
        else if (c < 32) {} // Ignore other control chars
        else res += c;
    }
    return res;
}

void log_to_web(String msg) {
    web_serial_buffer += msg;
    if (web_serial_buffer.length() > 2000) {
        web_serial_buffer = web_serial_buffer.substring(web_serial_buffer.length() - 2000);
    }
}

extern "C" void log_to_web_c(const char* msg) {
    log_to_web(String(msg));
    String str = String(msg);
    webSocket.broadcastTXT(str);
    Serial.print(msg);
}

// extern "C" void hub75_stop(){
//         if (matrix) {
//         matrix->fillScreen(0);
//         matrix->display();
//         delete matrix;
//         matrix = nullptr;
//     }
// }

// --- WebSocket Event Handler ---
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
    (void)num; (void)type; (void)payload; (void)length;
}

// --- Lua Bindings ---
int lua_print(lua_State *L) {
    int n = lua_gettop(L);
    String out = "";
    for (int i = 1; i <= n; i++) {
        const char *s = lua_tostring(L, i);
        if (s) out += s;
        if (i < n) out += "\t";
    }
    Serial.println(out);
    log_to_web(out + "\n");
    webSocket.broadcastTXT(out + "\n");
    return 0;
}

int lua_digitalWrite(lua_State *L) {
    int pin = luaL_checkinteger(L, 1);
    int val = luaL_checkinteger(L, 2);
    pinMode(pin, OUTPUT);
    digitalWrite(pin, val);

    // Track used pins for cleanup
    bool already_tracked = false;
    for (int i = 0; i < used_pins_count; i++) {
        if (used_pins[i] == pin) {
            already_tracked = true;
            break;
        }
    }
    if (!already_tracked && used_pins_count < 40) {
        used_pins[used_pins_count++] = pin;
    }

    return 0;
}

int lua_analogWrite(lua_State *L) {
    int pin = luaL_checkinteger(L, 1);
    int val = luaL_checkinteger(L, 2);
    pinMode(pin, OUTPUT);
    analogWrite(pin, val);

    // Track used pins for cleanup
    bool already_tracked = false;
    for (int i = 0; i < used_pins_count; i++) {
        if (used_pins[i] == pin) {
            already_tracked = true;
            break;
        }
    }
    if (!already_tracked && used_pins_count < 40) {
        used_pins[used_pins_count++] = pin;
    }

    return 0;
}

int lua_digitalRead(lua_State *L) {
    int pin = luaL_checkinteger(L, 1);
    pinMode(pin, INPUT);
    int val = digitalRead(pin);

    // Track used pins for cleanup
    bool already_tracked = false;
    for (int i = 0; i < used_pins_count; i++) {
        if (used_pins[i] == pin) {
            already_tracked = true;
            break;
        }
    }
    if (!already_tracked && used_pins_count < 40) {
        used_pins[used_pins_count++] = pin;
    }

    lua_pushinteger(L, val);
    return 1;
}

int lua_analogRead(lua_State *L) {
    int pin = luaL_checkinteger(L, 1);
    pinMode(pin, INPUT);
    int val = analogRead(pin);

    // Track used pins for cleanup
    bool already_tracked = false;
    for (int i = 0; i < used_pins_count; i++) {
        if (used_pins[i] == pin) {
            already_tracked = true;
            break;
        }
    }
    if (!already_tracked && used_pins_count < 40) {
        used_pins[used_pins_count++] = pin;
    }

    lua_pushinteger(L, val);
    return 1;
}

int lua_pinMode(lua_State *L) {
    int pin = luaL_checkinteger(L, 1);
    int mode = luaL_checkinteger(L, 2);
    pinMode(pin, mode);

    // Track used pins for cleanup
    bool already_tracked = false;
    for (int i = 0; i < used_pins_count; i++) {
        if (used_pins[i] == pin) {
            already_tracked = true;
            break;
        }
    }
    if (!already_tracked && used_pins_count < 40) {
        used_pins[used_pins_count++] = pin;
    }

    return 0;
}

int lua_delay(lua_State *L) {
    int ms = luaL_checkinteger(L, 1);
    unsigned long start = millis();
    while (millis() - start < (unsigned long)ms) {
        if (stop_requested) break;
        server.handleClient();
        webSocket.loop();
        delay(1);
    }
    return 0;
}

void lua_hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    if (stop_requested) luaL_error(L, "Stopped by user");
    unsigned long now = millis();
    if (now - last_yield_time >= 10) {
        last_yield_time = now;
        server.handleClient();
        webSocket.loop();
        yield();
    }
}

// --- HUB75 Lua Bindings ---

extern "C" volatile uint32_t dma_irq_count;

int lua_hub75_getIrqCount(lua_State *L) {
    lua_pushinteger(L, dma_irq_count);
    return 1;
}

int lua_hub75_setPins(lua_State *L) {
    if (!lua_istable(L, 1) || !lua_istable(L, 2)) {
        return luaL_error(L, "rgbPins and addrPins must be tables");
    }
    (void)luaL_checkinteger(L, 3);
    (void)luaL_checkinteger(L, 4);
    (void)luaL_checkinteger(L, 5);

    String msg = "Warning: RP2040Matrix uses strict PIO/DMA hardware pin mapping. Pins passed to setPins() are ignored.\n";
    Serial.print(msg);
    log_to_web(msg);
    webSocket.broadcastTXT(msg);

    return 0;
}

int lua_hub75_begin(lua_State *L) {
    int width = luaL_checkinteger(L, 1);
    int height = luaL_checkinteger(L, 2);

    if (matrix) {
        matrix->clear();
        matrix->display();
        delete matrix;      // ~GFXMatrix() จะเรียก hub75_stop() จริงใน hub75.cpp
        matrix = nullptr;
    }

    matrix = new GFXMatrix(width, height);
    matrix->begin();

    return 0;
}



int lua_hub75_show(lua_State *L) {
    (void)L;
    if (matrix) {
        matrix->display();
    }
    return 0;
}

uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

int lua_hub75_fillScreen(lua_State *L) {
    int r = luaL_checkinteger(L, 1);
    int g = luaL_checkinteger(L, 2);
    int b = luaL_checkinteger(L, 3);
    if (matrix) {
        matrix->fillScreen(color565(r, g, b));
    }
    return 0;
}

int lua_hub75_drawPixel(lua_State *L) {
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int r = luaL_checkinteger(L, 3);
    int g = luaL_checkinteger(L, 4);
    int b = luaL_checkinteger(L, 5);
    if (matrix) {
        matrix->drawPixel(x, y, color565(r, g, b));
    }
    return 0;
}

int lua_hub75_drawLine(lua_State *L) {
    int x0 = luaL_checkinteger(L, 1);
    int y0 = luaL_checkinteger(L, 2);
    int x1 = luaL_checkinteger(L, 3);
    int y1 = luaL_checkinteger(L, 4);
    int r = luaL_checkinteger(L, 5);
    int g = luaL_checkinteger(L, 6);
    int b = luaL_checkinteger(L, 7);
    if (matrix) {
        matrix->drawLine(x0, y0, x1, y1, color565(r, g, b));
    }
    return 0;
}

int lua_hub75_drawRect(lua_State *L) {
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int w = luaL_checkinteger(L, 3);
    int h = luaL_checkinteger(L, 4);
    int r = luaL_checkinteger(L, 5);
    int g = luaL_checkinteger(L, 6);
    int b = luaL_checkinteger(L, 7);
    if (matrix) {
        matrix->drawRect(x, y, w, h, color565(r, g, b));
    }
    return 0;
}

int lua_hub75_fillRect(lua_State *L) {
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int w = luaL_checkinteger(L, 3);
    int h = luaL_checkinteger(L, 4);
    int r = luaL_checkinteger(L, 5);
    int g = luaL_checkinteger(L, 6);
    int b = luaL_checkinteger(L, 7);
    if (matrix) {
        matrix->fillRect(x, y, w, h, color565(r, g, b));
    }
    return 0;
}

int lua_hub75_drawCircle(lua_State *L) {
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int radius = luaL_checkinteger(L, 3);
    int r = luaL_checkinteger(L, 4);
    int g = luaL_checkinteger(L, 5);
    int b = luaL_checkinteger(L, 6);
    if (matrix) {
        matrix->drawCircle(x, y, radius, color565(r, g, b));
    }
    return 0;
}

int lua_hub75_fillCircle(lua_State *L) {
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    int radius = luaL_checkinteger(L, 3);
    int r = luaL_checkinteger(L, 4);
    int g = luaL_checkinteger(L, 5);
    int b = luaL_checkinteger(L, 6);
    if (matrix) {
        matrix->fillCircle(x, y, radius, color565(r, g, b));
    }
    return 0;
}

int lua_hub75_drawString(lua_State *L) {
    const char* text = luaL_checkstring(L, 1);
    int x = luaL_checkinteger(L, 2);
    int y = luaL_checkinteger(L, 3);
    int r = luaL_checkinteger(L, 4);
    int g = luaL_checkinteger(L, 5);
    int b = luaL_checkinteger(L, 6);
    if (matrix) {
        matrix->setTextColor(color565(r, g, b));
        matrix->setCursor(x, y);
        matrix->print(text);
    }
    return 0;
}

void run_lua(String code) {
    Serial.println("[run_lua] START");
    is_running = true;
    stop_requested = false;
    lua_State *L = luaL_newstate();
    if (!L) {
        Serial.println("[run_lua] Lua State FAIL");
        log_to_web("Error: Lua State Fail\n");
        is_running = false;
        return;
    }
    Serial.println("[run_lua] Lua State OK");
    luaL_openlibs(L);
    Serial.println("[run_lua] libs opened");
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
    Serial.println("[run_lua] globals registered");

    // Register hub75 module
    lua_newtable(L);
    Serial.println("[run_lua] hub75 table created");
    lua_pushcfunction(L, lua_hub75_setPins); lua_setfield(L, -2, "setPins");
    lua_pushcfunction(L, lua_hub75_begin); lua_setfield(L, -2, "begin");
    lua_pushcfunction(L, lua_hub75_getIrqCount); lua_setfield(L, -2, "getIrqCount");
    lua_pushcfunction(L, lua_hub75_show); lua_setfield(L, -2, "show");
    lua_pushcfunction(L, lua_hub75_fillScreen); lua_setfield(L, -2, "fillScreen");
    lua_pushcfunction(L, lua_hub75_drawPixel); lua_setfield(L, -2, "drawPixel");
    lua_pushcfunction(L, lua_hub75_drawLine); lua_setfield(L, -2, "drawLine");
    lua_pushcfunction(L, lua_hub75_drawRect); lua_setfield(L, -2, "drawRect");
    lua_pushcfunction(L, lua_hub75_fillRect); lua_setfield(L, -2, "fillRect");
    lua_pushcfunction(L, lua_hub75_drawCircle); lua_setfield(L, -2, "drawCircle");
    lua_pushcfunction(L, lua_hub75_fillCircle); lua_setfield(L, -2, "fillCircle");
    lua_pushcfunction(L, lua_hub75_drawString); lua_setfield(L, -2, "drawString");
    lua_setglobal(L, "hub75");
    Serial.println("[run_lua] hub75 registered");
    lua_sethook(L, lua_hook, LUA_MASKCOUNT, 100);
    Serial.println("[run_lua] calling dostring");
    log_to_web("--- [" + current_file + "] Start ---\n");
    unsigned long start_time = millis();
    if (luaL_dostring(L, code.c_str())) {
        log_to_web("Lua Error: " + String(lua_tostring(L, -1)) + "\n");
    }
    Serial.println("[run_lua] dostring done");
    last_execution_time = millis() - start_time;
    log_to_web("--- [" + current_file + "] End ---\n");

    // Cleanup: Reset used pins
    for (int i = 0; i < used_pins_count; i++) {
        digitalWrite(used_pins[i], LOW);
        pinMode(used_pins[i], INPUT);
    }
    used_pins_count = 0;

    // Cleanup HUB75 matrix (Only if explicitly stopped by user)
    if (stop_requested && matrix) {
        matrix->clear();
        matrix->display();
        delete matrix;
        matrix = nullptr;
    }

    lua_close(L);
    is_running = false;
    stop_requested = false;
}

// --- Web Handlers ---
void handleRoot() {
    String s;

    Serial.printf("free_heap: %d\n", rp2040.getFreeHeap());

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    loopPhy(millis());

    s = "";
    s += R"===(<!doctype html><title>Pico Lua Playground</title><meta name=viewport content="width=device-width,initial-scale=1"><div class=sidebar><div class=sidebar-header><span>EXPLORER</span><button onclick=newFile() style="background:#fff0f6;border:2px solid #fbcfe8;color:#db2777;width:30px;height:30px;border-radius:50%;cursor:pointer;font-weight:700">)===";
    server.send(200, "text/html", s);
    loopPhy(millis());

    Serial.printf("free_heap: %d\n", rp2040.getFreeHeap());

    s = ""; // Do you need this?
    s += R"===(+</button></div><div id=fileList class=file-list></div></div><div class=main id=mainContainer><div class=header><div id=fileName style=font-weight:700;color:#7c3aed;font-size:16px>main.lua</div><div style=display:flex;align-items:center><span id=ramText style=font-size:11px;margin-right:15px;color:#6b7280;font-family:monospace>RAM: -- KB</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    Serial.printf("free_heap: %d\n", rp2040.getFreeHeap());

    s = "";
    s += R"===(<span id=statusText style=font-size:12px;margin-right:12px;font-weight:700;color:#9333ea>Idle</span><div id=status-light></div><button class="btn btn-docs" onclick=openDocsModal()>)===";
    server.sendContent(s);
    loopPhy(millis());

    Serial.printf("free_heap: %d\n", rp2040.getFreeHeap());

    s = "";
    s += R"===(API Docs)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(</button>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<button class="btn btn-save" onclick=saveCode()>Save</button>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<button class="btn btn-run" id=runBtn onclick=runCode()>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(Run)===";
    server.sendContent(s);
    loopPhy(millis());

    loopPhy(millis());
    s = "";
    s += R"===(</button>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<button class="btn btn-stop" id=stopBtn onclick=stopCode() disabled>)===";
    server.sendContent(s);
    loopPhy(millis());

    Serial.printf("--- Web Terminal --- : %d\n", rp2040.getFreeHeap());

    s = "";
    s += R"===(Stop</button></div></div><div class=editor-container><div class=line-numbers id=lineNumbers>1</div><textarea id=editor spellcheck=false placeholder="-- Write your Lua code here..." onscroll=syncScroll() oninput=updateLineNumbers()></textarea></div><div class=terminal id=terminal>--- Web Terminal ---</div></div><div id=newFileModal class=modal-overlay><div class=modal><div class=modal-title>Create New File</div><input id=newFileNameInput class=modal-input placeholder="e.g. blink.lua"><div class=modal-actions><button class="btn btn-cancel" onclick=closeModal()>Cancel</button><button class="btn btn-save" onclick=submitNewFile()>)===";
    server.sendContent(s);
    loopPhy(millis());

    Serial.printf("--- Delete file --- : %d\n", rp2040.getFreeHeap());

    s = "";
    s += R"===(Create</button></div></div></div><div id=deleteFileModal class=modal-overlay><div class=modal><div class=modal-title>Delete File</div><p id=deleteMessage style=font-size:14px;margin-bottom:20px;color:#6b7280;font-weight:600><div class=modal-actions><button class="btn btn-cancel" onclick=closeDeleteModal()>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(Cancel</button><button class="btn btn-stop" onclick=submitDeleteFile()>)===";
    server.sendContent(s);
    loopPhy(millis());

    Serial.printf("--- Lua API Documentation --- : %d\n", rp2040.getFreeHeap());

    s = "";
    s += R"===(Delete</button></div></div></div><div id=docsModal class=modal-overlay><div class="modal modal-large"><div class=modal-title style=display:flex;justify-content:space-between;align-items:center;margin-bottom:10px><span>Lua API Documentation</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    Serial.printf("--- Outputs values .... --- : %d\n", rp2040.getFreeHeap());
    loopPhy(millis());

    s = "";
    s += R"===(<span onclick=closeDocsModal() style=cursor:pointer;font-size:24px;color:#db2777;font-weight:700>&#215;</span></div><div class=modal-body><div class=api-section><div class=api-section-title>Global Functions & Constants</div><div class=api-item><div class=api-signature>print(...)</div><div class=api-desc>Outputs values to the Web Terminal for debugging. Accepts)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(multiple arguments of any type.</div><div class=api-params><span class=api-param-item><span class=api-param-name>...</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>any</span> - Values to)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(print</span></div></div><div class=api-item><div class=api-signature>digitalWrite(pin, state)</div><div class=api-desc>Sets the state of a GPIO pin to HIGH or LOW. Useful for)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(controlling onboard or external LEDs.</div><div class=api-params><span class=api-param-item><span class=api-param-name>pin</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - GPIO pin number)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===((e.g. 0-29)</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>state</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - Pin state (HIGH)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(or LOW)</span></div></div><div class=api-item><div class=api-signature>analogWrite(pin, val)</div><div class=api-desc>Sends a PWM signal (0-255) to a GPIO pin. Useful for fading)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(LEDs.</div><div class=api-params><span class=api-param-item><span class=api-param-name>pin</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - GPIO pin number)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===((e.g. 0-29)</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>val</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - PWM value (0 to)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(255)</span></div></div><div class=api-item><div class=api-signature>digitalRead(pin)</div><div class=api-desc>Reads the digital state of a GPIO pin (returns HIGH/1 or LOW/0).</div><div class=api-params><span class=api-param-item><span class=api-param-name>pin</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - GPIO pin)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(number</span></div></div><div class=api-item><div class=api-signature>analogRead(pin)</div><div class=api-desc>Reads the analog voltage value on an ADC pin (returns 0-1023).</div><div class=api-params><span class=api-param-item><span class=api-param-name>pin</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - ADC pin number)===";
    server.sendContent(s);
    loopPhy(millis());

    loopPhy(millis());
    s = "";
    s += R"===((e.g. 26-28)</span></div></div><div class=api-item><div class=api-signature>pinMode(pin, mode)</div><div class=api-desc>Configures the input/output mode of a GPIO pin.</div><div class=api-params><span class=api-param-item><span class=api-param-name>pin</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - GPIO pin)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(number</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>mode</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - Mode (INPUT,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(OUTPUT, INPUT_PULLUP, INPUT_PULLDOWN)</span></div></div><div class=api-item><div class=api-signature>delay(ms)</div><div class=api-desc>Pauses the execution of the script for a specified duration of)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(milliseconds.</div><div class=api-params><span class=api-param-item><span class=api-param-name>ms</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - Delay duration in)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(milliseconds</span></div></div><div class=api-item><div class=api-signature>Constants</div><div class=api-desc>Pre-defined global variables:</div><div class=api-params style=border:none;padding:0;margin:0><span class=api-param-item><span class=api-param-name>HIGH</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>1</span></span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>LOW</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>0</span></span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>LED_BUILTIN</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>25</span></span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>INPUT</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>0</span></span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>OUTPUT</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>1</span></span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>INPUT_PULLUP</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    loopPhy(millis());
    s += R"===(<span class=api-param-type>2</span></span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>INPUT_PULLDOWN</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>3</span></span></div></div></div><div class=api-section><div class=api-section-title style=display:flex;align-items:center>hub75 Module (HUB75 RGB LED Matrix 64x64))===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=badge-incomplete>INCOMPLETE</span></div><div class=api-item><div class=api-signature>hub75.setPins(rgbTable, addrTable, clk, lat, oe)</div><div class=api-desc>Configures the pins dynamically. Must be called first before)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(calling begin().</div><div class=api-params><span class=api-param-item><span class=api-param-name>rgbTable</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>table</span> - Array of 6 numbers)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(for {R1, G1, B1, R2, G2, B2}</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>addrTable</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>table</span> - Array of 5 numbers)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(for {A, B, C, D, E}</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>clk</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - Clock pin</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>lat</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - Latch pin</span>)===";
    server.sendContent(s);
    loopPhy(millis());
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>oe</span>)===";
    server.sendContent(s);
    loopPhy(millis());
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - Output enable)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(pin</span></div></div><div class=api-item><div class=api-signature>hub75.begin(width, height)</div><div class=api-desc>Initializes the matrix driver. Creates the frame buffer and)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(starts the PIO driving signals.</div><div class=api-params><span class=api-param-item><span class=api-param-name>width</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - Matrix width)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===((e.g. 64)</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>height</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - Matrix height)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===((e.g. 64)</span></div></div><div class=api-item><div class=api-signature>hub75.show()</div><div class=api-desc>Pushes the back buffer to the LED matrix panel to display the)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(changes. Call this to refresh the screen.</div></div><div class=api-item><div class=api-signature>hub75.fillScreen(r, g, b)</div><div class=api-desc>Fills the frame buffer with a single solid RGB color.</div><div class=api-params><span class=api-param-item><span class=api-param-name>r</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>g</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>b</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - RGB components)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===((0-255)</span></div></div><div class=api-item><div class=api-signature>hub75.drawPixel(x, y, r, g, b)</div><div class=api-desc>Draws a pixel at the given coordinate with specified color.</div><div class=api-params><span class=api-param-item><span class=api-param-name>x</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>y</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - Coordinates (0 to)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(63)</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>r</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>g</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>b</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    loopPhy(millis());
    s += R"===(<span class=api-param-type>number</span> - RGB components)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===((0-255)</span></div></div><div class=api-item><div class=api-signature>hub75.drawLine(x0, y0, x1, y1, r, g, b)</div><div class=api-desc>Draws a straight line between two points.</div><div class=api-params><span class=api-param-item><span class=api-param-name>x0</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>y0</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - Start point)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(coordinates</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>x1</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>y1</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - End point)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(coordinates</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>r</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>g</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>b</span>)===";
    server.sendContent(s);
    loopPhy(millis());
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - RGB components)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===((0-255)</span></div></div><div class=api-item><div class=api-signature>hub75.drawRect(x, y, w, h, r, g, b)</div><div class=api-desc>Draws an unfilled rectangle outline.</div><div class=api-params><span class=api-param-item><span class=api-param-name>x</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>y</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - Top-left)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(coordinates</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>w</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>h</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - Width and)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(height</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>r</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>g</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>b</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - RGB components)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===((0-255)</span></div></div><div class=api-item><div class=api-signature>hub75.fillRect(x, y, w, h, r, g, b)</div><div class=api-desc>Draws a filled rectangle.</div><div class=api-params><span class=api-param-item><span class=api-param-name>x</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>y</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - Top-left)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(coordinates</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>w</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>h</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - Width and)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(height</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>r</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>g</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>b</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - RGB components)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===((0-255)</span></div></div><div class=api-item><div class=api-signature>hub75.drawCircle(x, y, radius, r, g, b)</div><div class=api-desc>Draws a circle outline with specified radius.</div><div class=api-params><span class=api-param-item><span class=api-param-name>x</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>y</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - Center)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(coordinates</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>radius</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - Circle)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(radius</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>r</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>g</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>b</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - RGB components)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    loopPhy(millis());
    s += R"===((0-255)</span></div></div><div class=api-item><div class=api-signature>hub75.fillCircle(x, y, radius, r, g, b)</div><div class=api-desc>Draws a filled circle with specified radius.</div><div class=api-params><span class=api-param-item><span class=api-param-name>x</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>y</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - Center)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(coordinates</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>radius</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - Circle)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(radius</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>r</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>g</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>b</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - RGB components)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===((0-255)</span></div></div><div class=api-item><div class=api-signature>hub75.drawString(text, x, y, r, g, b)</div><div class=api-desc>Draws text starting at the given coordinates.</div><div class=api-params><span class=api-param-item><span class=api-param-name>text</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>string</span> - String message to)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(draw</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>x</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>y</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - Text starting)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(baseline coordinates</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-item><span class=api-param-name>r</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>g</span>,)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-name>b</span>)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===(<span class=api-param-type>number</span> - RGB components)===";
    server.sendContent(s);
    loopPhy(millis());

    s = "";
    s += R"===((0-255)</span></div></div></div></div></div></div><script>let currentPath="/main.lua",fileToDelete="",isWsConnected=!1,ws;const editor=document.getElementById("editor"),lineNumbers=document.getElementById("lineNumbers");function updateLineNumbers(){const t=editor.value.split(`)===";
    server.sendContent(s);
    loopPhy(millis());

    Serial.printf("BEEG JAVASCRIPT: %d\n", rp2040.getFreeHeap());

    s = "";
    loopPhy(millis());
    s += R"===(`),n=t.length;let e="";for(let t=1;t<=n;t++)e+=t+"<br>";lineNumbers.innerHTML=e}function syncScroll(){lineNumbers.scrollTop=editor.scrollTop}function connectWS(){ws=new WebSocket("ws://"+window.location.hostname+":81"),ws.onopen=()=>{isWsConnected=!0,console.log("WS Connected")},ws.onclose=()=>{isWsConnected=!1,console.log("WS Disconnected. Reconnecting..."),setTimeout(connectWS,2e3)},ws.onmessage=e=>{const t=document.getElementById("terminal");t.innerText+=e.data,t.scrollTop=t.scrollHeight},ws.onerror=e=>{ws.close()}}function listFiles(){fetch("/list_files").then(e=>e.json()).then(e=>{const t=document.getElementById("fileList");t.innerHTML="",e.forEach(e=>{const n=document.createElement("div");n.className="file-item"+("/"+e.name===currentPath?" active":""),n.innerHTML=`<span>${e.name}</span><span class="delete-btn" onclick="deleteFile('${e.name}', event)">&times;</span>`,n.onclick=()=>loadFile("/"+e.name),t.appendChild(n)})}).catch(()=>{})}function loadFile(e){currentPath=e,document.getElementById("fileName").innerText=e.substring(1),fetch("/read?path="+e).then(e=>e.text()).then(e=>{editor.value=e,updateLineNumbers(),listFiles()}).catch(()=>{})}function saveCode(){fetch("/upload?path="+currentPath,{method:"POST",body:editor.value}).then(()=>listFiles()).catch(()=>{})}function runCode(){fetch("/run",{method:"POST",body:editor.value}).catch(()=>{})}function stopCode(){fetch("/stop",{method:"POST"}).catch(()=>{})}function newFile(){document.getElementById("newFileModal").classList.add("show"),document.getElementById("newFileNameInput").focus()}function closeModal(){document.getElementById("newFileModal").classList.remove("show")}function submitNewFile(){const e=document.getElementById("newFileNameInput").value.trim();if(e){const t=e.endsWith(".lua")?e:e+".lua",n=t.startsWith("/")?t:"/"+t;fetch("/create?path="+n,{method:"POST"}).then(()=>{loadFile(n),closeModal()})}}function deleteFile(e,t){if(t.stopPropagation(),e==="main.lua")return;fileToDelete=e,document.getElementById("deleteMessage").innerText=`Delete ${e}?`,document.getElementById("deleteFileModal").classList.add("show")}function closeDeleteModal(){document.getElementById("deleteFileModal").classList.remove("show")}function submitDeleteFile(){fetch("/delete?path=/"+fileToDelete,{method:"POST"}).then(()=>{currentPath==="/"+fileToDelete?loadFile("/main.lua"):listFiles(),closeDeleteModal()})}function openDocsModal(){document.getElementById("docsModal").classList.add("show")}function closeDocsModal(){document.getElementById("docsModal").classList.remove("show")}setInterval(()=>{fetch("/poll").then(e=>e.json()).then(e=>{const n=document.getElementById("mainContainer"),s=document.getElementById("statusText"),a=document.getElementById("ramText"),o=document.getElementById("runBtn"),i=document.getElementById("stopBtn"),t=document.getElementById("terminal");e.free_heap&&(a.innerText="RAM: "+Math.round(e.free_heap/1024)+" KB"),e.running?(n.classList.add("running"),s.innerText="Running...",o.disabled=!0,i.disabled=!1):(n.classList.remove("running"),s.innerText="Idle",o.disabled=!1,i.disabled=!0),e.logs&&!isWsConnected&&(t.innerText+=e.logs,t.scrollTop=t.scrollHeight)}).catch(()=>{})},2e3),window.onload=()=>{loadFile("/main.lua"),connectWS()}</script>)===";
    server.sendContent(s);
    loopPhy(millis());

    server.sendContent("");
    loopPhy(millis());
}

void handleList() {
    String json = "["; Dir root = LittleFS.openDir("/"); bool first = true;
    while (root.next()) { if (!first) json += ","; json += "{\"name\":\"" + root.fileName() + "\"}"; first = false; }
    json += "]"; server.send(200, "application/json", json);
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
    if (f) { f.print(server.arg("plain")); f.close(); server.send(200, "text/plain", "Saved"); }
    else server.send(500, "text/plain", "Error");
}
void handleCreate() { String path = server.arg("path"); File f = LittleFS.open(path, "w"); if (f) { f.close(); server.send(200); } else server.send(500); }
void handleDelete() { String path = server.arg("path"); if (LittleFS.remove(path)) server.send(200); else server.send(500); }

void handleRun() {
    lua_code_pending = server.arg("plain");
    run_requested = true;
    Serial.println("[handleRun] called, code length=" + String(lua_code_pending.length()));
    server.send(200, "text/plain", "OK");
}
void handleStop() { stop_requested = true; server.send(200, "text/plain", "Stop Issued"); }

void handlePoll() {
    String json;
    json.reserve(512);
    json = "{";
    json += "\"running\":" + String(is_running ? "true" : "false") + ",";
    json += "\"free_heap\":" + String(rp2040.getFreeHeap()) + ",";
    json += "\"exec_time\":" + String(last_execution_time) + ",";
    json += "\"logs\":\"" + jsonEscape(web_serial_buffer) + "\"";
    json += "}";
    server.send(200, "application/json", json);
    web_serial_buffer = "";
}

void loopPhyNow() {
    loopPhy(millis());
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    // LittleFS.begin();

    initPhy();

    // Initialize DHCP Server
    // ip_addr_t lwip_ip, lwip_mask;
    // IP_ADDR4(&lwip_ip, PHY_IP_ADDR[0], PHY_IP_ADDR[1], PHY_IP_ADDR[2], PHY_IP_ADDR[3]);
    // IP_ADDR4(&lwip_mask, PHY_NETWORK_MASK[0], PHY_NETWORK_MASK[1], PHY_NETWORK_MASK[2], PHY_NETWORK_MASK[3]);
    // dhcp_server_init(&my_dhcp_server, &lwip_ip, &lwip_mask, netif_default);

    server.on("/", handleRoot);
    // server.on("/list_files", handleList);
    // server.on("/read", handleRead);
    // server.on("/upload", HTTP_POST, handleUpload);
    // server.on("/create", HTTP_POST, handleCreate);
    // server.on("/delete", HTTP_POST, handleDelete);
    // server.on("/run", HTTP_POST, handleRun);
    // server.on("/stop", HTTP_POST, handleStop);
    // server.on("/poll", handlePoll);
    server.begin();
    // webSocket.begin();
    // webSocket.onEvent(webSocketEvent);

    // Scheduler.startLoop(loopPhyNow());
}

void loop() {
    loopPhyNow();
    server.handleClient();
    // webSocket.loop();
    // if (run_requested) {
    //     run_requested = false;
    //     delay(50);
    //     run_lua(lua_code_pending);
    // }
}
