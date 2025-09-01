#include "globals.h"
#include <WiFi.h>
#include "esp_camera.h"

#if defined(CAMERA_MULTICLIENT_QUEUE)

#if defined(BENCHMARK)
#include <AverageFilter.h>
#define BENCHMARK_PRINT_INT 1000
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
TaskHandle_t tCam; // handles getting picture frames from the camera and storing them locally
TaskHandle_t tStream;

std::vector<uint8_t>* camBuf;   // pointer to the current frame vector

void handleNotFound();
void streamCB(void *pvParameters);

// ==== RTOS task to grab frames from the camera =========================
void camCB(void *pvParameters)
{
  TickType_t xLastWakeTime;

  //  A running interval associated with currently desired frame rate
  const TickType_t xFrequency = pdMS_TO_TICKS(1000 / FPS);

  // Create a queue to track all connected clients
  streamingClients = xQueueCreate(10, sizeof(WiFiClient *));

  // Create task to push the stream to all connected clients
  xTaskCreatePinnedToCore(
      streamCB,
      "streamCB",
      4 * KILOBYTE,
      NULL,
      tskIDLE_PRIORITY + 2,
      &tStream,
      APP_CPU);

  // Double buffer for frames and index of the current frame
  std::vector<uint8_t> fbs[2];
  int ifb = 0;

  xLastWakeTime = xTaskGetTickCount();

#if defined(BENCHMARK)
  captureAvg.initialize();
#endif

  camera_fb_t *fb = NULL;

  for (;;)
  {
    // Grab a frame from the camera and query its size

#if defined(BENCHMARK)
    uint32_t captureStart = micros();
#endif

    fb = esp_camera_fb_get();
    size_t s = fb->len;

    // If frame size is more than we have previously allocated, reserve 125% more space
    if (s > fbs[ifb].capacity())
    {
      fbs[ifb].reserve(s * 4 / 3);
    }

    // Resize the vector to the actual size of the frame
    fbs[ifb].resize(s);

    // Copy current frame into local buffer
    memcpy(fbs[ifb].data(), fb->buf, s);
    esp_camera_fb_return(fb);

#if defined(BENCHMARK)
    captureAvg.value(micros() - captureStart);
#endif

    // Wait on a semaphore until client operation completes before switching frames
    xSemaphoreTake(frameSync, portMAX_DELAY);

    // Switch the current frame pointer to the newly captured frame
    camBuf = &fbs[ifb];
    ++ifb;
    ifb = ifb & 1; // alternate between 0 and 1

    // Release the semaphore to allow streaming task to access the new frame
    xSemaphoreGive(frameSync);

    // Notify the streaming task that a frame is ready (only needed once)
    xTaskNotifyGive(tStream);

    // Wait until the end of the current frame rate interval (if any time left)
    if (xTaskDelayUntil(&xLastWakeTime, xFrequency) != pdTRUE)
      taskYIELD();

    // If streaming task has suspended itself (no active clients), suspend this task to save power
    if (eTaskGetState(tStream) == eSuspended)
    {
      vTaskSuspend(NULL); // passing NULL means "suspend yourself"
    }

#if defined(BENCHMARK)
    if (millis() - lastPrintCam > BENCHMARK_PRINT_INT)
    {
      lastPrintCam = millis();
      Log.verbose("setupCB: average frame capture time: %d microseconds\n", captureAvg.currentValue());
    }
#endif
  }
}

// ==== Handle connection request from clients ===============================
void handleJPGSstream(void)
{
  // Can only accommodate 10 clients. The limit is a default for WiFi connections
  if (!uxQueueSpacesAvailable(streamingClients))
  {
    Log.error("handleJPGSstream: Max number of WiFi clients reached\n");
    return;
  }

  // Create a new WiFi Client object to keep track of this one
  WiFiClient *client = new WiFiClient();
  if (client == NULL)
  {
    Log.error("handleJPGSstream: Can not create new WiFi client - OOM\n");
    return;
  }
  *client = server.client();

  // Immediately send this client a header
  client->setTimeout(1);
  client->write(HEADER, hdrLen);
  client->write(BOUNDARY, bdrLen);

  // Push the client to the streaming queue
  xQueueSend(streamingClients, (void *)&client, 0);

  // Wake up streaming tasks, if they were previously suspended:
  if (eTaskGetState(tCam) == eSuspended)
    vTaskResume(tCam);
  if (eTaskGetState(tStream) == eSuspended)
    vTaskResume(tStream);

  Log.trace("handleJPGSstream: Client connected\n");
}

// ==== Actually stream content to all connected clients ========================
void streamCB(void *pvParameters)
{
  char buf[16];
  TickType_t xLastWakeTime;
  TickType_t xFrequency = pdMS_TO_TICKS(1000 / FPS);

  // Wait until the first frame is captured and there is something to send to clients
  ulTaskNotifyTake(pdTRUE,         /* Clear the notification value before exiting. */
                   portMAX_DELAY); /* Block indefinitely. */

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
  for (;;)
  {
    // Only bother to send anything if there is someone watching
    UBaseType_t activeClients = uxQueueMessagesWaiting(streamingClients);
    if (activeClients)
    {
      WiFiClient *client;

      for (int i = 0; i < activeClients; i++)
      {
        // Pop a client from the front of the queue
        xQueueReceive(streamingClients, (void *)&client, 0);

        // Check if this client is still connected.
        if (!client->connected())
        {
          // Delete this client reference if disconnected and don't put it back on the queue
          Log.trace("streamCB: Client disconnected\n");
          delete client;
        }
        else
        {
          // Actively connected client: grab a semaphore to prevent frame changes while serving this frame
#if defined(BENCHMARK)
          streamStart = micros();
#endif

          xSemaphoreTake(frameSync, portMAX_DELAY);

#if defined(BENCHMARK)
          waitAvg.value(micros() - streamStart);
          frameAvg.value(camBuf->size());
          streamStart = micros();
#endif

          // Send the frame to the client
          sprintf(buf, "%zu\r\n\r\n", camBuf->size());
          client->write(CTNTTYPE, cntLen);
          client->write(buf, strlen(buf));
          client->write((char *)camBuf->data(), camBuf->size());
          client->write(BOUNDARY, bdrLen);

#if defined(BENCHMARK)
          streamAvg.value(micros() - streamStart);
#endif

          // The frame has been served. Release the semaphore and let other tasks run.
          // If there is a frame switch ready, it will happen now in between frames
          xSemaphoreGive(frameSync);

          // Since this client is still connected, push it to the end of the queue for further processing
          xQueueSend(streamingClients, (void *)&client, 0);
        }
      }
    }
    else
    {
      // Since there are no connected clients, suspend this task to save power
      vTaskSuspend(NULL);
    }
    // Let other tasks run after serving every client
    if (xTaskDelayUntil(&xLastWakeTime, xFrequency) != pdTRUE)
      taskYIELD();

#if defined(BENCHMARK)
    fpsAvg.value(1000.0 / (float)(millis() - lastFrame));
    lastFrame = millis();
#endif

#if defined(BENCHMARK)
    if (millis() - lastPrint > BENCHMARK_PRINT_INT)
    {
      lastPrint = millis();
      Log.verbose("streamCB: wait avg=%d, stream avg=%d us, frame avg size=%d bytes, fps=%S\n", waitAvg.currentValue(), streamAvg.currentValue(), frameAvg.currentValue(), String(fpsAvg.currentValue()));
    }
#endif
  }
}

#endif
