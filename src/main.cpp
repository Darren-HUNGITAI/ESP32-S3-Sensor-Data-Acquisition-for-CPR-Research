#include <Arduino.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <Wire.h>

#include "ICM_20948.h"
#include "secrets.h"

#ifndef PRELOAD_WIFI_0_SSID
#ifdef WIFI_SSID
#define PRELOAD_WIFI_0_SSID WIFI_SSID
#else
#define PRELOAD_WIFI_0_SSID ""
#endif
#endif

#ifndef PRELOAD_WIFI_0_PASSWORD
#ifdef WIFI_PASSWORD
#define PRELOAD_WIFI_0_PASSWORD WIFI_PASSWORD
#else
#define PRELOAD_WIFI_0_PASSWORD ""
#endif
#endif

#ifndef PRELOAD_WIFI_1_SSID
#define PRELOAD_WIFI_1_SSID ""
#endif
#ifndef PRELOAD_WIFI_1_PASSWORD
#define PRELOAD_WIFI_1_PASSWORD ""
#endif
#ifndef PRELOAD_WIFI_2_SSID
#define PRELOAD_WIFI_2_SSID ""
#endif
#ifndef PRELOAD_WIFI_2_PASSWORD
#define PRELOAD_WIFI_2_PASSWORD ""
#endif
#ifndef PRELOAD_WIFI_3_SSID
#define PRELOAD_WIFI_3_SSID ""
#endif
#ifndef PRELOAD_WIFI_3_PASSWORD
#define PRELOAD_WIFI_3_PASSWORD ""
#endif

static constexpr uint8_t I2C_SDA_PIN = 8;
static constexpr uint8_t I2C_SCL_PIN = 9;
static constexpr uint32_t I2C_CLOCK_HZ = 400000;

static constexpr bool ICM_AD0_VAL = false; // AD0/SDO tied to GND => address 0x68
static constexpr uint8_t ICM_EXPECTED_ADDR = 0x68;

static constexpr uint8_t CONFIG_BUTTON_PIN = 0; // BOOT button on most ESP32-S3 dev boards
static constexpr uint32_t CONFIG_BUTTON_HOLD_MS = 5000;
static constexpr uint32_t CONFIG_PORTAL_TIMEOUT_MS = 300000;
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
static constexpr uint32_t MQTT_RETRY_MS = 3000;
static constexpr uint32_t PUBLISH_INTERVAL_MS = 100;

static constexpr size_t WIFI_SLOT_COUNT = 4;
static constexpr size_t WIFI_SSID_LEN = 33;
static constexpr size_t WIFI_PASS_LEN = 65;

static const char *CONFIG_AP_NAME = "ICM20948-Setup";
static const char *CONFIG_AP_PASSWORD = nullptr; // Open AP for easier phone setup.
static const char *MQTT_DATA_TOPIC = "esp32s3/icm20948/data";
static const char *MQTT_STATUS_TOPIC = "esp32s3/icm20948/status";

struct WifiCredential
{
  char ssid[WIFI_SSID_LEN] = "";
  char password[WIFI_PASS_LEN] = "";
};

struct MqttConfig
{
  char host[96] = "";
  char port[8] = "1883";
  char username[64] = "";
  char password[64] = "";
  char tls[4] = "0";
};

ICM_20948_I2C imu;
WiFiClient plainClient;
WiFiClientSecure secureClient;
PubSubClient mqttClient(plainClient);
WifiCredential wifiCredentials[WIFI_SLOT_COUNT];
MqttConfig mqttConfig;

static uint32_t lastMqttAttemptMs = 0;
static uint32_t lastPublishMs = 0;
static uint32_t configButtonPressedAtMs = 0;

static const char *const PRELOAD_WIFI_SSIDS[WIFI_SLOT_COUNT] = {
    PRELOAD_WIFI_0_SSID,
    PRELOAD_WIFI_1_SSID,
    PRELOAD_WIFI_2_SSID,
    PRELOAD_WIFI_3_SSID,
};

static const char *const PRELOAD_WIFI_PASSWORDS[WIFI_SLOT_COUNT] = {
    PRELOAD_WIFI_0_PASSWORD,
    PRELOAD_WIFI_1_PASSWORD,
    PRELOAD_WIFI_2_PASSWORD,
    PRELOAD_WIFI_3_PASSWORD,
};

