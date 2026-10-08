// =====================================================
// NODE CONFIGURATION AND COMMUNICATION PROTOCOL
// =====================================================
/*
Defines the hardware pins, monitoring timings and message formats shared with
the ESP32 gateway.

The enums and packed packet structures give both devices a common representation
for pairing, sensor readings, configuration updates and learning commands.
*/

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <Preferences.h>

#include <OneWire.h>
#include <DallasTemperature.h>
#include <Adafruit_NeoPixel.h>

#define TEMP_PIN D9
#define LED_PIN D7
#define DOOR_PIN D8

#define LED_COUNT 1

#define REPAIR_BUTTON_PIN 0
#define REPAIR_HOLD_MS 3000

#define DOOR_GPIO GPIO_NUM_7

#define FAST_INTERVAL_SECONDS 30
#define MEDIUM_INTERVAL_SECONDS 60
#define STABLE_INTERVAL_SECONDS 90

#define DOOR_CHECK_INTERVAL_SECONDS 5
#define DOOR_WARNING_SECONDS 30

#define TEMP_FAST_CHANGE 1.0
#define TEMP_MEDIUM_CHANGE 0.3

#define PAIRING_DURATION_MS 15000
#define PAIRING_RETRY_MS 1000
#define UNPAIRED_SLEEP_SECONDS 30

#define PROTOCOL_VERSION 1

#define FRIDGE_MANAGER_MAGIC 0x464D4752

enum MessageType : uint8_t {
  MSG_PAIR_REQUEST = 1,
  MSG_PAIR_RESPONSE = 2,
  MSG_SENSOR_DATA = 3,
  MSG_SENSOR_CONFIG = 4,
  MSG_CONFIG_ACK = 5,
  MSG_LEARNING_COMMAND = 6
};

enum Compartment : uint8_t {
  COMPARTMENT_UNASSIGNED = 0,
  COMPARTMENT_FRIDGE = 1,
  COMPARTMENT_FREEZER = 2
};

enum TemperatureState : uint8_t {
  TEMP_STATE_NORMAL = 0,
  TEMP_STATE_TRANSIENT = 1,
  TEMP_STATE_WARNING = 2,
  TEMP_STATE_RECOVERY = 3
};

enum LearningMode : uint8_t {
  LEARNING_MODE_NONE = 0,
  LEARNING_MODE_FAST = 1,
  LEARNING_MODE_24H = 2,
  LEARNING_MODE_48H = 3
};

#define LEARNING_FAST_SECONDS (2UL * 60UL * 60UL)
#define LEARNING_24H_SECONDS (24UL * 60UL * 60UL)
#define LEARNING_48H_SECONDS (48UL * 60UL * 60UL)

// =====================================================
// NODE STATE
// =====================================================
/*
Holds the node's hardware interfaces, saved configuration and current operating state.

Normal variables contain settings that are restored from Preferences after boot,
while RTC_DATA_ATTR values preserve short-term monitoring and learning state
across deep-sleep wake cycles.
*/

OneWire oneWire(TEMP_PIN);

DallasTemperature sensors(
  &oneWire);

Adafruit_NeoPixel statusLED(
  LED_COUNT,
  LED_PIN,
  NEO_GRB + NEO_KHZ800);

Preferences preferences;

uint32_t configVersion = 0;

uint8_t sensorCompartment =
  COMPARTMENT_UNASSIGNED;

float minTemperature = 1.0;
float maxTemperature = 5.0;

float temperatureHysteresis = 1.0;

uint32_t temperatureWarningSeconds = 900;
uint32_t temperatureRecoverySeconds = 300;

uint32_t doorWarningSeconds = 30;

bool sensorEnabled = true;

uint8_t learningMode =
  LEARNING_MODE_FAST;

bool learningTrained = false;

float learnedAverageTemperature = 0.0;
float learnedMinimumTemperature = 0.0;
float learnedMaximumTemperature = 0.0;

uint32_t learnedSampleCount = 0;

uint8_t nodeMAC[6];
uint8_t gatewayMAC[6];

const uint8_t broadcastMAC[6] = {
  0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF
};

bool gatewayPaired = false;

// RTC memory survives deep sleep, unlike normal RAM. These values let each
// wake cycle continue timing door, temperature and learning behaviour.
RTC_DATA_ATTR bool doorWasOpen = false;

RTC_DATA_ATTR uint32_t doorOpenSeconds = 0;

RTC_DATA_ATTR float previousTemperature = 0.0;

RTC_DATA_ATTR bool previousTemperatureValid = false;

RTC_DATA_ATTR uint8_t temperatureState =
  TEMP_STATE_NORMAL;

RTC_DATA_ATTR uint32_t temperatureOutOfRangeSeconds = 0;

RTC_DATA_ATTR uint32_t temperatureRecoveryElapsedSeconds = 0;

RTC_DATA_ATTR uint32_t previousSleepSeconds = 0;

RTC_DATA_ATTR bool learningActive = false;

RTC_DATA_ATTR uint32_t learningElapsedSeconds = 0;

RTC_DATA_ATTR uint32_t learningSampleCount = 0;

RTC_DATA_ATTR float learningTemperatureSum = 0.0;

RTC_DATA_ATTR float learningMinimumTemperature = 100.0;

RTC_DATA_ATTR float learningMaximumTemperature = -100.0;

// packed prevents compiler padding between fields so both ESP32 devices
// interpret the transmitted ESP-NOW bytes using the same packet layout.
struct __attribute__((packed)) PairRequest {
  uint32_t magic;
  uint8_t messageType;
  uint8_t protocolVersion;
};

struct __attribute__((packed)) PairResponse {
  uint32_t magic;
  uint8_t messageType;
  uint8_t protocolVersion;
};

