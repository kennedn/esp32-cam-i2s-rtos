//  === Arduino-Log implementation  =================================================================
#include "globals.h"
#include "logging.h"

#define MILLIS_FUNCTION xTaskGetTickCount()
// #define MILLIS_FUNCTION millis()

#ifndef DISABLE_LOGGING
/**
 * @brief Prints a timestamp in [days:hours:minutes:seconds.milliseconds] format to the log output.
 *
 * Used as a prefix for log messages to aid in debugging and event tracing.
 *
 * @param logOutput Pointer to the Print object for log output.
 * @return void
 */
void printTimestampMillis(Print* logOutput);

/**
 * @brief Prints a buffer in hex and ASCII for debugging.
 *
 * Useful for inspecting raw data or binary buffers during development.
 *
 * @param aBuf Pointer to the buffer.
 * @param aSize Size of the buffer in bytes.
 * @return void
 */
void printBuffer(const char* aBuf, size_t aSize);
#endif  //   #ifndef DISABLE_LOGGING

/**
 * @brief Initializes the logging system.
 *
 * Sets up the log level, output stream, and timestamp prefix.
 * Should be called early in setup().
 *
 * @return void
 */
void setupLogging() {
#ifndef DISABLE_LOGGING
  Log.begin(LOG_LEVEL, &Serial);
  Log.setPrefix(printTimestampMillis);
  Log.trace("setupLogging()" CR);
#endif  //  #ifndef DISABLE_LOGGING
}

/**
 * @brief Prints a simple millisecond-based timestamp to the log output.
 *
 * @param logOutput Pointer to the Print object for log output.
 * @return void
 */
void printTimestamp(Print* logOutput) {
  char c[24];
  sprintf(c, "%10lu ", (long unsigned int) MILLIS_FUNCTION);
  logOutput->print(c);
}

/**
 * @brief Prints a formatted timestamp (days:hours:minutes:seconds.milliseconds) to the log output.
 *
 * @param logOutput Pointer to the Print object for log output.
 * @return void
 */
void printTimestampMillis(Print* logOutput) {
  char c[64];
  unsigned long mm = MILLIS_FUNCTION;
  int ms = mm % 1000;
  int s = mm / 1000;
  int m = s / 60;
  int h = m / 60;
  int d = h / 24;
  sprintf(c, "%02d:%02d:%02d:%02d.%03d ", d, h % 24, m % 60, s % 60, ms);
  logOutput->print(c);
}

#ifndef DISABLE_LOGGING
/**
 * @brief Prints the contents of a buffer in both hexadecimal and ASCII.
 *
 * Each line shows 16 bytes in hex and their ASCII representation.
 * Non-printable characters are shown as '.'.
 *
 * @param aBuf Pointer to the buffer.
 * @param aSize Size of the buffer in bytes.
 * @return void
 */
void printBuffer(const char* aBuf, size_t aSize) {
    Serial.println("Buffer contents:");

    char c;
    String s;
    size_t cnt = 0;

    for (int j = 0; j < aSize / 16 + 1; j++) {
      Serial.printf("%04x : ", j * 16);
      for (int i = 0; i < 16 && cnt < aSize; i++) {
        c = aBuf[cnt++];
        Serial.printf("%02x ", c);
        if (c < 32) {
          s += '.';
        }
        else {
          s += c;
        }
      }
      Serial.print(" : ");
      Serial.println(s);
      s = "";
    }
    Serial.println();
}
#else
void printBuffer(const char* aBuf, size_t aSize) {}
#endif