static void copyString(char *destination, size_t destinationSize, const String &source)
{
  snprintf(destination, destinationSize, "%s", source.c_str());
}

static void copyString(char *destination, size_t destinationSize, const char *source)
{
  snprintf(destination, destinationSize, "%s", source == nullptr ? "" : source);
}

static bool isNonEmpty(const char *value)
{
  return value != nullptr && value[0] != '\0';
}

static bool mqttUsesTls()
{
  return mqttConfig.tls[0] == '1' || mqttConfig.tls[0] == 'y' || mqttConfig.tls[0] == 'Y';
}

static uint16_t mqttPort()
{
  char *end = nullptr;
  unsigned long value = strtoul(mqttConfig.port, &end, 10);
  if (end == mqttConfig.port || value == 0 || value > 65535)
  {
    return mqttUsesTls() ? 8883 : 1883;
  }
  return static_cast<uint16_t>(value);
}

static void loadMqttConfig()
{
  Preferences preferences;
  preferences.begin("mqtt", false);
  copyString(mqttConfig.host, sizeof(mqttConfig.host), preferences.getString("host", ""));
  copyString(mqttConfig.port, sizeof(mqttConfig.port), preferences.getString("port", "1883"));
  copyString(mqttConfig.username, sizeof(mqttConfig.username), preferences.getString("user", ""));
  copyString(mqttConfig.password, sizeof(mqttConfig.password), preferences.getString("pass", ""));
  copyString(mqttConfig.tls, sizeof(mqttConfig.tls), preferences.getString("tls", "0"));
  preferences.end();
}

static void saveMqttConfig()
{
  Preferences preferences;
  preferences.begin("mqtt", false);
  preferences.putString("host", mqttConfig.host);
  preferences.putString("port", mqttConfig.port);
  preferences.putString("user", mqttConfig.username);
  preferences.putString("pass", mqttConfig.password);
  preferences.putString("tls", mqttConfig.tls);
  preferences.end();
}

static void loadWifiCredentials()
{
  Preferences preferences;
  preferences.begin("wifi_multi", false);
  for (size_t index = 0; index < WIFI_SLOT_COUNT; index++)
  {
    char ssidKey[8];
    char passKey[8];
    snprintf(ssidKey, sizeof(ssidKey), "ssid%u", static_cast<unsigned int>(index));
    snprintf(passKey, sizeof(passKey), "pass%u", static_cast<unsigned int>(index));

    copyString(wifiCredentials[index].ssid,
               sizeof(wifiCredentials[index].ssid),
               preferences.getString(ssidKey, ""));
    copyString(wifiCredentials[index].password,
               sizeof(wifiCredentials[index].password),
               preferences.getString(passKey, ""));
  }
  preferences.end();
}

static void saveWifiCredentials()
{
  Preferences preferences;
  preferences.begin("wifi_multi", false);
  for (size_t index = 0; index < WIFI_SLOT_COUNT; index++)
  {
    char ssidKey[8];
    char passKey[8];
    snprintf(ssidKey, sizeof(ssidKey), "ssid%u", static_cast<unsigned int>(index));
    snprintf(passKey, sizeof(passKey), "pass%u", static_cast<unsigned int>(index));

    preferences.putString(ssidKey, wifiCredentials[index].ssid);
    preferences.putString(passKey, wifiCredentials[index].password);
  }
  preferences.end();
}

static void clearWifiCredentials()
{
  Preferences preferences;
  preferences.begin("wifi_multi", false);
  preferences.clear();
  preferences.end();

  for (size_t index = 0; index < WIFI_SLOT_COUNT; index++)
  {
    wifiCredentials[index].ssid[0] = '\0';
    wifiCredentials[index].password[0] = '\0';
  }
}

static bool hasAnyWifiCredential()
{
  for (size_t index = 0; index < WIFI_SLOT_COUNT; index++)
  {
    if (isNonEmpty(wifiCredentials[index].ssid))
    {
      return true;
    }
  }
  return false;
}