struct __attribute__((packed)) SensorPacket {
  uint32_t magic;
  uint8_t messageType;
  float temperature;
  bool doorOpen;
  bool doorWarning;
  uint8_t wakeReason;
  uint8_t temperatureState;
  uint8_t learningMode;
  bool learningActive;
  bool learningTrained;
  uint32_t learningElapsedSeconds;
  uint32_t learningSampleCount;
  float learnedMinimumTemperature;
  float learnedMaximumTemperature;
};

struct __attribute__((packed)) SensorConfigPacket {
  uint32_t magic;
  uint8_t messageType;
  uint8_t protocolVersion;
  uint32_t configVersion;
  uint8_t compartment;
  float minTemperature;
  float maxTemperature;
  float temperatureHysteresis;
  uint32_t temperatureWarningSeconds;
  uint32_t temperatureRecoverySeconds;
  uint32_t doorWarningSeconds;
  bool enabled;
};

struct __attribute__((packed)) ConfigAckPacket {
  uint32_t magic;
  uint8_t messageType;
  uint8_t protocolVersion;
  uint32_t configVersion;
};

struct __attribute__((packed)) LearningCommandPacket {
  uint32_t magic;
  uint8_t messageType;
  uint8_t protocolVersion;
  uint8_t command;
  uint8_t mode;
};

// These flags are changed by ESP-NOW callbacks outside the normal sequential
// flow, so volatile prevents the compiler from assuming their values stay fixed.
volatile bool sendComplete = false;
volatile bool sendSuccessful = false;
volatile bool pairingResponseReceived = false;

/*
Acknowledges the configuration version currently applied by the node.
*/
void sendConfigAck();

/*
Starts a new learning session in the requested mode while retaining any previously
completed profile until the new training run finishes successfully.
*/
void startLearning(
  uint8_t mode);

/*
Stops active learning and clears temporary training data without deleting the
previously completed learned profile.
*/
void cancelLearning();

// =====================================================
// HARDWARE AND STATUS HELPERS
// =====================================================
/*
Provides small helpers used throughout the node for readable MAC output, RGB
status indication and selecting the duration of the active learning mode.
*/

/*
Prints a MAC address in the standard readable format used in Serial diagnostics.
*/
void printMAC(
  const uint8_t *mac) {

  for (int i = 0; i < 6; i++) {

    if (i > 0) {
      Serial.print(":");
    }

    if (mac[i] < 0x10) {
      Serial.print("0");
    }

    Serial.print(
      mac[i],
      HEX);
  }
}

/*
Sets the node's WS2812B status LED to the requested RGB colour.
*/
void setLED(
  uint8_t red,
  uint8_t green,
  uint8_t blue) {

  statusLED.setPixelColor(
    0,
    statusLED.Color(
      red,
      green,
      blue));

  statusLED.show();
}

// Turns off the node's status LED.
void clearLED() {

  statusLED.clear();
  statusLED.show();
}

/*
Briefly flashes the status LED to provide local feedback without leaving it on.
*/
void flashLED(
  uint8_t red,
  uint8_t green,
  uint8_t blue) {

  setLED(
    red,
    green,
    blue);

  delay(150);

  clearLED();
}

/*
Returns the training duration associated with the currently selected learning mode.
*/
uint32_t getLearningDurationSeconds() {

  switch (learningMode) {

    case LEARNING_MODE_24H:
      return LEARNING_24H_SECONDS;

    case LEARNING_MODE_48H:
      return LEARNING_48H_SECONDS;

    case LEARNING_MODE_FAST:
    default:
      return LEARNING_FAST_SECONDS;
  }
}

// =====================================================
// PERSISTENT NODE STORAGE
// =====================================================
/*
Loads and saves information that must survive power loss or a full restart.

ESP32 Preferences stores the learned temperature profile, paired gateway identity
and sensor configuration, while active wake-to-wake counters remain in RTC memory.
*/

// Restores the completed learning profile. Active learning progress itself is
// kept in RTC memory because it only needs to survive deep-sleep wake cycles.
/*
Restores the last completed learned-temperature profile from persistent NVS storage.
*/
void loadLearningFromNVS() {

  preferences.begin(
    "fridgeMgr",
    true);

  learningMode =
    preferences.getUChar(
      "learnMode",
      LEARNING_MODE_FAST);

  learningTrained =
    preferences.getBool(
      "learned",
      false);

  learnedAverageTemperature =
    preferences.getFloat(
      "learnAvg",
      0.0);

  learnedMinimumTemperature =
    preferences.getFloat(
      "learnMin",
      0.0);

  learnedMaximumTemperature =
    preferences.getFloat(
      "learnMax",
      0.0);

  learnedSampleCount =
    preferences.getUInt(
      "learnSamples",
      0);

  preferences.end();

  Serial.print(
    "Learning profile: ");

  Serial.println(
    learningTrained
      ? "TRAINED"
      : "NOT TRAINED");

  Serial.print(
    "Learning mode: ");

  Serial.println(
    learningMode);

  if (learningTrained) {

    Serial.print(
      "Learned minimum: ");

    Serial.println(
      learnedMinimumTemperature);

    Serial.print(
      "Learned maximum: ");

    Serial.println(
      learnedMaximumTemperature);
  }
}

/*
Saves the completed learned-temperature profile so it survives a full restart.
*/
void saveLearningToNVS() {

  preferences.begin(
    "fridgeMgr",
    false);

  preferences.putUChar(
    "learnMode",
    learningMode);

  preferences.putBool(
    "learned",
    learningTrained);

  preferences.putFloat(
    "learnAvg",
    learnedAverageTemperature);

  preferences.putFloat(
    "learnMin",
    learnedMinimumTemperature);

  preferences.putFloat(
    "learnMax",
    learnedMaximumTemperature);

  preferences.putUInt(
    "learnSamples",
    learnedSampleCount);

  preferences.end();

  Serial.println(
    "Learning profile saved to NVS");
}

