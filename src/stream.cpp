#include "globals.h"
#include "mjpeg.h"

/**
 * @brief Handles HTTP requests to invalid URLs.
 *
 * Responds with a message indicating the server is running and echoes request details.
 * This helps users and developers diagnose incorrect URLs or HTTP methods.
 *
 * @return void
 * @note Sends a response to the current HTTP client.
 */
void handleNotFound() {
  String message;
  message += "INMP441 Wav stream available at: <a href='http://"  + server.hostHeader() + String(I2S_URL)   + "'>http://" + server.hostHeader() + String(I2S_URL)   + "</a><br>";
  message += "OV2640 MJPEG stream available at: <a href='http://" + server.hostHeader() + String(MJPEG_URL) + "'>http://" + server.hostHeader() + String(MJPEG_URL) + "</a>";
  server.send(200, "text/html", message);
} 

/**
 * @brief RTOS task: Initializes streaming infrastructure and runs the main web server loop.
 *
 * Sets up synchronization primitives, launches the camera capture task, and registers HTTP handlers.
 * The main loop services HTTP clients and yields to other RTOS tasks at a fixed interval.
 *
 * @param pvParameters Unused (RTOS task parameter signature).
 * @return Never returns; runs as a FreeRTOS task.
 * @note Initializes global frameSync semaphore, starts camera and web server tasks, and handles HTTP requests.
 */
void setupCB(void* pvParameters) {
  TickType_t xLastWakeTime;
  const TickType_t xFrequency = pdMS_TO_TICKS(WSINTERVAL);

  // Create frame synchronization semaphore (binary) and set it to available
  frameSync = xSemaphoreCreateBinary();
  xSemaphoreGive(frameSync);

  // Launch camera capture RTOS task on the application core
  xTaskCreatePinnedToCore(
      camCB,        // Task function
      "cam",        // Task name
      4 * KILOBYTE, // Stack size
      NULL,         // Parameters
      tskIDLE_PRIORITY + 2, // Priority
      &tCam,        // Task handle
      APP_CPU);     // Core

  // Register HTTP handlers for MJPEG stream and 404s
  server.on(MJPEG_URL, HTTP_GET, handleJPGSstream);
  server.onNotFound(handleNotFound);

  // Start the web server
  server.begin();

  Log.trace("setupCB: Starting streaming service\n");
  Log.verbose("setupCB: free heap (start)  : %d\n", ESP.getFreeHeap());

  // Main server loop: handle HTTP clients and yield to other tasks
  xLastWakeTime = xTaskGetTickCount();
  for (;;) {
    server.handleClient();

    // Yield to other tasks and maintain a fixed interval between iterations
    if (xTaskDelayUntil(&xLastWakeTime, xFrequency) != pdTRUE ) taskYIELD();
  }
}