static void applyPreloadedWifiCredentials()
{
  bool changed = false;

  for (size_t index = 0; index < WIFI_SLOT_COUNT; index++)
  {
    if (!isNonEmpty(PRELOAD_WIFI_SSIDS[index]))
    {
      continue;
    }

    bool slotChanged = strcmp(wifiCredentials[index].ssid, PRELOAD_WIFI_SSIDS[index]) != 0 ||
                       strcmp(wifiCredentials[index].password, PRELOAD_WIFI_PASSWORDS[index]) != 0;

    copyString(wifiCredentials[index].ssid, sizeof(wifiCredentials[index].ssid), PRELOAD_WIFI_SSIDS[index]);
    copyString(wifiCredentials[index].password, sizeof(wifiCredentials[index].password), PRELOAD_WIFI_PASSWORDS[index]);

    Serial.printf("Preloaded Wi-Fi slot %u from secrets.h: %s\n",
                  static_cast<unsigned int>(index),
                  wifiCredentials[index].ssid);

    changed = changed || slotChanged;
  }

  if (changed)
  {
    saveWifiCredentials();
  }
}

static void resetSavedConfig()
{
  Serial.println("Resetting saved Wi-Fi and MQTT settings...");

  clearWifiCredentials();

  Preferences preferences;
  preferences.begin("mqtt", false);
  preferences.clear();
  preferences.end();

  WiFiManager wifiManager;
  wifiManager.resetSettings();
  WiFi.persistent(true);
  WiFi.disconnect(true, true);
  WiFi.persistent(false);
  delay(500);
  ESP.restart();
}

static void printHexAddress(uint8_t address)
{
  Serial.print("0x");
  if (address < 0x10)
  {
    Serial.print("0");
  }
  Serial.print(address, HEX);
}

static bool scanI2CBus()
{
  bool foundExpectedDevice = false;

  Serial.println("Scanning I2C bus...");
  for (uint8_t address = 1; address < 127; address++)
  {
    Wire.beginTransmission(address);
    uint8_t error = Wire.endTransmission();

    if (error == 0)
    {
      Serial.print("  Found device at ");
      printHexAddress(address);
      if (address == ICM_EXPECTED_ADDR)
      {
        Serial.print("  <- ICM-20948 expected address");
        foundExpectedDevice = true;
      }
      Serial.println();
    }
  }

  if (!foundExpectedDevice)
  {
    Serial.print("Did not find ICM-20948 at ");
    printHexAddress(ICM_EXPECTED_ADDR);
    Serial.println(". Check SDA/SCL, power, GND, AD0, and NCS wiring.");
  }

  return foundExpectedDevice;
}

static void initializeImu()
{
  Serial.println("Initializing ICM-20948...");

  while (true)
  {
    imu.begin(Wire, ICM_AD0_VAL);

    Serial.print("  Status: ");
    Serial.println(imu.statusString());

    if (imu.status == ICM_20948_Stat_Ok)
    {
      Serial.println("ICM-20948 initialized.");
      return;
    }

    Serial.println("  Retrying in 500 ms...");
    delay(500);
  }
}

static bool connectToWifiSlot(size_t index)
{
  if (!isNonEmpty(wifiCredentials[index].ssid))
  {
    return false;
  }

  Serial.printf("Connecting to Wi-Fi slot %u: %s",
                static_cast<unsigned int>(index),
                wifiCredentials[index].ssid);

  WiFi.disconnect(false, false);
  delay(100);
  WiFi.begin(wifiCredentials[index].ssid, wifiCredentials[index].password);

  uint32_t startedAtMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAtMs < WIFI_CONNECT_TIMEOUT_MS)
  {
    Serial.print(".");
    delay(500);
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.print("Wi-Fi connected. ESP32 IP: ");
    Serial.println(WiFi.localIP());
    Serial.printf("Wi-Fi RSSI: %d dBm\n", WiFi.RSSI());
    return true;
  }

  Serial.printf("Wi-Fi slot %u failed.\n", static_cast<unsigned int>(index));
  WiFi.disconnect(false, false);
  delay(200);
  return false;
}