// The saved gateway MAC is the node's persistent trust relationship. Once
// paired, normal configuration/learning commands are accepted only from it.
/*
Restores the previously paired gateway MAC so normal wakes do not require re-pairing.
Returns true when a valid stored gateway was found.
*/
bool loadGatewayFromNVS() {

  preferences.begin(
    "fridgeMgr",
    true);

  bool paired =
    preferences.getBool(
      "paired",
      false);

  if (paired) {

    size_t macLength =
      preferences.getBytesLength(
        "gateway");

    if (macLength == 6) {

      preferences.getBytes(
        "gateway",
        gatewayMAC,
        6);
    }

    else {
      paired = false;
    }
  }

  preferences.end();

  gatewayPaired = paired;

  return paired;
}

/*
Stores the approved gateway MAC in NVS after successful pairing.
*/
void saveGatewayToNVS(
  const uint8_t *mac) {

  memcpy(
    gatewayMAC,
    mac,
    6);

  preferences.begin(
    "fridgeMgr",
    false);

  preferences.putBytes(
    "gateway",
    gatewayMAC,
    6);

  preferences.putBool(
    "paired",
    true);

  preferences.end();

  gatewayPaired = true;

  Serial.println(
    "Gateway saved to NVS");
}

/*
Removes the stored gateway relationship so the node can be paired again.
*/
void clearGatewayFromNVS() {

  preferences.begin(
    "fridgeMgr",
    false);

  preferences.remove(
    "gateway");

  preferences.putBool(
    "paired",
    false);

  preferences.end();

  memset(
    gatewayMAC,
    0,
    6);

  gatewayPaired = false;

  Serial.println(
    "Gateway pairing cleared");
}

// Configuration comes from the Pi through the gateway, but is stored locally
// so thresholds and timings remain available when the node is offline/asleep.
/*
Restores the node's last saved thresholds, timings, compartment and enabled state.
*/
void loadConfigFromNVS() {

  preferences.begin(
    "fridgeMgr",
    true);

  configVersion =
    preferences.getUInt(
      "cfgVer",
      0);

  sensorCompartment =
    preferences.getUChar(
      "compartment",
      COMPARTMENT_UNASSIGNED);

  minTemperature =
    preferences.getFloat(
      "minTemp",
      1.0);

  maxTemperature =
    preferences.getFloat(
      "maxTemp",
      5.0);

  temperatureHysteresis =
    preferences.getFloat(
      "tempHyst",
      1.0);

  temperatureWarningSeconds =
    preferences.getUInt(
      "tempWarn",
      900);

  temperatureRecoverySeconds =
    preferences.getUInt(
      "tempRecover",
      300);

  doorWarningSeconds =
    preferences.getUInt(
      "doorWarn",
      30);

  sensorEnabled =
    preferences.getBool(
      "enabled",
      true);

  preferences.end();

  Serial.print(
    "Config version: ");

  Serial.println(
    configVersion);
}

/*
Applies a received sensor configuration and stores it persistently on the node.
*/
void saveConfigToNVS(
  const SensorConfigPacket &config) {

  preferences.begin(
    "fridgeMgr",
    false);

  preferences.putUInt(
    "cfgVer",
    config.configVersion);

  preferences.putUChar(
    "compartment",
    config.compartment);

  preferences.putFloat(
    "minTemp",
    config.minTemperature);

  preferences.putFloat(
    "maxTemp",
    config.maxTemperature);

  preferences.putFloat(
    "tempHyst",
    config.temperatureHysteresis);

  preferences.putUInt(
    "tempWarn",
    config.temperatureWarningSeconds);

  preferences.putUInt(
    "tempRecover",
    config.temperatureRecoverySeconds);

  preferences.putUInt(
    "doorWarn",
    config.doorWarningSeconds);

  preferences.putBool(
    "enabled",
    config.enabled);

  preferences.end();

  configVersion =
    config.configVersion;

  sensorCompartment =
    config.compartment;

  minTemperature =
    config.minTemperature;

  maxTemperature =
    config.maxTemperature;

  temperatureHysteresis =
    config.temperatureHysteresis;

  temperatureWarningSeconds =
    config.temperatureWarningSeconds;

  temperatureRecoverySeconds =
    config.temperatureRecoverySeconds;

  doorWarningSeconds =
    config.doorWarningSeconds;

  sensorEnabled =
    config.enabled;

  Serial.print(
    "Saved config version: ");

  Serial.println(
    configVersion);
}

// =====================================================
// PAIRING AND ESP-NOW COMMUNICATION
// =====================================================
/*
Manages the node's trusted gateway relationship and ESP-NOW connection.

A long BOOT-button hold clears the saved gateway, unpaired nodes broadcast pairing
requests, and received packets are accepted only when their protocol and sender
match the expected Fridge Manager gateway.
*/

// Re-pairing is deliberately physical: BOOT must be held long enough to clear
// the stored gateway, preventing an ordinary wake from discarding trust state.
/*
Checks for the physical BOOT-button re-pair gesture.
Returns true when the saved gateway has been cleared.
*/
bool checkManualRepair() {

  pinMode(
    REPAIR_BUTTON_PIN,
    INPUT_PULLUP);

  Serial.println(
    "Hold BOOT for 3 seconds to re-pair...");

  unsigned long checkStart =
    millis();

  while (
    millis() - checkStart < 5000) {

    if (
      digitalRead(REPAIR_BUTTON_PIN)
      == LOW) {

      unsigned long heldSince =
        millis();

      Serial.println(
        "BOOT button pressed...");

      while (
        digitalRead(REPAIR_BUTTON_PIN)
        == LOW) {

        if (
          millis() - heldSince >= REPAIR_HOLD_MS) {

          Serial.println(
            "Manual re-pair requested");

          clearGatewayFromNVS();

          flashLED(
            0,
            0,
            150);

          return true;
        }

        delay(20);
      }
    }

    delay(20);
  }

  return false;
}

