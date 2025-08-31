#include "globals.h"
#include "streaming.h"
#include <ArduinoLog.h>

// ==== Handle invalid URL requests ============================================
void handleNotFound() {
  String message = "Server is running!\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET) ? "GET" : "POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  server.send(200, "text / plain", message);
}

void setupCB(void* pvParameters) {
  TickType_t xLastWakeTime;
  const TickType_t xFrequency = pdMS_TO_TICKS(WSINTERVAL);

  // Creating frame synchronization semaphore and initializing it
  frameSync = xSemaphoreCreateBinary();
  xSemaphoreGive( frameSync );


  //  Creating RTOS task for grabbing frames from the camera
  xTaskCreatePinnedToCore(
      camCB,        // callback
      "cam",        // name
      4 * KILOBYTE, // stack size
      NULL,         // parameters
      tskIDLE_PRIORITY + 2, // priority
      &tCam,        // RTOS task handle
      APP_CPU);     // core

  //  Registering webserver handling routines
  server.on(MJPEG_URL, HTTP_GET, handleJPGSstream);
  server.onNotFound(handleNotFound);

  //  Starting webserver
  server.begin();

  Log.trace("setupCB: Starting streaming service\n");
  Log.verbose ("setupCB: free heap (start)  : %d\n", ESP.getFreeHeap());

  //=== loop() section  ===================
  xLastWakeTime = xTaskGetTickCount();
  for (;;) {
    server.handleClient();

    //  After every server client handling request, we let other tasks run and then pause
    if ( xTaskDelayUntil(&xLastWakeTime, xFrequency) != pdTRUE ) taskYIELD();
  }
}