static bool connectToStoredWifi()
{
  if (!hasAnyWifiCredential())
  {
    Serial.println("No stored Wi-Fi credentials.");
    return false;
  }

  bool visible[WIFI_SLOT_COUNT] = {};

  Serial.println("Scanning Wi-Fi networks...");
  int networkCount = WiFi.scanNetworks(false, true);
  if (networkCount < 0)
  {
    Serial.println("Wi-Fi scan failed. Trying stored slots anyway.");
  }
  else
  {
    Serial.printf("Found %d Wi-Fi network(s).\n", networkCount);
    for (int networkIndex = 0; networkIndex < networkCount; networkIndex++)
    {
      String scannedSsid = WiFi.SSID(networkIndex);
      for (size_t slot = 0; slot < WIFI_SLOT_COUNT; slot++)
      {
        if (isNonEmpty(wifiCredentials[slot].ssid) && scannedSsid == wifiCredentials[slot].ssid)
        {
          visible[slot] = true;
        }
      }
    }
  }
  WiFi.scanDelete();

  for (size_t slot = 0; slot < WIFI_SLOT_COUNT; slot++)
  {
    if (visible[slot] && connectToWifiSlot(slot))
    {
      return true;
    }
  }

  Serial.println("Trying stored Wi-Fi slots that were not visible in scan.");
  for (size_t slot = 0; slot < WIFI_SLOT_COUNT; slot++)
  {
    if (!visible[slot] && connectToWifiSlot(slot))
    {
      return true;
    }
  }

  Serial.println("Could not connect to any stored Wi-Fi.");
  return false;
}

static void copyPortalValuesToConfig(WiFiManagerParameter *wifiSsidParams,
                                     WiFiManagerParameter *wifiPassParams,
                                     WiFiManagerParameter &mqttHostParam,
                                     WiFiManagerParameter &mqttPortParam,
                                     WiFiManagerParameter &mqttUserParam,
                                     WiFiManagerParameter &mqttPassParam,
                                     WiFiManagerParameter &mqttTlsParam)
{
  for (size_t slot = 0; slot < WIFI_SLOT_COUNT; slot++)
  {
    copyString(wifiCredentials[slot].ssid,
               sizeof(wifiCredentials[slot].ssid),
               wifiSsidParams[slot].getValue());
    copyString(wifiCredentials[slot].password,
               sizeof(wifiCredentials[slot].password),
               wifiPassParams[slot].getValue());
  }

  copyString(mqttConfig.host, sizeof(mqttConfig.host), mqttHostParam.getValue());
  copyString(mqttConfig.port, sizeof(mqttConfig.port), mqttPortParam.getValue());
  copyString(mqttConfig.username, sizeof(mqttConfig.username), mqttUserParam.getValue());
  copyString(mqttConfig.password, sizeof(mqttConfig.password), mqttPassParam.getValue());
  copyString(mqttConfig.tls, sizeof(mqttConfig.tls), mqttTlsParam.getValue());
}

