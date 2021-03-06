// Copyright (c) 2021 - Schelte Bron

#include <Arduino.h>
#include <LittleFS.h>
#include <OTGWSerial.h>

#define I2CSCL D1
#define I2CSDA D2
#define BUTTON D3
#define PICRST D5

#define LED1 D4
#define LED2 D0

#define FIRMWARE "/gateway.hex"

extern OTGWSerial Pic;

void fwupgradestart(const char *hexfile);
