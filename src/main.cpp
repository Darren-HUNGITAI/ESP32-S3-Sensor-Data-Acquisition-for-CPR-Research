#include <Arduino.h>
#include <string.h>

#define SAMPLE_TRANSPORT_USB_SERIAL 1
#define SAMPLE_TRANSPORT_WIFI_UDP 2

#ifndef SAMPLE_TRANSPORT
#define SAMPLE_TRANSPORT SAMPLE_TRANSPORT_USB_SERIAL
#endif

#if SAMPLE_TRANSPORT != SAMPLE_TRANSPORT_USB_SERIAL && SAMPLE_TRANSPORT != SAMPLE_TRANSPORT_WIFI_UDP
#error "Unsupported SAMPLE_TRANSPORT. Use 1 for USB serial or 2 for Wi-Fi UDP."
#endif

#if SAMPLE_TRANSPORT == SAMPLE_TRANSPORT_WIFI_UDP
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#include <WiFi.h>
#include <WiFiUdp.h>

#ifndef WIFI_SSID
#ifdef PRELOAD_WIFI_0_SSID
#define WIFI_SSID PRELOAD_WIFI_0_SSID
#else
#define WIFI_SSID ""
#endif
#endif

#ifndef WIFI_PASSWORD
#ifdef PRELOAD_WIFI_0_PASSWORD
#define WIFI_PASSWORD PRELOAD_WIFI_0_PASSWORD
#else
#define WIFI_PASSWORD ""
#endif
#endif

#ifndef UDP_TARGET_IP
#define UDP_TARGET_IP ""
#endif

#ifndef UDP_TARGET_PORT
#define UDP_TARGET_PORT 5005
#endif

#ifndef UDP_LOCAL_PORT
#define UDP_LOCAL_PORT 5006
#endif
#endif

static constexpr uint8_t PRESSURE_VOUT_ADC_PIN = 1; // MCP6002-B OUTB
static constexpr uint8_t PRESSURE_VREF_ADC_PIN = 2; // MCP6002-A OUTA

static constexpr uint32_t SERIAL_BAUD_RATE = 921600;
static constexpr uint32_t SAMPLE_INTERVAL_US = 10000; // 100 Hz
static constexpr size_t SERIAL_LINE_BUFFER_SIZE = 120;
static const char *CSV_HEADER_LINE =
    "uptime_ms,pressure_vout_raw,pressure_vout_mv,pressure_vref_raw,pressure_vref_mv,pressure_delta_mv";

#if SAMPLE_TRANSPORT == SAMPLE_TRANSPORT_WIFI_UDP
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
static constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 2000;
WiFiUDP udp;
IPAddress udpTargetIp;
static uint32_t lastWifiReconnectMs = 0;
#endif

static uint32_t lastSampleUs = 0;

struct PressureSample
{
  uint32_t uptimeMs;
  uint16_t pressureVoutRaw;
  uint32_t pressureVoutMv;
  uint16_t pressureVrefRaw;
  uint32_t pressureVrefMv;
  int32_t pressureDeltaMv;
};

static void initializePressureAdc()
{
  analogReadResolution(12);
  analogSetPinAttenuation(PRESSURE_VOUT_ADC_PIN, ADC_11db);
  analogSetPinAttenuation(PRESSURE_VREF_ADC_PIN, ADC_11db);
  pinMode(PRESSURE_VOUT_ADC_PIN, INPUT);
  pinMode(PRESSURE_VREF_ADC_PIN, INPUT);

  Serial.printf("# Pressure ADC Vout=GPIO%u, Vref=GPIO%u, resolution=12-bit\n",
                PRESSURE_VOUT_ADC_PIN,
                PRESSURE_VREF_ADC_PIN);
}

static void haltWithMessage(const char *message)
{
  Serial.println(message);
  while (true)
  {
    delay(1000);
  }
}

static void printStartupBanner()
{
  Serial.println("# ESP32-S3 N16R8 + A401/MCP6002 pressure validation logger");
#if SAMPLE_TRANSPORT == SAMPLE_TRANSPORT_USB_SERIAL
  Serial.println("# Transport: USB serial CSV");
#elif SAMPLE_TRANSPORT == SAMPLE_TRANSPORT_WIFI_UDP
  Serial.printf("# Transport: Wi-Fi UDP CSV -> %s:%u\n",
                UDP_TARGET_IP,
                static_cast<unsigned int>(UDP_TARGET_PORT));
#endif
  Serial.println("# Units: pressure ADC raw counts and millivolts");
  Serial.println("# ICM-20948 output is temporarily disabled for pressure-circuit validation.");
  Serial.printf("# CSV columns: %s\n", CSV_HEADER_LINE);
}