static void startConfigPortalAndRestart()
{
  Serial.println("Starting setup portal.");
  Serial.printf("Connect to SSID \"%s\" and open http://192.168.4.1\n", CONFIG_AP_NAME);

  bool portalSaved = false;

  WiFiManager wifiManager;
  wifiManager.setConfigPortalBlocking(false);
  wifiManager.setConfigPortalTimeout(CONFIG_PORTAL_TIMEOUT_MS / 1000);
  wifiManager.setParamsPage(true);
  wifiManager.setShowPassword(true);
  wifiManager.setSaveParamsCallback([&portalSaved]() {
    portalSaved = true;
  });

  const char *menu[] = {"param", "info", "exit"};
  wifiManager.setMenu(menu, 3);

  WiFiManagerParameter wifiHeader("<p><b>Wi-Fi slots (priority: 0 to 3)</b></p>");
  WiFiManagerParameter wifiSsidParams[WIFI_SLOT_COUNT] = {
      WiFiManagerParameter("wifi0_ssid", "Wi-Fi slot 0 SSID", wifiCredentials[0].ssid, sizeof(wifiCredentials[0].ssid)),
      WiFiManagerParameter("wifi1_ssid", "Wi-Fi slot 1 SSID", wifiCredentials[1].ssid, sizeof(wifiCredentials[1].ssid)),
      WiFiManagerParameter("wifi2_ssid", "Wi-Fi slot 2 SSID", wifiCredentials[2].ssid, sizeof(wifiCredentials[2].ssid)),
      WiFiManagerParameter("wifi3_ssid", "Wi-Fi slot 3 SSID", wifiCredentials[3].ssid, sizeof(wifiCredentials[3].ssid)),
  };
  WiFiManagerParameter wifiPassParams[WIFI_SLOT_COUNT] = {
      WiFiManagerParameter("wifi0_pass", "Wi-Fi slot 0 password", wifiCredentials[0].password, sizeof(wifiCredentials[0].password), "type=\"password\""),
      WiFiManagerParameter("wifi1_pass", "Wi-Fi slot 1 password", wifiCredentials[1].password, sizeof(wifiCredentials[1].password), "type=\"password\""),
      WiFiManagerParameter("wifi2_pass", "Wi-Fi slot 2 password", wifiCredentials[2].password, sizeof(wifiCredentials[2].password), "type=\"password\""),
      WiFiManagerParameter("wifi3_pass", "Wi-Fi slot 3 password", wifiCredentials[3].password, sizeof(wifiCredentials[3].password), "type=\"password\""),
  };

  WiFiManagerParameter mqttHeader("<p><b>MQTT</b></p>");
  WiFiManagerParameter mqttHostParam("mqtt_host", "MQTT host or IP", mqttConfig.host, sizeof(mqttConfig.host));
  WiFiManagerParameter mqttPortParam("mqtt_port", "MQTT port", mqttConfig.port, sizeof(mqttConfig.port));
  WiFiManagerParameter mqttUserParam("mqtt_user", "MQTT username", mqttConfig.username, sizeof(mqttConfig.username));
  WiFiManagerParameter mqttPassParam("mqtt_pass", "MQTT password", mqttConfig.password, sizeof(mqttConfig.password), "type=\"password\"");
  WiFiManagerParameter mqttTlsParam("mqtt_tls", "MQTT TLS? 0=no, 1=yes", mqttConfig.tls, sizeof(mqttConfig.tls));

  wifiManager.addParameter(&wifiHeader);
  for (size_t slot = 0; slot < WIFI_SLOT_COUNT; slot++)
  {
    wifiManager.addParameter(&wifiSsidParams[slot]);
    wifiManager.addParameter(&wifiPassParams[slot]);
  }
  wifiManager.addParameter(&mqttHeader);
  wifiManager.addParameter(&mqttHostParam);
  wifiManager.addParameter(&mqttPortParam);
  wifiManager.addParameter(&mqttUserParam);
  wifiManager.addParameter(&mqttPassParam);
  wifiManager.addParameter(&mqttTlsParam);

  wifiManager.startConfigPortal(CONFIG_AP_NAME, CONFIG_AP_PASSWORD);

  uint32_t startedAtMs = millis();
  while (millis() - startedAtMs < CONFIG_PORTAL_TIMEOUT_MS)
  {
    wifiManager.process();

    if (portalSaved)
    {
      copyPortalValuesToConfig(wifiSsidParams,
                               wifiPassParams,
                               mqttHostParam,
                               mqttPortParam,
                               mqttUserParam,
                               mqttPassParam,
                               mqttTlsParam);
      saveWifiCredentials();
      saveMqttConfig();

      Serial.println("Saved Wi-Fi/MQTT settings. Restarting...");
      wifiManager.stopConfigPortal();
      delay(500);
      ESP.restart();
    }

    delay(10);
    yield();
  }

  Serial.println("Setup portal timed out. Restarting...");
  wifiManager.stopConfigPortal();
  delay(500);
  ESP.restart();
}

static void setupWifiAndConfigPortal()
{
  loadMqttConfig();
  loadWifiCredentials();
  applyPreloadedWifiCredentials();

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);

  bool missingMqttHost = strlen(mqttConfig.host) == 0;
  if (missingMqttHost)
  {
    Serial.println("MQTT host is not configured.");
    startConfigPortalAndRestart();
  }

  if (!connectToStoredWifi())
  {
    startConfigPortalAndRestart();
  }

  Serial.printf("MQTT target: %s:%u, TLS=%s\n",
                mqttConfig.host,
                mqttPort(),
                mqttUsesTls() ? "yes" : "no");
}

static void configureMqttClient()
{
  if (mqttUsesTls())
  {
    secureClient.setInsecure();
    secureClient.setHandshakeTimeout(15);
    mqttClient.setClient(secureClient);
  }
  else
  {
    mqttClient.setClient(plainClient);
  }

  mqttClient.setServer(mqttConfig.host, mqttPort());
  mqttClient.setBufferSize(512);
}

