#pragma once

#include <Arduino.h>

bool luaIsRunning();
void runLua(String code);
void stopLua();
void initLua();
void loopLua();
void doLuaStuff();