// ESP-NOW reports transmission completion asynchronously through this callback.
// sendSensorPacket() waits on these flags for up to one second before sleeping.
/*
ESP-NOW callback that records whether an asynchronous node transmission completed.
*/
void onDataSent(
  const wifi_tx_info_t *info,
  esp_now_send_status_t status) {

  sendSuccessful =
    (status == ESP_NOW_SEND_SUCCESS);

  sendComplete = true;
}

// All incoming ESP-NOW traffic enters here. The magic value identifies Fridge
// Manager traffic, then messageType determines which packet format is expected.
/*
Receives ESP-NOW messages and routes valid pairing, configuration and learning
commands from the gateway to the appropriate node logic.
*/
void onDataReceived(
  const esp_now_recv_info_t *info,
  const uint8_t *data,
  int len) {

  if (len < 5) {
    return;
  }

  uint32_t magic;

  memcpy(
    &magic,
    data,
    sizeof(magic));

  if (
    magic != FRIDGE_MANAGER_MAGIC) {
    return;
  }

  uint8_t messageType =
    data[4];

  // A pair response is only useful while no gateway is currently trusted.
  if (
    messageType == MSG_PAIR_RESPONSE) {

    if (
      len != sizeof(PairResponse)) {
      return;
    }

    PairResponse response;

    memcpy(
      &response,
      data,
      sizeof(response));

    if (
      response.protocolVersion != PROTOCOL_VERSION) {
      return;
    }

    if (gatewayPaired) {
      return;
    }

    Serial.println();
    Serial.println(
      "Pair response received!");

    Serial.print(
      "Gateway MAC: ");

    printMAC(
      info->src_addr);

    Serial.println();

    saveGatewayToNVS(
      info->src_addr);

    pairingResponseReceived = true;

    return;
  }

  // Configuration is accepted only from the MAC saved during pairing.
  if (
    messageType == MSG_SENSOR_CONFIG) {

    if (
      len != sizeof(SensorConfigPacket)) {
      return;
    }

    if (!gatewayPaired) {
      return;
    }

    if (
      memcmp(
        info->src_addr,
        gatewayMAC,
        6)
      != 0) {

      Serial.println(
        "Config rejected: unknown sender");

      return;
    }

    SensorConfigPacket config;

    memcpy(
      &config,
      data,
      sizeof(config));

    if (
      config.protocolVersion != PROTOCOL_VERSION) {
      return;
    }

    if (
      config.configVersion > configVersion) {

      saveConfigToNVS(
        config);
    }

    Serial.print(
      "Config received. Version: ");

    Serial.println(
      config.configVersion);

    sendConfigAck();

    return;
  }

  // Learning is executed on this node; the gateway only forwards the command.
  if (
    messageType == MSG_LEARNING_COMMAND) {

    if (
      len != sizeof(LearningCommandPacket)) {
      return;
    }

    if (!gatewayPaired) {
      return;
    }

    if (
      memcmp(
        info->src_addr,
        gatewayMAC,
        6)
      != 0) {

      Serial.println(
        "Learning command rejected: unknown sender");

      return;
    }

    LearningCommandPacket command;

    memcpy(
      &command,
      data,
      sizeof(command));

    if (
      command.protocolVersion != PROTOCOL_VERSION) {
      return;
    }

    if (
      command.command == 1) {

      startLearning(
        command.mode);
    }

    else if (
      command.command == 2) {

      cancelLearning();
    }

    return;
  }
}

// The ACK returns the node's current configVersion. This lets the gateway/Pi
// distinguish a configuration merely sent from one actually applied by the node.
void sendConfigAck() {

  if (!gatewayPaired) {
    return;
  }

  ConfigAckPacket ack;

  ack.magic =
    FRIDGE_MANAGER_MAGIC;

  ack.messageType =
    MSG_CONFIG_ACK;

  ack.protocolVersion =
    PROTOCOL_VERSION;

  ack.configVersion =
    configVersion;

  esp_now_send(
    gatewayMAC,
    reinterpret_cast<uint8_t *>(
      &ack),
    sizeof(ack));

  Serial.print(
    "Config ACK sent. Version: ");

  Serial.println(
    configVersion);
}

// ESP-NOW requires a destination in its peer table before direct transmission.
// Link encryption is disabled here; sender trust is enforced separately by MAC.
/*
Ensures the destination exists in the ESP-NOW peer table before transmission.
Returns whether the peer is available for communication.
*/
bool addPeer(
  const uint8_t *mac) {

  if (
    esp_now_is_peer_exist(mac)) {
    return true;
  }

  esp_now_peer_info_t peerInfo = {};

  memcpy(
    peerInfo.peer_addr,
    mac,
    6);

  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  if (
    esp_now_add_peer(
      &peerInfo)
    != ESP_OK) {

    Serial.println(
      "Failed to add ESP-NOW peer");

    return false;
  }

  return true;
}

/*
Initialises Wi-Fi station mode and ESP-NOW communication for the sensor node.
Returns whether ESP-NOW initialisation succeeded.
*/
bool setupEspNow() {

  WiFi.mode(
    WIFI_STA);

  delay(100);

  esp_wifi_get_mac(
    WIFI_IF_STA,
    nodeMAC);

  Serial.print(
    "Node MAC: ");

  printMAC(
    nodeMAC);

  Serial.println();

  if (
    esp_now_init() != ESP_OK) {

    Serial.println(
      "ESP-NOW initialization failed");

    return false;
  }

  esp_now_register_send_cb(
    onDataSent);

  esp_now_register_recv_cb(
    onDataReceived);

  return true;
}