static void makeMqttClientId(char *buffer, size_t bufferSize)
{
  uint64_t chipId = ESP.getEfuseMac();
  snprintf(buffer,
           bufferSize,
           "esp32s3-icm20948-%04X%08X",
           static_cast<uint16_t>(chipId >> 32),
           static_cast<uint32_t>(chipId));
}

static void connectMqtt()
{
  if (mqttClient.connected() || strlen(mqttConfig.host) == 0)
  {
    return;
  }

  uint32_t nowMs = millis();
  if (nowMs - lastMqttAttemptMs < MQTT_RETRY_MS)
  {
    return;
  }
  lastMqttAttemptMs = nowMs;

  char clientId[40];
  makeMqttClientId(clientId, sizeof(clientId));

  Serial.printf("Connecting to MQTT broker %s:%u as %s... ",
                mqttConfig.host,
                mqttPort(),
                clientId);

  bool connected = false;
  if (strlen(mqttConfig.username) > 0)
  {
    connected = mqttClient.connect(clientId,
                                   mqttConfig.username,
                                   mqttConfig.password,
                                   MQTT_STATUS_TOPIC,
                                   0,
                                   true,
                                   "offline");
  }
  else
  {
    connected = mqttClient.connect(clientId,
                                   MQTT_STATUS_TOPIC,
                                   0,
                                   true,
                                   "offline");
  }

  if (connected)
  {
    Serial.println("connected");
    mqttClient.publish(MQTT_STATUS_TOPIC, "online", true);
  }
  else
  {
    Serial.printf("failed, state=%d\n", mqttClient.state());
  }
}

static void publishImuSample()
{
  if (!mqttClient.connected() || !imu.dataReady())
  {
    return;
  }

  uint32_t nowMs = millis();
  if (nowMs - lastPublishMs < PUBLISH_INTERVAL_MS)
  {
    return;
  }
  lastPublishMs = nowMs;

  imu.getAGMT();

  char payload[384];
  snprintf(payload,
           sizeof(payload),
           "{\"uptime_ms\":%lu,"
           "\"acc_mg\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f},"
           "\"gyr_dps\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f},"
           "\"mag_uT\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f},"
           "\"temp_c\":%.2f}",
           static_cast<unsigned long>(nowMs),
           imu.accX(),
           imu.accY(),
           imu.accZ(),
           imu.gyrX(),
           imu.gyrY(),
           imu.gyrZ(),
           imu.magX(),
           imu.magY(),
           imu.magZ(),
           imu.temp());

  bool ok = mqttClient.publish(MQTT_DATA_TOPIC, payload);
  Serial.println(ok ? payload : "MQTT publish failed");
}

static void handleConfigButton()
{
  bool pressed = digitalRead(CONFIG_BUTTON_PIN) == LOW;

  if (!pressed)
  {
    configButtonPressedAtMs = 0;
    return;
  }

  if (configButtonPressedAtMs == 0)
  {
    configButtonPressedAtMs = millis();
    Serial.println("Keep holding BOOT for 5 seconds to reset Wi-Fi/MQTT settings.");
    return;
  }

  if (millis() - configButtonPressedAtMs >= CONFIG_BUTTON_HOLD_MS)
  {
    resetSavedConfig();
  }
}

void setup()
{
  Serial.begin(115200);
  delay(2000);

  pinMode(CONFIG_BUTTON_PIN, INPUT_PULLUP);

  Serial.println();
  Serial.println("ESP32-S3 N16R8 + ICM-20948 MQTT demo");
  Serial.printf("SDA=GPIO%u, SCL=GPIO%u, expected IMU address=",
                I2C_SDA_PIN,
                I2C_SCL_PIN);
  printHexAddress(ICM_EXPECTED_ADDR);
  Serial.println();

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(I2C_CLOCK_HZ);

  scanI2CBus();
  initializeImu();

  setupWifiAndConfigPortal();
  configureMqttClient();
}

void loop()
{
  handleConfigButton();

  if (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    return;
  }

  connectMqtt();
  mqttClient.loop();
  publishImuSample();
}