#if SAMPLE_TRANSPORT == SAMPLE_TRANSPORT_WIFI_UDP
static void beginUdpTransport()
{
  if (strlen(WIFI_SSID) == 0)
  {
    haltWithMessage("# Wi-Fi UDP transport requires WIFI_SSID in include/secrets.h.");
  }

  if (strlen(UDP_TARGET_IP) == 0 || !udpTargetIp.fromString(UDP_TARGET_IP))
  {
    haltWithMessage("# Wi-Fi UDP transport requires UDP_TARGET_IP as the PC IPv4 address in include/secrets.h.");
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("# Connecting to configured Wi-Fi");
  uint32_t startedAtMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAtMs < WIFI_CONNECT_TIMEOUT_MS)
  {
    Serial.print(".");
    delay(250);
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED)
  {
    haltWithMessage("# Wi-Fi connection failed. Check SSID/password and retry.");
  }

  if (udp.begin(UDP_LOCAL_PORT) == 0)
  {
    haltWithMessage("# UDP local socket failed to start.");
  }

  String localIp = WiFi.localIP().toString();
  Serial.printf("# Wi-Fi connected. ESP32 IP=%s, RSSI=%d dBm\n", localIp.c_str(), WiFi.RSSI());
  Serial.printf("# UDP sender ready. Target=%s:%u, local port=%u\n",
                UDP_TARGET_IP,
                static_cast<unsigned int>(UDP_TARGET_PORT),
                static_cast<unsigned int>(UDP_LOCAL_PORT));
}
#endif

static void beginSampleTransport()
{
#if SAMPLE_TRANSPORT == SAMPLE_TRANSPORT_WIFI_UDP
  beginUdpTransport();
#endif
}

static void maintainSampleTransport()
{
#if SAMPLE_TRANSPORT == SAMPLE_TRANSPORT_WIFI_UDP
  if (WiFi.status() == WL_CONNECTED)
  {
    return;
  }

  uint32_t nowMs = millis();
  if (nowMs - lastWifiReconnectMs < WIFI_RECONNECT_INTERVAL_MS)
  {
    return;
  }

  lastWifiReconnectMs = nowMs;
  Serial.println("# Wi-Fi disconnected; reconnecting.");
  WiFi.disconnect(false, false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
#endif
}

static void writeTransportLine(const char *line, size_t length)
{
#if SAMPLE_TRANSPORT == SAMPLE_TRANSPORT_USB_SERIAL
  Serial.write(reinterpret_cast<const uint8_t *>(line), length);
  Serial.write(static_cast<uint8_t>('\n'));
#elif SAMPLE_TRANSPORT == SAMPLE_TRANSPORT_WIFI_UDP
  if (WiFi.status() != WL_CONNECTED)
  {
    return;
  }

  udp.beginPacket(udpTargetIp, UDP_TARGET_PORT);
  udp.write(reinterpret_cast<const uint8_t *>(line), length);
  udp.write(static_cast<uint8_t>('\n'));
  udp.endPacket();
#endif
}

static void writeCsvHeader()
{
  writeTransportLine(CSV_HEADER_LINE, strlen(CSV_HEADER_LINE));
}

static bool readPressureSample(PressureSample &sample)
{
  uint32_t nowUs = micros();
  if (nowUs - lastSampleUs < SAMPLE_INTERVAL_US)
  {
    return false;
  }
  lastSampleUs = nowUs;

  sample.uptimeMs = millis();
  sample.pressureVoutRaw = analogRead(PRESSURE_VOUT_ADC_PIN);
  sample.pressureVoutMv = analogReadMilliVolts(PRESSURE_VOUT_ADC_PIN);
  sample.pressureVrefRaw = analogRead(PRESSURE_VREF_ADC_PIN);
  sample.pressureVrefMv = analogReadMilliVolts(PRESSURE_VREF_ADC_PIN);
  sample.pressureDeltaMv = static_cast<int32_t>(sample.pressureVoutMv) -
                           static_cast<int32_t>(sample.pressureVrefMv);
  return true;
}

static void writePressureSample(const PressureSample &sample)
{
  char line[SERIAL_LINE_BUFFER_SIZE];
  int written = snprintf(line,
                         sizeof(line),
                         "%lu,%u,%lu,%u,%lu,%ld",
                         static_cast<unsigned long>(sample.uptimeMs),
                         sample.pressureVoutRaw,
                         static_cast<unsigned long>(sample.pressureVoutMv),
                         sample.pressureVrefRaw,
                         static_cast<unsigned long>(sample.pressureVrefMv),
                         static_cast<long>(sample.pressureDeltaMv));

  if (written < 0 || static_cast<size_t>(written) >= sizeof(line))
  {
    Serial.println("# Dropped sample because the serial line buffer was too small.");
    return;
  }

  writeTransportLine(line, static_cast<size_t>(written));
}

void setup()
{
  Serial.begin(SERIAL_BAUD_RATE);
  delay(2000);

  Serial.println();
  printStartupBanner();

  initializePressureAdc();
  beginSampleTransport();
  writeCsvHeader();
}

void loop()
{
  maintainSampleTransport();

  PressureSample sample;
  if (readPressureSample(sample))
  {
    writePressureSample(sample);
  }
}
