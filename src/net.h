#pragma once
#include <Arduino.h>
#include <IPAddress.h>

void net_begin();
void net_loop();

IPAddress net_localIP();   // active ETH or WiFi IP
bool      net_connected(); // ETH link up OR WiFi connected
