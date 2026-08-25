#pragma once

#include <Arduino_10BASE_T1S.h>

uint8_t const PHY_T1S_PLCA_NODE_ID = 0; // 0 = PLCA coordinator or something
IPAddress const PHY_IP_ADDR     {192, 168,  42, 100 + PHY_T1S_PLCA_NODE_ID};
IPAddress const PHY_NETWORK_MASK{255, 255, 255,   0};
IPAddress const PHY_GATEWAY     = PHY_IP_ADDR;

T1SPlcaSettings const PHY_T1S_PLCA_SETTINGS{PHY_T1S_PLCA_NODE_ID};
T1SMacSettings  const PHY_T1S_DEFAULT_MAC_SETTINGS;

extern TC6::TC6_Io t1s_io;
extern TC6::TC6_Arduino_10BASE_T1S t1s_phy;

void initPhy();
void servicePhy();
