#include "globals.h"
#include <WiFi.h>
#include "esp_camera.h"

#if defined(CAMERA_MULTICLIENT_QUEUE)

#if defined(BENCHMARK)
#include <AverageFilter.h>
#define BENCHMARK_PRINT_INT 5000
averageFilter<uint32_t> captureAvg(10);
uint32_t lastPrintCam = millis();
#endif

const char *HEADER = "HTTP/1.1 200 OK\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Content-Type: multipart/x-mixed-replace; boundary=+++===123454321===+++\r\n";
const char *BOUNDARY = "\r\n--+++===123454321===+++\r\n";
const char *CTNTTYPE = "Content-Type: image/jpeg\r\nContent-Length: ";
const int hdrLen = strlen(HEADER);
const int bdrLen = strlen(BOUNDARY);
const int cntLen = strlen(CTNTTYPE);

QueueHandle_t streamingClients;
SemaphoreHandle_t frameSync;
TaskHandle_t tCam;    // Camera frame capture task handle
TaskHandle_t tStream; // Streaming task handle

std::vector<uint8_t>* camBuf;   // Points to the latest captured frame for streaming

void handleNotFound();
void streamCB(void *pvParameters);

/**
 * @brief RTOS task: Continuously captures frames from the camera and updates the shared buffer.
 *
 * Uses double buffering to avoid race conditions between capture and streaming.
 * Synchronization is handled via a semaphore to ensure the streaming task never reads a partially written frame.
 *
 * @param pvParameters Unused (RTOS task parameter signature).
 * @return Never returns; runs as a FreeRTOS task.
 * @note Updates global camBuf pointer to point to the latest frame.
 * @note Blocks on frameSync semaphore to synchronize with the streaming task.
 */
void camCB(void *pvParameters) {
  TickType_t xLastWakeTime;
  const TickType_t xFrequency = pdMS_TO_TICKS(1000 / FPS);

  streamingClients = xQueueCreate(10, sizeof(WiFiClient *));
  xTaskCreatePinnedToCore(
      streamCB,
      "streamCB",
      4 * KILOBYTE,
      NULL,
      tskIDLE_PRIORITY + 2,
      &tStream,
      APP_CPU);

  std::vector<uint8_t> fbs[2]; // Double buffer for frames
  int ifb = 0;                 // Index for double buffering

  xLastWakeTime = xTaskGetTickCount();

#if defined(BENCHMARK)
  captureAvg.initialize();
#endif

  camera_fb_t *fb = NULL;

  for (;;) {
#if defined(BENCHMARK)
    uint32_t captureStart = micros();
#endif

    fb = esp_camera_fb_get();
    size_t s = fb->len;

    // Ensure buffer is large enough for the new frame (with some headroom to reduce reallocations)
    if (s > fbs[ifb].capacity()) {
      fbs[ifb].reserve(s * 4 / 3);
    }
    fbs[ifb].resize(s);

    memcpy(fbs[ifb].data(), fb->buf, s);
    esp_camera_fb_return(fb);

#if defined(BENCHMARK)
    captureAvg.value(micros() - captureStart);
#endif

    // Block until the streaming task has finished with the previous frame
    xSemaphoreTake(frameSync, portMAX_DELAY);

    // Publish the new frame for streaming
    camBuf = &fbs[ifb];
    ifb = (ifb + 1) & 1; // Toggle between two buffers

    // Allow the streaming task to access the new frame
    xSemaphoreGive(frameSync);

    // Notify the streaming task that a new frame is available (only required for the first frame)
    xTaskNotifyGive(tStream);

    // Maintain target frame rate
    if (xTaskDelayUntil(&xLastWakeTime, xFrequency) != pdTRUE)
      taskYIELD();

    // Suspend capture if there are no active clients (saves power)
    if (eTaskGetState(tStream) == eSuspended) {
      vTaskSuspend(NULL);
    }

#if defined(BENCHMARK)
    if (millis() - lastPrintCam > BENCHMARK_PRINT_INT) {
      lastPrintCam = millis();
      Log.verbose("setupCB: average frame capture time: %d microseconds\n", captureAvg.currentValue());
    }
#endif
  }
}

/**
 * @brief Handles new client connections for MJPEG streaming.
 *
 * Enforces a maximum client limit and immediately sends HTTP headers to the client.
 * Adds the client to the streaming queue and resumes streaming/capture tasks if needed.
 *
 * @return void
 * @note May allocate a new WiFiClient and modify the streamingClients queue.
 */
