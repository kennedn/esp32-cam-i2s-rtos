#include "globals.h"
#include "stream.h"
#include <WiFi.h>
#include <ESP_I2S.h>
#include <wav_header.h>

#if defined(BENCHMARK)
#include <AverageFilter.h>
#define BENCHMARK_PRINT_INT 1000
averageFilter<int32_t> streamAvg(10);
uint32_t lastPrint = millis();
#endif

#define PIN_I2S_BCLK     13   // I2S Bit Clock
#define PIN_I2S_WS       15   // I2S Word Select (LRCLK)
#define PIN_I2S_SD       12   // I2S Serial Data (mic data out)

#define SAMPLE_RATE_HZ 44100
#define I2S_BIT_WIDTH I2S_DATA_BIT_WIDTH_16BIT
#define I2S_SLOT I2S_STD_SLOT_LEFT

const uint16_t num_channels = 1; // mono
const uint32_t sample_size = 0xFFFFFFFF; // live stream, so set max size since unknown
const pcm_wav_header_t wav_header = PCM_WAV_HEADER_DEFAULT(sample_size, I2S_BIT_WIDTH, SAMPLE_RATE_HZ, num_channels); 

TaskHandle_t tMic;
QueueHandle_t i2sClients = xQueueCreate(5, sizeof(WiFiClient*));
I2SClass i2s;

void I2SHandler() {
  if ( !uxQueueSpacesAvailable(i2sClients) ) {
    Serial.println("Max number of WiFi clients reached");
    return;
  }

  // Create a new WiFi Client object for this connection
  WiFiClient* client = new WiFiClient();
  if ( client == NULL ) {
    Serial.println("Can not create new WiFi client - OOM");
    return;
  }
  *client = server.client();

  // Send audio/wav chunked header to client
  client->setTimeout(1);
  client->setNoDelay(true);
  client->print(
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: audio/wav\r\n"
    "Accept-Ranges: none\r\n"
    "Transfer-Encoding: chunked\r\n"
    "Connection: close\r\n"
    "\r\n"
  );
  client->printf("%X\r\n", (unsigned)sizeof(wav_header));
  client->write((const char*)&wav_header, sizeof(wav_header));
  client->print("\r\n");
  client->clear(); 
  
  // Add the client to the streaming queue
  xQueueSend(i2sClients, (void *) &client, 0);

  if (eTaskGetState(tMic) == eSuspended) {
    vTaskResume(tMic);
  }
  Serial.println("Client connected");
}

void micCB(void *pv) {
  const size_t BYTES_PER_MS = (SAMPLE_RATE_HZ * (I2S_BIT_WIDTH / 8)) / 1000;
  const size_t BYTES_BUFFER = BYTES_PER_MS * 20; // ~20ms of audio
  std::vector<uint8_t> localBuf(BYTES_BUFFER);
  uint32_t streamStart = 0;
  for (;;) {
#if defined(BENCHMARK)
    streamStart = micros();
#endif
    UBaseType_t activeClients = uxQueueMessagesWaiting(i2sClients);
    // No clients, suspend this task
    if ( !activeClients ) {
      vTaskSuspend(NULL);
      continue;
    }
    size_t recievedBytes = i2s.readBytes((char*)localBuf.data(), localBuf.size());

    WiFiClient *client;
    for (int i=0; i < activeClients; i++) {
      xQueueReceive(i2sClients, (void*)&client, 0);

      if (!client->connected()) {
        Serial.println("Client disconnected");
        client->stop();
        delete client;
        continue;
      }
   
      client->printf("%X\r\n", localBuf.size());
      client->write(localBuf.data(), localBuf.size());
      client->print("\r\n");

      xQueueSend(i2sClients, (void *) &client, 0);
    }
#if defined(BENCHMARK)
    streamAvg.value(micros() - streamStart);
#endif
  }
}

void I2SSetup() {
  i2s.setPins(PIN_I2S_BCLK, PIN_I2S_WS, -1, PIN_I2S_SD, -1); // BCLK/SCK, LRCLK/WS, SDOUT, SDIN, MCLK
  bool ok = i2s.begin(I2S_MODE_STD, SAMPLE_RATE_HZ, I2S_BIT_WIDTH, I2S_SLOT_MODE_MONO, I2S_SLOT);
  if (!ok) {
    Log.fatal("I2S begin failed");
    return;
  }
}

