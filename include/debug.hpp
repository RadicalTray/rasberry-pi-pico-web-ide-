#pragma once
#include <Arduino.h>

static void timer(const char *name) {
    static unsigned long prev_time = 0;
    auto now = millis();
    if (name != nullptr) {
        Serial.printf("%s took %d ms\n", name, now - prev_time);
    }
    prev_time = now;
}

static void dumpMemStats() {
    Serial.printf("free heap: %d (%d/%d)\n",
            rp2040.getFreeHeap(),
            rp2040.getUsedHeap(),
            rp2040.getTotalHeap()
    );
    // TODO: lwip heap
    //  requires defined(LWIP_DEBUG)
}
