#include <Arduino.h>
#include <lwip/dhcp.h>
#include <dhcpserver/dhcpserver.h>

#include "phy.hpp"

// If hostname is needed: https://github.com/maxgerhardt/esp-open-rtos-ping-example/tree/master
//  (just call lwip_getaddrinfo() to get hostname info)

static dhcp_server_t my_dhcp_server;
static struct dhcp netif_dhcp;

static void initDHCPServer() {
    IPAddress const ip = PHY_IP_ADDR;
    IPAddress const subnet = PHY_NETWORK_MASK;
    ip_addr_t lwip_ip, lwip_mask;
    IP_ADDR4(&lwip_ip, ip[0], ip[1], ip[2], ip[3]);
    IP_ADDR4(&lwip_mask, subnet[0], subnet[1], subnet[2], subnet[3]);
    dhcp_server_init(&my_dhcp_server, &lwip_ip, &lwip_mask, netif_default);
}

static void initDHCPClient() {
    dhcp_set_struct(netif_default, &netif_dhcp);
    err_t err = dhcp_start(netif_default);
    if (err != ERR_OK) {
        Serial.print("Failed to start dhcp client");
        return;
    }
    while (!dhcp_supplied_address(netif_default)) {
        Serial.print("Waiting for dhcp to supply ip address...\n");
        delay(1000);
    }
}

void initDHCP() {
    initDHCPClient();
    Serial.printf("IP address: %s\n", ip4addr_ntoa(netif_ip4_addr(netif_default)));

}