/*
Broadcasts a pairing request so a gateway in pairing mode can discover this node.
*/
void sendPairRequest() {

  PairRequest request;

  request.magic =
    FRIDGE_MANAGER_MAGIC;

  request.messageType =
    MSG_PAIR_REQUEST;

  request.protocolVersion =
    PROTOCOL_VERSION;

  sendComplete = false;
  sendSuccessful = false;

  esp_now_send(
    broadcastMAC,
    reinterpret_cast<uint8_t *>(
      &request),
    sizeof(request));

  Serial.println(
    "Pair request broadcast");
}

// While unpaired, the node repeatedly broadcasts requests for a limited period.
// The gateway controls whether an unknown node is actually approved/trusted.
/*
Runs the timed pairing process, repeatedly advertising until a gateway responds
or the pairing window expires. Returns whether pairing succeeded.
*/
bool startPairing() {

  Serial.println();
  Serial.println(
    "No gateway stored.");

  Serial.println(
    "Starting automatic pairing...");

  if (
    !addPeer(
      broadcastMAC)) {
    return false;
  }

  pairingResponseReceived = false;

  unsigned long startTime =
    millis();

  unsigned long lastRequest =
    0;

  setLED(
    0,
    0,
    80);

  while (
    millis() - startTime < PAIRING_DURATION_MS) {

    if (
      pairingResponseReceived) {

      clearLED();

      flashLED(
        0,
        120,
        0);

      Serial.println(
        "Pairing successful");

      Serial.print(
        "Stored gateway: ");

      printMAC(
        gatewayMAC);

      Serial.println();

      return true;
    }

    if (
      millis() - lastRequest >= PAIRING_RETRY_MS
      || lastRequest == 0) {

      sendPairRequest();

      lastRequest =
        millis();
    }

    delay(10);
  }

  clearLED();

  Serial.println(
    "No gateway found");

  return false;
}

// =====================================================
// SENSOR REPORTING
// =====================================================
/*
Builds the node's current sensor and learning state into a SensorPacket and
sends it to the paired gateway.

The node waits briefly for the ESP-NOW send callback so it can determine whether
the report was transmitted successfully before continuing toward deep sleep.
*/

/*
Packages the current temperature, door, edge-state and learning information and
sends it to the paired gateway. Returns whether the ESP-NOW send succeeded.
*/
bool sendSensorPacket(
  float temperature,
  bool doorOpen,
  bool doorWarning,
  uint8_t wakeReason) {

  if (!gatewayPaired) {

    Serial.println(
      "Cannot send: node is unpaired");

    return false;
  }

  if (
    !addPeer(
      gatewayMAC)) {
    return false;
  }

  SensorPacket packet;

  packet.magic =
    FRIDGE_MANAGER_MAGIC;

  packet.messageType =
    MSG_SENSOR_DATA;

  packet.temperature =
    temperature;

  packet.doorOpen =
    doorOpen;

  packet.doorWarning =
    doorWarning;

  packet.wakeReason =
    wakeReason;

  packet.temperatureState =
    temperatureState;

  packet.learningMode =
    learningMode;

  packet.learningActive =
    learningActive;

  packet.learningTrained =
    learningTrained;

  packet.learningElapsedSeconds =
    learningElapsedSeconds;

  // While training, report live sample progress; otherwise report the number
  // of samples belonging to the most recently completed learned profile.
  if (learningActive) {

    packet.learningSampleCount =
      learningSampleCount;
  }

  else {

    packet.learningSampleCount =
      learnedSampleCount;
  }

  packet.learnedMinimumTemperature =
    learnedMinimumTemperature;

  packet.learnedMaximumTemperature =
    learnedMaximumTemperature;

  sendComplete = false;
  sendSuccessful = false;

  esp_err_t result =
    esp_now_send(
      gatewayMAC,
      reinterpret_cast<uint8_t *>(
        &packet),
      sizeof(packet));

  if (
    result != ESP_OK) {

    Serial.println(
      "ESP-NOW send request failed");

    return false;
  }

  unsigned long start =
    millis();

  while (
    !sendComplete &&
    millis() - start < 1000) {

    delay(1);
  }

  if (!sendComplete) {

    Serial.println(
      "ESP-NOW send timeout");

    return false;
  }

  Serial.print(
    "ESP-NOW send: ");

  Serial.println(
    sendSuccessful
      ? "SUCCESS"
      : "FAILED");

  return sendSuccessful;
}

// =====================================================
// WAKE AND SLEEP MANAGEMENT
// =====================================================
/*
Identifies why the node woke and prepares the next low-power sleep cycle.

Timer wakeups support periodic monitoring, while the reed-switch GPIO can wake a
closed-door node immediately when the door opens.
*/

/*
Converts the ESP32 wake cause into the compact wake code reported by Fridge Manager.
*/
uint8_t getWakeReason() {

  esp_sleep_wakeup_cause_t cause =
    esp_sleep_get_wakeup_cause();

  switch (cause) {

    case ESP_SLEEP_WAKEUP_TIMER:
      return 1;

    case ESP_SLEEP_WAKEUP_EXT1:
      return 2;

    default:
      return 0;
  }
}

// Prints a readable description of the node's wake reason.
void printWakeReason(
  uint8_t wakeReason) {

  Serial.print(
    "Wake reason: ");

  switch (wakeReason) {

    case 1:
      Serial.println(
        "TIMER");
      break;

    case 2:
      Serial.println(
        "DOOR");
      break;

    default:
      Serial.println(
        "POWER RESET");
      break;
  }
}

// =====================================================
// EDGE TEMPERATURE MONITORING
// =====================================================
/*
Runs the node's local rule-based temperature state machine without relying on
the Raspberry Pi for each decision.

Out-of-range readings must persist before becoming warnings, and hysteresis plus
a timed recovery period prevent short temperature changes from repeatedly
triggering and clearing alerts.
*/

