#pragma once
#include <Arduino.h>

static void dumpMemStats() {
    Serial.printf("free heap: %d (%d/%d)\n",
            rp2040.getFreeHeap(),
            rp2040.getUsedHeap(),
            rp2040.getTotalHeap()
    );
    // TODO: lwip heap
    //  requires defined(LWIP_DEBUG)
}
