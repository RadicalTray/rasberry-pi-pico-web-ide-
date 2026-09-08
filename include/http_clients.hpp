#pragma once
#include <Arduino.h>

void httpInit();
void httpClear();
int httpPost(const String &url, const String &contentType, const String &body);
int httpWait(int handle, int *code, String &text);