/*
Runs the local temperature state machine using persistence and hysteresis to avoid
turning short temperature fluctuations into immediate warnings.
*/
void updateTemperatureState(
  float temperature,
  bool doorOpen,
  bool sensorFault,
  uint32_t elapsedSeconds) {

  if (
    sensorFault ||
    !sensorEnabled) {

    temperatureState =
      TEMP_STATE_NORMAL;

    temperatureOutOfRangeSeconds =
      0;

    temperatureRecoveryElapsedSeconds =
      0;

    return;
  }

  // The configured limits detect an excursion; the tighter hysteresis band
  // is used separately to decide when conditions are genuinely recovered.
  bool tooCold =
    temperature < minTemperature;

  bool tooWarm =
    temperature > maxTemperature;

  bool outOfRange =
    tooCold || tooWarm;

  bool recovered =
    temperature >=
      (minTemperature + temperatureHysteresis)
    &&
    temperature <=
      (maxTemperature - temperatureHysteresis);

  switch (temperatureState) {

    case TEMP_STATE_NORMAL:

      if (outOfRange) {

        temperatureState =
          TEMP_STATE_TRANSIENT;

        temperatureOutOfRangeSeconds =
          0;
      }

      break;

    // TRANSIENT prevents a brief compressor/door-related excursion from
    // immediately becoming a temperature warning.
    case TEMP_STATE_TRANSIENT:

      if (!outOfRange) {

        temperatureState =
          TEMP_STATE_NORMAL;

        temperatureOutOfRangeSeconds =
          0;
      }

      else {

        temperatureOutOfRangeSeconds +=
          elapsedSeconds;

        if (
          temperatureOutOfRangeSeconds >=
          temperatureWarningSeconds) {

          temperatureState =
            TEMP_STATE_WARNING;

          temperatureRecoveryElapsedSeconds =
            0;
        }
      }

      break;

    case TEMP_STATE_WARNING:

      if (recovered) {

        temperatureState =
          TEMP_STATE_RECOVERY;

        temperatureRecoveryElapsedSeconds =
          0;
      }

      break;

    // RECOVERY requires stable temperature for the configured recovery time
    // before clearing the event back to NORMAL.
    case TEMP_STATE_RECOVERY:

      if (outOfRange) {

        temperatureState =
          TEMP_STATE_WARNING;

        temperatureRecoveryElapsedSeconds =
          0;
      }

      else if (recovered) {

        temperatureRecoveryElapsedSeconds +=
          elapsedSeconds;

        if (
          temperatureRecoveryElapsedSeconds >=
          temperatureRecoverySeconds) {

          temperatureState =
            TEMP_STATE_NORMAL;

          temperatureOutOfRangeSeconds =
            0;

          temperatureRecoveryElapsedSeconds =
            0;
        }
      }

      else {

        

        

        

        temperatureRecoveryElapsedSeconds =
          0;
      }

      break;
  }

  Serial.print(
    "Edge temperature state: ");

  switch (temperatureState) {

    case TEMP_STATE_NORMAL:
      Serial.println(
        "NORMAL");
      break;

    case TEMP_STATE_TRANSIENT:
      Serial.println(
        "TRANSIENT");
      break;

    case TEMP_STATE_WARNING:
      Serial.println(
        "WARNING");
      break;

    case TEMP_STATE_RECOVERY:
      Serial.println(
        "RECOVERY");
      break;
  }

  if (doorOpen) {

    Serial.println(
      "Edge context: door open");
  }
}

// =====================================================
// TEMPERATURE LEARNING
// =====================================================
/*
Builds a temperature profile locally on the sensor node over the selected
learning period.

Only valid readings taken while the door is closed are sampled. Completed
minimum, maximum and average values are saved to persistent storage, while
starting or cancelling relearning retains any previously completed profile.
*/

/*
Advances an active learning session and collects valid closed-door temperature
samples for the node's learned operating profile.
*/
void updateLearning(
  float temperature,
  bool doorOpen,
  bool sensorFault,
  uint32_t elapsedSeconds) {

  if (!learningActive) {
    return;
  }

  // Elapsed time advances using the previous timer-sleep duration. Door wakeups
  // do not pretend that an entire scheduled sleep interval has elapsed.
  learningElapsedSeconds +=
    elapsedSeconds;

  // Door-open and faulty readings are excluded so they do not distort the learned profile.
  if (
    !sensorFault &&
    !doorOpen) {

    learningTemperatureSum +=
      temperature;

    learningSampleCount++;

    if (
      temperature <
      learningMinimumTemperature) {

      learningMinimumTemperature =
        temperature;
    }

    if (
      temperature >
      learningMaximumTemperature) {

      learningMaximumTemperature =
        temperature;
    }
  }

  Serial.print(
    "Learning elapsed: ");

  Serial.print(
    learningElapsedSeconds);

  Serial.print(" / ");

  Serial.print(
    getLearningDurationSeconds());

  Serial.println(
    " seconds");

  Serial.print(
    "Learning samples: ");

  Serial.println(
    learningSampleCount);

  if (
    learningElapsedSeconds >=
    getLearningDurationSeconds()) {

    learningActive =
      false;

    if (
      learningSampleCount > 0) {

      // Promote the temporary training statistics into the completed profile.
      learnedAverageTemperature =
        learningTemperatureSum /
        learningSampleCount;

      learnedMinimumTemperature =
        learningMinimumTemperature;

      learnedMaximumTemperature =
        learningMaximumTemperature;

      learnedSampleCount =
        learningSampleCount;

      learningTrained =
        true;

      saveLearningToNVS();

      Serial.println(
        "Learning complete");

      Serial.print(
        "Learned average: ");

      Serial.println(
        learnedAverageTemperature);

      Serial.print(
        "Learned minimum: ");

      Serial.println(
        learnedMinimumTemperature);

      Serial.print(
        "Learned maximum: ");

      Serial.println(
        learnedMaximumTemperature);
    }

    else {

      Serial.println(
        "Learning failed: no valid samples");

      if (learningTrained) {

        Serial.println(
          "Previous learned profile retained");
      }
    }
  }
}