void handleJPGSstream(void) {
  if (!uxQueueSpacesAvailable(streamingClients)) {
    Log.error("handleJPGSstream: Max number of WiFi clients reached\n");
    return;
  }

  WiFiClient *client = new WiFiClient();
  if (client == NULL) {
    Log.error("handleJPGSstream: Can not create new WiFi client - OOM\n");
    return;
  }
  *client = server.client();

  client->setTimeout(1);
  client->write(HEADER, hdrLen);
  client->write(BOUNDARY, bdrLen);

  xQueueSend(streamingClients, (void *)&client, 0);

  // Resume tasks if they were suspended due to no clients
  if (eTaskGetState(tCam) == eSuspended)
    vTaskResume(tCam);
  if (eTaskGetState(tStream) == eSuspended)
    vTaskResume(tStream);

  Log.trace("handleJPGSstream: Client connected\n");
}

/**
 * @brief RTOS task: Streams the latest camera frame to all connected clients.
 *
 * Waits for new frames, then sends them to each client in the queue.
 * Uses a semaphore to ensure it never reads a frame while it is being updated.
 * Handles client disconnects and manages the client queue.
 *
 * @param pvParameters Unused (RTOS task parameter signature).
 * @return Never returns; runs as a FreeRTOS task.
 * @note Reads from global camBuf, modifies streamingClients queue, and interacts with WiFi clients.
 * @note Blocks on frameSync semaphore to synchronize with the camera task.
 */
void streamCB(void *pvParameters) {
  char buf[16];
  TickType_t xLastWakeTime;
  TickType_t xFrequency = pdMS_TO_TICKS(1000 / FPS);

  // Wait until the first frame is available
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

#if defined(BENCHMARK)
  averageFilter<int32_t> streamAvg(10);
  averageFilter<int32_t> waitAvg(10);
  averageFilter<uint32_t> frameAvg(10);
  averageFilter<float> fpsAvg(10);
  uint32_t streamStart = 0;
  streamAvg.initialize();
  waitAvg.initialize();
  frameAvg.initialize();
  uint32_t lastPrint = millis();
  uint32_t lastFrame = millis();
#endif

  xLastWakeTime = xTaskGetTickCount();
  for (;;) {
    UBaseType_t activeClients = uxQueueMessagesWaiting(streamingClients);
    if (activeClients) {
      WiFiClient *client;

      for (int i = 0; i < activeClients; i++) {
        xQueueReceive(streamingClients, (void *)&client, 0);

        if (!client->connected()) {
          // Remove disconnected clients from the queue
          Log.trace("streamCB: Client disconnected\n");
          delete client;
        } else {
#if defined(BENCHMARK)
          streamStart = micros();
#endif
          // Prevent reading a frame while it is being updated
          xSemaphoreTake(frameSync, portMAX_DELAY);

#if defined(BENCHMARK)
          waitAvg.value(micros() - streamStart);
          frameAvg.value(camBuf->size());
          streamStart = micros();
#endif

          // Send the current frame to the client
          sprintf(buf, "%zu\r\n\r\n", camBuf->size());
          client->write(CTNTTYPE, cntLen);
          client->write(buf, strlen(buf));
          client->write((char *)camBuf->data(), camBuf->size());
          client->write(BOUNDARY, bdrLen);

#if defined(BENCHMARK)
          streamAvg.value(micros() - streamStart);
#endif

          xSemaphoreGive(frameSync);

          // Keep the client in the queue for the next frame
          xQueueSend(streamingClients, (void *)&client, 0);
        }
      }
    } else {
      // No clients: suspend to save power
      vTaskSuspend(NULL);
    }

    if (xTaskDelayUntil(&xLastWakeTime, xFrequency) != pdTRUE)
      taskYIELD();

#if defined(BENCHMARK)
    fpsAvg.value(1000.0 / (float)(millis() - lastFrame));
    lastFrame = millis();
#endif

#if defined(BENCHMARK)
    if (millis() - lastPrint > BENCHMARK_PRINT_INT) {
      lastPrint = millis();
      Log.verbose("streamCB: wait avg=%d, stream avg=%d us, frame avg size=%d bytes, fps=%S\n", waitAvg.currentValue(), streamAvg.currentValue(), frameAvg.currentValue(), String(fpsAvg.currentValue()));
    }
#endif
  }
}

#endif
