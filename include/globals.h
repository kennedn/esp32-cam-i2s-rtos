#pragma once
#include <WebServer.h>
#include <ArduinoLog.h>

#define APP_CPU     1
#define PRO_CPU     0
#define KILOBYTE    1024
#define SERIAL_RATE 115200
#define MJPEG_URL "/mjpeg"
#define I2S_URL "/i2s"

extern SemaphoreHandle_t frameSync;
extern TaskHandle_t tCam;
extern TaskHandle_t tMic;
extern WebServer server;