void startLearning(
  uint8_t mode) {

  if (
    mode != LEARNING_MODE_FAST &&
    mode != LEARNING_MODE_24H &&
    mode != LEARNING_MODE_48H) {

    mode =
      LEARNING_MODE_FAST;
  }

  learningMode =
    mode;

  learningActive =
    true;

  learningElapsedSeconds =
    0;

  learningSampleCount =
    0;

  learningTemperatureSum =
    0.0;

  learningMinimumTemperature =
    100.0;

  learningMaximumTemperature =
    -100.0;

  saveLearningToNVS();

  Serial.print(
    "Learning started. Mode: ");

  Serial.println(
    learningMode);

  Serial.print(
    "Learning duration: ");

  Serial.print(
    getLearningDurationSeconds());

  Serial.println(
    " seconds");

  if (learningTrained) {

    Serial.println(
      "Previous learned profile retained during relearning");
  }
}

void cancelLearning() {

  learningActive =
    false;

  learningElapsedSeconds =
    0;

  learningSampleCount =
    0;

  learningTemperatureSum =
    0.0;

  learningMinimumTemperature =
    100.0;

  learningMaximumTemperature =
    -100.0;

  Serial.println(
    "Learning cancelled");

  if (learningTrained) {

    Serial.println(
      "Previous learned profile retained");
  }
}

// =====================================================
// ADAPTIVE MONITORING
// =====================================================
/*
Selects how soon the node should wake again based on current conditions.

Door activity, sensor faults and temperature events use faster monitoring, while
stable temperatures allow longer sleep intervals to reduce battery consumption.
*/

/*
Chooses the next monitoring interval from door state, sensor health, edge state
and temperature rate of change. Returns the selected sleep time in seconds.
*/
uint32_t calculateSleepInterval(
  float currentTemperature,
  bool doorOpen,
  bool sensorFault) {

  // Door activity has highest priority because the node must re-check quickly
  // enough to build the open-duration warning while the door remains open.
  if (doorOpen) {

    Serial.println(
      "Monitoring mode: DOOR OPEN");

    return DOOR_CHECK_INTERVAL_SECONDS;
  }

  if (sensorFault) {

    Serial.println(
      "Monitoring mode: SENSOR FAULT");

    return FAST_INTERVAL_SECONDS;
  }

  // Any active edge-temperature state keeps sampling fast until it settles.
  if (
    temperatureState !=
    TEMP_STATE_NORMAL) {

    Serial.println(
      "Monitoring mode: EDGE EVENT");

    return FAST_INTERVAL_SECONDS;
  }

  if (
    !previousTemperatureValid) {

    Serial.println(
      "Monitoring mode: INITIAL");

    return FAST_INTERVAL_SECONDS;
  }

  // When conditions are otherwise normal, rate of change decides whether the
  // node can safely extend its sleep interval to save battery.
  float temperatureChange =
    fabs(
      currentTemperature -
      previousTemperature);

  Serial.print(
    "Previous temperature: ");

  Serial.print(
    previousTemperature);

  Serial.println(" C");

  Serial.print(
    "Temperature change: ");

  Serial.print(
    temperatureChange);

  Serial.println(" C");

  if (
    temperatureChange >=
    TEMP_FAST_CHANGE) {

    Serial.println(
      "Monitoring mode: FAST");

    return FAST_INTERVAL_SECONDS;
  }

  if (
    temperatureChange >=
    TEMP_MEDIUM_CHANGE) {

    Serial.println(
      "Monitoring mode: MEDIUM");

    return MEDIUM_INTERVAL_SECONDS;
  }

  Serial.println(
    "Monitoring mode: STABLE");

  return STABLE_INTERVAL_SECONDS;
}

/*
Configures timer and door wake sources, then places the battery node into deep sleep.
*/
void enterDeepSleep(
  uint32_t seconds) {

  Serial.print(
    "Sleeping for ");

  Serial.print(
    seconds);

  Serial.println(
    " seconds");

  bool doorCurrentlyOpen =
    digitalRead(
      DOOR_PIN)
    == HIGH;

  Serial.print(
    "Door state before sleep: ");

  Serial.println(
    doorCurrentlyOpen
      ? "OPEN"
      : "CLOSED");

  rtc_gpio_init(
    DOOR_GPIO);

  rtc_gpio_set_direction(
    DOOR_GPIO,
    RTC_GPIO_MODE_INPUT_ONLY);

  rtc_gpio_pullup_en(
    DOOR_GPIO);

  rtc_gpio_pulldown_dis(
    DOOR_GPIO);

  esp_sleep_pd_config(
    ESP_PD_DOMAIN_RTC_PERIPH,
    ESP_PD_OPTION_ON);

  // EXT1 is armed only while closed; an open door is instead checked frequently by timer.
  // With the door closed, a rising reed-switch signal can wake the ESP32 before
  // its timer expires. If already open, timer wakes avoid an immediate wake loop.
  if (!doorCurrentlyOpen) {

    esp_sleep_enable_ext1_wakeup(
      1ULL << DOOR_GPIO,
      ESP_EXT1_WAKEUP_ANY_HIGH);

    Serial.println(
      "Door wake enabled");
  }

  else {

    Serial.println(
      "Door open - EXT1 disabled");

    Serial.println(
      "Using timer wake only");
  }

  esp_sleep_enable_timer_wakeup(
    static_cast<uint64_t>(
      seconds)
    * 1000000ULL);

  clearLED();

  Serial.println(
    "Entering deep sleep...");

  Serial.flush();

  delay(50);

  esp_deep_sleep_start();
}

