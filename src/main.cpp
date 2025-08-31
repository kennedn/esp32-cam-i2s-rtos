#include <WebServer.h>
#include <WiFi.h>
#include "esp_camera.h"

#include "globals.h"
#include "stream.h"
#include "camera_pins.h"
#include "logging.h"

// Global web server instance on port 80
WebServer server(80);

// RTOS task handle for the MJPEG streaming service
TaskHandle_t tMjpeg;            

void setup() {

  // Setup Serial connection for debugging/logging
  Serial.begin(SERIAL_RATE);
  delay(500); // Wait for Serial to connect

  setupLogging(); // Initialize logging system

  // Print initial memory and system info
  Log.trace("\n\nMulti-client MJPEG Server\n");
  Log.trace("setup: total heap  : %d\n", ESP.getHeapSize());
  Log.trace("setup: free heap   : %d\n", ESP.getFreeHeap());
  Log.trace("setup: total psram : %d\n", ESP.getPsramSize());
  Log.trace("setup: free psram  : %d\n", ESP.getFreePsram());

  // Camera configuration struct
  static camera_config_t camera_config = {
    .pin_pwdn       = PWDN_GPIO_NUM,
    .pin_reset      = RESET_GPIO_NUM,
    .pin_xclk       = XCLK_GPIO_NUM,
    .pin_sscb_sda   = SIOD_GPIO_NUM,
    .pin_sscb_scl   = SIOC_GPIO_NUM,
    .pin_d7         = Y9_GPIO_NUM,
    .pin_d6         = Y8_GPIO_NUM,
    .pin_d5         = Y7_GPIO_NUM,
    .pin_d4         = Y6_GPIO_NUM,
    .pin_d3         = Y5_GPIO_NUM,
    .pin_d2         = Y4_GPIO_NUM,
    .pin_d1         = Y3_GPIO_NUM,
    .pin_d0         = Y2_GPIO_NUM,
    .pin_vsync      = VSYNC_GPIO_NUM,
    .pin_href       = HREF_GPIO_NUM,
    .pin_pclk       = PCLK_GPIO_NUM,

    .xclk_freq_hz   = XCLK_FREQ,
    .ledc_timer     = LEDC_TIMER_0,
    .ledc_channel   = LEDC_CHANNEL_0,
    .pixel_format   = PIXFORMAT_JPEG,
    .frame_size     = FRAME_SIZE,
    .jpeg_quality   = JPEG_QUALITY,
    .fb_count       = 2,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_LATEST,
  };

  // Initialize the camera hardware
  if (esp_camera_init(&camera_config) != ESP_OK) {
    Log.fatal("setup: Error initializing the camera\n");
    delay(10000);
    ESP.restart();
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  // ESP-EYE board requires pullups on these pins
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

sensor_t* s = esp_camera_sensor_get();
#if defined (FLIP_VERTICALLY)
  // Optionally flip image vertically if requested
  s->set_vflip(s, true);
#endif

#if defined (WHITEBALANCE)
  s->set_wb_mode(s, WHITEBALANCE);
#endif

  // Connect to WiFi using credentials from build flags
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setSleep(false);

  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");

  // Print camera connection URL
  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.print(MJPEG_URL);
  Serial.println("' to connect");

  // Start the main streaming RTOS task on the PRO_CPU core
  xTaskCreatePinnedToCore(
    setupCB,                 // Task function
    "mjpeg",                 // Task name
    3 * KILOBYTE,            // Stack size
    NULL,                    // Parameters
    tskIDLE_PRIORITY + 2,    // Priority
    &tMjpeg,                 // Task handle
    PRO_CPU);                // CPU core

  Log.trace("setup complete: free heap  : %d\n", ESP.getFreeHeap());
}

void loop() {
  // Delete the default Arduino loop task, as everything is handled by RTOS tasks
  vTaskDelete(NULL);
}
