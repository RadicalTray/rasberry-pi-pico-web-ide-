#include <SPI.h>

#include "phy.hpp"

// Declares t1s_io and t1s_phy
Arduino_10BASE_T1S_PHY_TC6(SPI, PIN_ETH_SS, PIN_ETH_RST, PIN_ETH_IRQ);

static void OnPlcaStatus(bool success, bool plcaStatus) {
  if (!success) {
    Serial.print("PLCA status register read failed\n");
    return;
  }

  if (plcaStatus)
    Serial.print("PLCA Mode active\n");
  else {
    Serial.print("CSMA/CD fallback\n");
    t1s_phy.enablePlca();
  }
}

void initPhy() {
  pinMode(PIN_ETH_IRQ, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_ETH_IRQ),
                  []() { t1s_io.onInterrupt(); },
                  FALLING);

  if (!t1s_io.begin()) {
    Serial.print("'TC6_Io::begin(...)' failed.\n");
    for (;;) { }
  }

  MacAddress const mac_addr = MacAddress::create_from_uid();

  if (!t1s_phy.begin(
    PHY_IP_ADDR,
    PHY_NETWORK_MASK,
    PHY_GATEWAY,
    mac_addr,
    PHY_T1S_PLCA_SETTINGS,
    PHY_T1S_DEFAULT_MAC_SETTINGS
  )) {
    Serial.print("'TC6::begin(...)' failed.\n");
    for (;;) { }
  }

  Serial.print("IP\t");
  Serial.println(PHY_IP_ADDR);
  Serial.println(mac_addr);
  Serial.println(PHY_T1S_PLCA_SETTINGS);
  Serial.println(PHY_T1S_DEFAULT_MAC_SETTINGS);
}

void servicePhy() {
  static unsigned long prev_beacon_check = 0;
  auto now = millis();

  t1s_phy.service();

  if ((now - prev_beacon_check) > 1000) {
    prev_beacon_check = now;
    if (!t1s_phy.getPlcaStatus(OnPlcaStatus))
      Serial.print("getPlcaStatus(...) failed\n");
  }
}