// =====================================================
// NODE WAKE CYCLE
// =====================================================
/*
Performs one complete sensing cycle each time the node powers up or wakes from
deep sleep.

The node restores state, pairs if required, reads the door and temperature,
updates edge intelligence and learning, reports to the gateway, allows a short
command-receive window, shows status, then calculates and enters the next sleep.
*/

/*
Runs one complete node wake cycle: restore state, read sensors, update edge logic,
communicate with the gateway and return to deep sleep.
*/
void setup() {

  Serial.begin(
    115200);

  delay(1000);

  Serial.println();
  Serial.println(
    "Fridge Manager Sensor Node");

  Serial.println(
    "==========================");

  rtc_gpio_deinit(
    DOOR_GPIO);

  pinMode(
    DOOR_PIN,
    INPUT_PULLUP);

  sensors.begin();

  statusLED.begin();

  clearLED();

  uint8_t wakeReason =
    getWakeReason();

  printWakeReason(
    wakeReason);

  if (
    !setupEspNow()) {

    Serial.println(
      "ESP-NOW unavailable");

    enterDeepSleep(
      FAST_INTERVAL_SECONDS);
  }

  bool forcePairing =
    false;

  // Only check the physical BOOT-button repair gesture after a power/reset wake;
  // normal timer/door wakeups should stay fast and return to sleep promptly.
  if (
    wakeReason == 0) {

    forcePairing =
      checkManualRepair();
  }

  if (!forcePairing) {

    loadGatewayFromNVS();
  }

  loadConfigFromNVS();
  loadLearningFromNVS();

  if (!gatewayPaired) {

    if (
      !startPairing()) {

      Serial.println(
        "Pairing failed.");

      Serial.println(
        "Will retry after sleep.");

      flashLED(
        255,
        0,
        0);

      enterDeepSleep(
        UNPAIRED_SLEEP_SECONDS);
    }

    addPeer(
      gatewayMAC);
  }

  else {

    Serial.print(
      "Stored gateway: ");

    printMAC(
      gatewayMAC);

    Serial.println();

    addPeer(
      gatewayMAC);
  }

  

  // Door duration is reconstructed across wake cycles using RTC memory. While
  // open, adaptive monitoring wakes every five seconds so the counter progresses.
  bool doorOpen =
    digitalRead(
      DOOR_PIN)
    == HIGH;

  Serial.print(
    "Door: ");

  Serial.println(
    doorOpen
      ? "OPEN"
      : "CLOSED");

  if (doorOpen) {

    if (doorWasOpen) {

      doorOpenSeconds +=
        DOOR_CHECK_INTERVAL_SECONDS;
    }

    else {

      doorOpenSeconds =
        0;
    }
  }

  else {

    doorOpenSeconds =
      0;
  }

  doorWasOpen =
    doorOpen;

  bool doorWarning =
    doorOpen &&
    (doorOpenSeconds >=
     doorWarningSeconds);

  Serial.print(
    "Door open time: ");

  Serial.print(
    doorOpenSeconds);

  Serial.println(
    " seconds");

  Serial.print(
    "Door warning: ");

  Serial.println(
    doorWarning
      ? "YES"
      : "NO");

  

  sensors.requestTemperatures();

  float temperature =
    sensors.getTempCByIndex(
      0);

  // DallasTemperature uses DEVICE_DISCONNECTED_C (approximately -127 C) for a
  // missing DS18B20; the additional limit catches the same invalid region.
  bool sensorFault =
    (
      temperature ==
        DEVICE_DISCONNECTED_C
      ||
      temperature <= -126.0
    );

  if (sensorFault) {

    Serial.println(
      "Temperature sensor fault");
  }

  else {

    Serial.print(
      "Temperature: ");

    Serial.print(
      temperature);

    Serial.println(" C");
  }

  

  uint32_t elapsedSeconds =
    0;

  // Only timer wakes contribute the scheduled sleep duration to persistence
  // timers. A door-triggered wake may occur much earlier than that interval.
  if (
    wakeReason == 1) {

    elapsedSeconds =
      previousSleepSeconds;
  }

  

  // Both edge features run locally before transmission, so the packet reports
  // the node's newly calculated state rather than relying on cloud/Pi processing.
  updateTemperatureState(
    temperature,
    doorOpen,
    sensorFault,
    elapsedSeconds);

  updateLearning(
    temperature,
    doorOpen,
    sensorFault,
    elapsedSeconds);

  

  // This report is also the gateway's opportunity to notice the node is awake
  // and return any configuration or learning command waiting for this MAC.
  sendSensorPacket(
    temperature,
    doorOpen,
    doorWarning,
    wakeReason);

  

  // Keep the radio awake briefly so the gateway can return queued config or learning commands.
  delay(500);

  

  if (sensorFault) {

    flashLED(
      255,
      0,
      0);
  }

  else if (doorWarning) {

    flashLED(
      255,
      40,
      0);
  }

  else if (doorOpen) {

    flashLED(
      255,
      80,
      0);
  }

  else {

    flashLED(
      0,
      80,
      0);
  }

  

  // The next interval is retained in RTC memory so the following timer wake can
  // use the actual scheduled interval when advancing edge/learning timers.
  uint32_t nextSleepInterval =
    calculateSleepInterval(
      temperature,
      doorOpen,
      sensorFault);

  previousSleepSeconds =
    nextSleepInterval;

  if (!sensorFault) {

    previousTemperature =
      temperature;

    previousTemperatureValid =
      true;
  }

  enterDeepSleep(
    nextSleepInterval);
}

// =====================================================
// MAIN LOOP
// =====================================================
/*
The normal Arduino loop is intentionally empty because this battery node performs
its work once in setup() and then enters deep sleep.

Each later wake starts setup() again, with RTC memory preserving the state needed
between monitoring cycles.
*/

/*
Intentionally empty because each wake cycle is completed in setup() before sleep.
*/
void loop() {
}
