// =====================================================
// COMMUNICATION PROTOCOL
// =====================================================
/*
Defines the ESP-NOW protocol shared between the gateway and sensor nodes.

The packed packet structures keep the transmitted layout consistent, while the
magic value, message type and protocol version allow received data to be
identified before it is handled.
*/

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <Preferences.h>

/*
This ESP32-S3 is the always-powered communications gateway.

Sensor nodes communicate with it using ESP-NOW. The gateway then translates
sensor reports into serial JSON for the Raspberry Pi and translates serial
PAIR/CONFIG/LEARN commands from the Pi back into ESP-NOW packets for the nodes.
*/
#define PROTOCOL_VERSION 1
#define FRIDGE_MANAGER_MAGIC 0x464D4752

#define PAIRING_WINDOW_MS 30000
#define MAX_KNOWN_SENSORS 10

enum MessageType : uint8_t {
  MSG_PAIR_REQUEST = 1,
  MSG_PAIR_RESPONSE = 2,
  MSG_SENSOR_DATA = 3,
  MSG_SENSOR_CONFIG = 4,
  MSG_CONFIG_ACK = 5,
  MSG_LEARNING_COMMAND = 6
};

/*
Packed structures define the binary protocol shared with the sensor firmware.
'packed' prevents compiler-added padding from changing field positions, so both
ESP32 devices interpret the same transmitted byte sequence consistently.
*/
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

struct PendingConfig {
  bool valid;
  uint8_t sensorMAC[6];
  SensorConfigPacket config;
};

struct PendingLearningCommand {
  bool valid;
  uint8_t sensorMAC[6];
  uint8_t command;
  uint8_t mode;
};

// =====================================================
// GATEWAY STATE
// =====================================================
/*
Stores the gateway's current pairing, configuration and learning-command state.

Known sensors are retained separately from temporary pending operations so the
gateway can remember trusted nodes while still handling one configuration,
learning command or pairing discovery at a time.
*/

/*
Preferences provides persistent NVS storage on the gateway.

It is used here to remember approved sensor MAC addresses across gateway restarts.
That trusted-sensor list is separate from the temporary ESP-NOW peer table.
*/
Preferences preferences;

/*
The gateway may receive a command from the Raspberry Pi while a battery node is
asleep. These pending structures hold work until the target sensor next wakes and
sends data, giving the gateway an opportunity to transmit back to that node.

Configuration remains pending until a matching acknowledgement is received.
The learning command follows a simpler one-send-at-next-wake approach.
*/
PendingConfig pendingConfig = {};
PendingLearningCommand pendingLearning = {};

// Each row stores one approved six-byte ESP32 MAC address.
uint8_t knownSensors[MAX_KNOWN_SENSORS][6];
uint8_t knownSensorCount = 0;

/*
Unknown nodes are discoverable only during the explicit pairing window.
pendingPairMAC is discovery state only: the node is not added to the persistent
trusted list until the Pi/UI sends PAIR:ACCEPT for that same MAC address.
*/
bool pairingActive = false;
unsigned long pairingStartedAt = 0;

bool pendingPairValid = false;
uint8_t pendingPairMAC[6] = {};


// =====================================================
// MAC ADDRESS UTILITIES
// =====================================================
/*
Converts ESP32 MAC addresses between byte arrays and the text format used by
serial commands and status messages.

This lets the gateway use the same sensor identifier for ESP-NOW communication
and for communication with the Raspberry Pi.
*/

// Prints a MAC address in the readable format used by gateway diagnostics.
void printMAC(const uint8_t *mac) {

  for (int i = 0; i < 6; i++) {

    if (i > 0) {
      Serial.print(":");
    }

    if (mac[i] < 0x10) {
      Serial.print("0");
    }

    Serial.print(mac[i], HEX);
  }
}


// Converts the human-readable AA:BB:CC:DD:EE:FF serial format into six bytes.
/*
Converts a text MAC address from a Pi serial command into six binary bytes.
Returns whether the supplied address was valid.
*/
bool parseMAC(
  const char *text,
  uint8_t *mac) {

  unsigned int values[6];

  int count = sscanf(
    text,
    "%x:%x:%x:%x:%x:%x",
    &values[0],
    &values[1],
    &values[2],
    &values[3],
    &values[4],
    &values[5]);

  if (count != 6) {
    return false;
  }

  for (int i = 0; i < 6; i++) {

    if (values[i] > 255) {
      return false;
    }

    mac[i] =
      static_cast<uint8_t>(
        values[i]);
  }

  return true;
}


// =====================================================
// TRUSTED SENSOR STORAGE
// =====================================================
/*
Maintains the list of sensor nodes that the gateway is allowed to recognise.

The list is stored in ESP32 Preferences so trusted sensors survive a restart,
and peers can be restored to ESP-NOW when the gateway starts again.
*/

// Checks persistent application-level trust, not merely ESP-NOW peer presence.
/*
Checks whether a sensor MAC is in the gateway's persistent approved-sensor list.
This application-level trust is separate from the ESP-NOW peer table.
*/
bool isKnownSensor(
  const uint8_t *mac) {

  for (
    uint8_t i = 0;
    i < knownSensorCount;
    i++) {

    if (
      memcmp(
        knownSensors[i],
        mac,
        6)
      == 0) {

      return true;
    }
  }

  return false;
}


// Writes the approved MAC list to NVS so trust survives a gateway restart.
/*
Saves the approved sensor list to NVS so trust survives a gateway restart.
*/
void saveKnownSensors() {

  preferences.begin(
    "fm_gateway",
    false);

  preferences.putUChar(
    "count",
    knownSensorCount);

  // Store the complete fixed-size array so the trust list survives a restart.
  preferences.putBytes(
    "sensors",
    knownSensors,
    sizeof(knownSensors));

  preferences.end();
}


// Restores the persisted trusted-sensor list during gateway startup.
/*
Restores the approved sensor list from NVS when the gateway starts.
*/
void loadKnownSensors() {

  memset(
    knownSensors,
    0,
    sizeof(knownSensors));

  preferences.begin(
    "fm_gateway",
    true);

  knownSensorCount =
    preferences.getUChar(
      "count",
      0);

  if (
    knownSensorCount > MAX_KNOWN_SENSORS) {

    knownSensorCount = 0;
  }

  if (knownSensorCount > 0) {

    preferences.getBytes(
      "sensors",
      knownSensors,
      sizeof(knownSensors));
  }

  preferences.end();

  Serial.print(
    "Known sensors loaded: ");

  Serial.println(
    knownSensorCount);
}


// Permanently approves a sensor MAC unless it is already trusted/list is full.
/*
Adds a sensor to the persistent approved list if space is available.
Returns whether the sensor is now remembered.
*/
bool rememberSensor(
  const uint8_t *mac) {

  if (isKnownSensor(mac)) {
    return true;
  }

  if (
    knownSensorCount >= MAX_KNOWN_SENSORS) {

    Serial.println(
      "PAIR_ERROR:sensor_list_full");

    return false;
  }

  memcpy(
    knownSensors[knownSensorCount],
    mac,
    6);

  knownSensorCount++;

  saveKnownSensors();

  Serial.print(
    "PAIR_REMEMBERED:{\"node_mac\":\"");

  printMAC(mac);

  Serial.println("\"}");

  return true;
}


// Removes application trust and also removes the current ESP-NOW peer entry.
/*
Removes a sensor from the approved list and ESP-NOW peer table.
Returns whether a known sensor was actually removed.
*/
bool forgetSensor(
  const uint8_t *mac) {

  for (
    uint8_t i = 0;
    i < knownSensorCount;
    i++) {

    if (
      memcmp(
        knownSensors[i],
        mac,
        6)
      == 0) {

      for (
        uint8_t j = i;
        j + 1 < knownSensorCount;
        j++) {

        memcpy(
          knownSensors[j],
          knownSensors[j + 1],
          6);
      }

      knownSensorCount--;

      memset(
        knownSensors[knownSensorCount],
        0,
        6);

      saveKnownSensors();

      if (
        esp_now_is_peer_exist(mac)) {

        esp_now_del_peer(mac);
      }

      Serial.print(
        "PAIR_FORGOTTEN:{\"node_mac\":\"");

      printMAC(mac);

      Serial.println("\"}");

      return true;
    }
  }

  Serial.print(
    "PAIR_NOT_FOUND:{\"node_mac\":\"");

  printMAC(mac);

  Serial.println("\"}");

  return false;
}


/*
Reports the gateway's remembered sensors over Serial for the Raspberry Pi/UI.
*/
void listKnownSensors() {

  Serial.print(
    "PAIR_LIST:{\"count\":");

  Serial.print(
    knownSensorCount);

  Serial.print(
    ",\"sensors\":[");

  for (
    uint8_t i = 0;
    i < knownSensorCount;
    i++) {

    if (i > 0) {
      Serial.print(",");
    }

    Serial.print("\"");

    printMAC(
      knownSensors[i]);

    Serial.print("\"");
  }

  Serial.println("]}");
}


// =====================================================
// ESP-NOW PEER MANAGEMENT
// =====================================================
/*
Ensures a sensor MAC address exists in the ESP-NOW peer table before the
gateway attempts to transmit directly to it.

Peers are added without ESP-NOW encryption because trust is controlled by the
gateway's stored sensor list and explicit pairing workflow.
*/

/*
ESP-NOW requires a destination to exist in its peer table before direct sends.
This is transport setup only and does not itself mean the sensor is trusted.
Encryption is explicitly disabled by peerInfo.encrypt = false.
*/
/*
Ensures a sensor exists in the ESP-NOW peer table so the gateway can transmit to it.
This transport relationship does not by itself make the sensor trusted.
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

  esp_err_t result =
    esp_now_add_peer(
      &peerInfo);

  if (
    result != ESP_OK) {

    Serial.print(
      "Failed to add peer. Error: ");

    Serial.println(
      result);

    return false;
  }

  Serial.print(
    "Added sensor peer: ");

  printMAC(mac);

  Serial.println();

  return true;
}


// =====================================================
// SENSOR PAIRING
// =====================================================
/*
Controls discovery and approval of sensor nodes.

Known sensors can automatically restore their connection, while an unknown
sensor is only exposed as a temporary discovery during the pairing window and
is not trusted until the Raspberry Pi explicitly accepts it.
*/

// Opens the 30-second discovery window requested by the Raspberry Pi/UI.
/*
Opens the timed Add Sensor window so unknown pairing requests can be discovered.
*/
void startPairingMode() {

  pairingActive = true;

  pairingStartedAt =
    millis();

  pendingPairValid = false;

  memset(
    pendingPairMAC,
    0,
    sizeof(pendingPairMAC));

  Serial.println(
    "PAIRING_STARTED");
}

/*
Closes pairing mode and clears any sensor that was only pending discovery.
*/
void stopPairingMode() {

  pairingActive = false;
  pendingPairValid = false;

  memset(
    pendingPairMAC,
    0,
    sizeof(pendingPairMAC));

  Serial.println(
    "PAIRING_CANCELLED");
}


// Sends protocol confirmation to a node so it can persist this gateway MAC.
/*
Sends this gateway's pairing response to an approved or previously known sensor.
*/
void sendPairResponse(
  const uint8_t *sensorMAC) {

  if (!addPeer(sensorMAC)) {
    return;
  }

  PairResponse response;

  response.magic =
    FRIDGE_MANAGER_MAGIC;

  response.messageType =
    MSG_PAIR_RESPONSE;

  response.protocolVersion =
    PROTOCOL_VERSION;

  esp_err_t result =
    esp_now_send(
      sensorMAC,
      reinterpret_cast<uint8_t *>(
        &response),
      sizeof(response));

  if (
    result == ESP_OK) {

    Serial.println(
      "Pair response queued");
  }

  else {

    Serial.print(
      "Pair response failed. Error: ");

    Serial.println(
      result);
  }
}


/*
Processes a node's pairing broadcast.

Previously approved sensors are automatically restored. An unknown sensor is
ignored unless pairing mode is active; during pairing it is only staged as a
pending discovery until the user/Pi explicitly accepts it.
*/
/*
Processes sensor pairing requests, automatically restoring known sensors while
staging unknown sensors for explicit approval only during pairing mode.
*/
void handlePairRequest(
  const esp_now_recv_info_t *info,
  const uint8_t *data,
  int len) {

  if (
    len != sizeof(PairRequest)) {

    return;
  }

  PairRequest request;

  memcpy(
    &request,
    data,
    sizeof(request));

  if (
    request.magic != FRIDGE_MANAGER_MAGIC || request.messageType != MSG_PAIR_REQUEST) {

    return;
  }

  if (
    request.protocolVersion != PROTOCOL_VERSION) {

    Serial.println(
      "Unsupported protocol version");

    return;
  }

  // Existing trusted sensor:
  // automatically restore pairing.

  if (
    isKnownSensor(
      info->src_addr)) {

    Serial.print(
      "KNOWN_SENSOR_REPAIR:{\"node_mac\":\"");

    printMAC(
      info->src_addr);

    Serial.println("\"}");

    sendPairResponse(
      info->src_addr);

    Serial.print(
      "PAIR_RESTORED:{\"node_mac\":\"");

    printMAC(
      info->src_addr);

    Serial.println("\"}");

    return;
  }

  // Unknown sensors are only discoverable
  // while Add Sensor pairing is active.

  if (!pairingActive) {

    Serial.print(
      "UNKNOWN_PAIR_REQUEST_IGNORED:{\"node_mac\":\"");

    printMAC(
      info->src_addr);

    Serial.println("\"}");

    return;
  }

  // Store as a temporary discovery only.
  // Do NOT trust or pair it yet.

  memcpy(
    pendingPairMAC,
    info->src_addr,
    6);

  pendingPairValid = true;

  Serial.print(
    "PAIR_FOUND:{\"node_mac\":\"");

  printMAC(
    pendingPairMAC);

  Serial.println("\"}");
}

// Converts the currently discovered MAC into persistent trust after approval.
/*
Approves the currently discovered sensor when its MAC matches the requested device,
then remembers it and sends the pairing response.
*/
void acceptPendingPair(
  const uint8_t *mac) {

  if (!pairingActive) {

    Serial.println(
      "PAIR_ERROR:not_pairing");

    return;
  }

  if (!pendingPairValid) {

    Serial.println(
      "PAIR_ERROR:no_sensor_found");

    return;
  }

  if (
    memcmp(
      pendingPairMAC,
      mac,
      6)
    != 0) {

    Serial.println(
      "PAIR_ERROR:sensor_mismatch");

    return;
  }

  if (!rememberSensor(mac)) {
    return;
  }

  sendPairResponse(mac);

  pairingActive = false;
  pendingPairValid = false;

  memset(
    pendingPairMAC,
    0,
    sizeof(pendingPairMAC));

  Serial.print(
    "PAIRING_COMPLETE:{\"node_mac\":\"");

  printMAC(mac);

  Serial.println("\"}");
}

// =====================================================
// SERIAL PAIRING COMMANDS
// =====================================================
/*
Processes pairing commands received from the Raspberry Pi over USB serial.

Commands can start or cancel discovery, list stored sensors, accept a discovered
node, manually remember a node, or remove a previously trusted sensor.
*/

/*
Parses PAIR commands arriving from the Raspberry Pi over USB serial.

START/CANCEL control discovery, LIST reports trusted nodes, ACCEPT approves the
current discovery, REMEMBER manually stores a MAC, and FORGET removes one.
*/
/*
Handles PAIR commands from the Raspberry Pi for starting, cancelling, listing,
accepting, remembering and forgetting sensors.
*/
void handleSerialPair(
  char *command) {

  if (
    strcmp(
      command,
      "PAIR:START")
    == 0) {

    startPairingMode();
    return;
  }

  if (
    strcmp(
      command,
      "PAIR:CANCEL")
    == 0) {

    stopPairingMode();
    return;
  }

  if (
    strcmp(
      command,
      "PAIR:LIST")
    == 0) {

    listKnownSensors();
    return;
  }

  const char *acceptPrefix =
    "PAIR:ACCEPT,";

  const char *rememberPrefix =
    "PAIR:REMEMBER,";

  const char *forgetPrefix =
    "PAIR:FORGET,";

  if (
    strncmp(
      command,
      acceptPrefix,
      strlen(acceptPrefix))
    == 0) {

    uint8_t mac[6];

    if (
      !parseMAC(
        command + strlen(acceptPrefix),
        mac)) {

      Serial.println(
        "PAIR_ERROR:invalid_mac");

      return;
    }

    acceptPendingPair(mac);
    return;
  }

  if (
    strncmp(
      command,
      rememberPrefix,
      strlen(rememberPrefix))
    == 0) {

    uint8_t mac[6];

    if (
      !parseMAC(
        command + strlen(rememberPrefix),
        mac)) {

      Serial.println(
        "PAIR_ERROR:invalid_mac");

      return;
    }

    rememberSensor(mac);
    return;
  }

  if (
    strncmp(
      command,
      forgetPrefix,
      strlen(forgetPrefix))
    == 0) {

    uint8_t mac[6];

    if (
      !parseMAC(
        command + strlen(forgetPrefix),
        mac)) {

      Serial.println(
        "PAIR_ERROR:invalid_mac");

      return;
    }

    forgetSensor(mac);
    return;
  }

  Serial.println(
    "PAIR_ERROR:invalid_command");
}


// =====================================================
// SENSOR CONFIGURATION
// =====================================================
/*
Receives sensor configuration from the Raspberry Pi and prepares it for delivery.

The serial command is validated and converted into the same packed configuration
packet understood by the node. The packet remains pending until that sensor next
reports, allowing configuration to be delivered while battery nodes are awake.
*/

/*
Parses the comma-separated CONFIG command produced by the Pi.

The values are converted into the same packed SensorConfigPacket understood by
the sensor firmware, then retained in pendingConfig until the target node wakes.
*/
/*
Parses a CONFIG command from the Pi and stages one configuration for delivery when
the target battery node next wakes.
*/
void handleSerialConfig(
  char *command) {

  char macText[18];

  unsigned long version;
  unsigned int compartment;

  float minTemp;
  float maxTemp;
  float hysteresis;

  unsigned long warningSeconds;
  unsigned long recoverySeconds;
  unsigned long doorSeconds;

  unsigned int enabled;

  int fields = sscanf(
    command,
    "CONFIG:%17[^,],%lu,%u,%f,%f,%f,%lu,%lu,%lu,%u",
    macText,
    &version,
    &compartment,
    &minTemp,
    &maxTemp,
    &hysteresis,
    &warningSeconds,
    &recoverySeconds,
    &doorSeconds,
    &enabled);

  if (
    fields != 10) {

    Serial.println(
      "CONFIG_ERROR:invalid_command");

    return;
  }

  if (
    compartment > 2 || enabled > 1) {

    Serial.println(
      "CONFIG_ERROR:invalid_values");

    return;
  }

  uint8_t parsedMAC[6];

  if (
    !parseMAC(
      macText,
      parsedMAC)) {

    Serial.println(
      "CONFIG_ERROR:invalid_mac");

    return;
  }

  // Mark the configuration as waiting; it is not considered applied yet.
  pendingConfig.valid = true;

  memcpy(
    pendingConfig.sensorMAC,
    parsedMAC,
    6);

  pendingConfig.config.magic =
    FRIDGE_MANAGER_MAGIC;

  pendingConfig.config.messageType =
    MSG_SENSOR_CONFIG;

  pendingConfig.config.protocolVersion =
    PROTOCOL_VERSION;

  pendingConfig.config.configVersion =
    version;

  pendingConfig.config.compartment =
    compartment;

  pendingConfig.config.minTemperature =
    minTemp;

  pendingConfig.config.maxTemperature =
    maxTemp;

  pendingConfig.config.temperatureHysteresis =
    hysteresis;

  pendingConfig.config.temperatureWarningSeconds =
    warningSeconds;

  pendingConfig.config.temperatureRecoverySeconds =
    recoverySeconds;

  pendingConfig.config.doorWarningSeconds =
    doorSeconds;

  pendingConfig.config.enabled =
    enabled == 1;

  Serial.print(
    "CONFIG_READY:{\"node_mac\":\"");

  printMAC(
    pendingConfig.sensorMAC);

  Serial.print(
    "\",\"config_version\":");

  Serial.print(
    pendingConfig.config.configVersion);

  Serial.println("}");
}


// =====================================================
// SENSOR LEARNING CONTROL
// =====================================================
/*
Forwards learning start and cancel requests from the Raspberry Pi to a sensor node.

The gateway does not perform the learning itself. It validates and queues the
requested action, then sends the command when the target node is available.
*/

// Packages a pending START/CANCEL learning request for ESP-NOW transmission.
/*
Builds and transmits a START or CANCEL learning command to the target sensor.
*/
void sendLearningCommand(
  const uint8_t *sensorMAC,
  uint8_t command,
  uint8_t mode) {

  if (!addPeer(sensorMAC)) {
    return;
  }

  LearningCommandPacket packet;

  packet.magic =
    FRIDGE_MANAGER_MAGIC;

  packet.messageType =
    MSG_LEARNING_COMMAND;

  packet.protocolVersion =
    PROTOCOL_VERSION;

  packet.command =
    command;

  packet.mode =
    mode;

  esp_err_t result =
    esp_now_send(
      sensorMAC,
      reinterpret_cast<uint8_t *>(
        &packet),
      sizeof(packet));

  if (
    result == ESP_OK) {

    Serial.print(
      "Learning command queued for sensor: ");

    printMAC(sensorMAC);

    Serial.print(
      " command ");

    Serial.print(command);

    Serial.print(
      " mode ");

    Serial.println(mode);
  }

  else {

    Serial.print(
      "Learning command failed. Error: ");

    Serial.println(
      result);
  }
}


/*
Parses LEARN:<MAC>,START|CANCEL,<mode> from the Pi and stages it for the node.

Modes 1-3 are accepted for START. CANCEL is represented as command 2 with mode 0.
*/
/*
Parses a LEARN command from the Pi and stages it until the target node is awake.
*/
void handleSerialLearning(
  char *command) {

  char macText[18];
  char actionText[10];

  unsigned int mode;

  int fields = sscanf(
    command,
    "LEARN:%17[^,],%9[^,],%u",
    macText,
    actionText,
    &mode);

  if (
    fields != 3) {

    Serial.println(
      "LEARN_ERROR:invalid_command");

    return;
  }

  uint8_t parsedMAC[6];

  if (
    !parseMAC(
      macText,
      parsedMAC)) {

    Serial.println(
      "LEARN_ERROR:invalid_mac");

    return;
  }

  uint8_t learningCommand;

  if (
    strcmp(
      actionText,
      "START")
    == 0) {

    learningCommand = 1;

    if (
      mode < 1 || mode > 3) {

      Serial.println(
        "LEARN_ERROR:invalid_mode");

      return;
    }
  }

  else if (
    strcmp(
      actionText,
      "CANCEL")
    == 0) {

    learningCommand = 2;
    mode = 0;
  }

  else {

    Serial.println(
      "LEARN_ERROR:invalid_action");

    return;
  }

  pendingLearning.valid =
    true;

  memcpy(
    pendingLearning.sensorMAC,
    parsedMAC,
    6);

  pendingLearning.command =
    learningCommand;

  pendingLearning.mode =
    static_cast<uint8_t>(
      mode);

  Serial.print(
    "LEARN_READY:{\"node_mac\":\"");

  printMAC(parsedMAC);

  Serial.print(
    "\",\"command\":");

  Serial.print(
    learningCommand);

  Serial.print(
    ",\"mode\":");

  Serial.print(mode);

  Serial.println("}");
}


// =====================================================
// CONFIGURATION DELIVERY
// =====================================================
/*
Sends a prepared configuration packet to the target sensor over ESP-NOW.

The configuration version travels with the packet so the later acknowledgement
can confirm that the node applied the same configuration requested by the Pi.
*/

// Sends the staged configuration packet when its target node is awake.
/*
Sends a prepared configuration packet to its target sensor over ESP-NOW.
*/
void sendSensorConfig(
  const uint8_t *sensorMAC,
  const SensorConfigPacket &config) {

  if (!addPeer(sensorMAC)) {
    return;
  }

  esp_err_t result =
    esp_now_send(
      sensorMAC,
      reinterpret_cast<const uint8_t *>(
        &config),
      sizeof(config));

  if (
    result == ESP_OK) {

    Serial.print(
      "Config queued for sensor: ");

    printMAC(sensorMAC);

    Serial.print(
      " version ");

    Serial.println(
      config.configVersion);
  }

  else {

    Serial.print(
      "Config send failed. Error: ");

    Serial.println(
      result);
  }
}


// =====================================================
// SENSOR DATA HANDLING
// =====================================================
/*
Processes sensor reports received from the fridge and freezer nodes.

Each report is printed in a readable form and as JSON for the Raspberry Pi
bridge. A matching report also provides an opportunity to deliver pending
configuration or learning commands while that battery-powered node is awake.
*/

// Prints the readable meaning of a sensor's reported wake code.
void printWakeReason(
  uint8_t wakeReason) {

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


/*
Handles a SensorPacket received from a node.

Besides diagnostic serial output, it creates the JSON: line consumed by the
Raspberry Pi gateway_mqtt.py bridge. After processing the report, the gateway
uses the node's awake period to send any configuration or learning work waiting
for that same MAC address.
*/
/*
Processes a sensor report, emits the JSON consumed by gateway_mqtt.py, and uses
the node's awake window to deliver any matching pending configuration or learning command.
*/
void handleSensorData(
  const esp_now_recv_info_t *info,
  const uint8_t *data,
  int len) {

  if (
    len != sizeof(SensorPacket)) {

    Serial.print(
      "Unexpected packet size: ");

    Serial.println(len);

    return;
  }

  SensorPacket packet;

  memcpy(
    &packet,
    data,
    sizeof(packet));

  if (
    packet.magic != FRIDGE_MANAGER_MAGIC || packet.messageType != MSG_SENSOR_DATA) {

    return;
  }

  Serial.println();
  Serial.println(
    "==============================");

  Serial.println(
    "ESP-NOW SENSOR PACKET");

  Serial.print(
    "Sensor MAC: ");

  printMAC(
    info->src_addr);

  Serial.println();

  Serial.print(
    "Temperature: ");

  Serial.print(
    packet.temperature,
    2);

  Serial.println(
    " C");

  Serial.print(
    "Door: ");

  Serial.println(
    packet.doorOpen
      ? "OPEN"
      : "CLOSED");

  Serial.print(
    "Door Warning: ");

  Serial.println(
    packet.doorWarning
      ? "YES"
      : "NO");

  Serial.print(
    "Wake Reason: ");

  printWakeReason(
    packet.wakeReason);

  Serial.print(
    "Temperature State: ");

  Serial.println(
    packet.temperatureState);

  Serial.print(
    "Learning Trained: ");

  Serial.println(
    packet.learningTrained
      ? "YES"
      : "NO");

  if (
    packet.learningTrained) {

    Serial.print(
      "Learned Minimum: ");

    Serial.print(
      packet.learnedMinimumTemperature,
      2);

    Serial.println(
      " C");

    Serial.print(
      "Learned Maximum: ");

    Serial.print(
      packet.learnedMaximumTemperature,
      2);

    Serial.println(
      " C");
  }

  Serial.println(
    "==============================");

  // The JSON: prefix lets gateway_mqtt.py distinguish machine-readable sensor
  // reports from the gateway's ordinary diagnostic Serial output.
  Serial.print(
    "JSON:{\"node_mac\":\"");

  printMAC(
    info->src_addr);

  Serial.print(
    "\",\"temperature\":");

  Serial.print(
    packet.temperature,
    2);

  Serial.print(
    ",\"door_open\":");

  Serial.print(
    packet.doorOpen
      ? "true"
      : "false");

  Serial.print(
    ",\"door_warning\":");

  Serial.print(
    packet.doorWarning
      ? "true"
      : "false");

  Serial.print(
    ",\"wake_reason\":");

  Serial.print(
    packet.wakeReason);

  Serial.print(
    ",\"temperature_state\":");

  Serial.print(
    packet.temperatureState);

  Serial.print(
    ",\"learning_mode\":");

  Serial.print(
    packet.learningMode);

  Serial.print(
    ",\"learning_active\":");

  Serial.print(
    packet.learningActive
      ? "true"
      : "false");

  Serial.print(
    ",\"learning_trained\":");

  Serial.print(
    packet.learningTrained
      ? "true"
      : "false");

  Serial.print(
    ",\"learning_elapsed_seconds\":");

  Serial.print(
    packet.learningElapsedSeconds);

  Serial.print(
    ",\"learning_sample_count\":");

  Serial.print(
    packet.learningSampleCount);

  Serial.print(
    ",\"learned_minimum_temperature\":");

  Serial.print(
    packet.learnedMinimumTemperature,
    2);

  Serial.print(
    ",\"learned_maximum_temperature\":");

  Serial.print(
    packet.learnedMaximumTemperature,
    2);

  Serial.println("}");

  // A sensor report confirms the sleeping node is awake, so pending work can be sent.
  // Battery nodes sleep most of the time, so their own report acts as evidence
  // that the radio is awake and ready to receive its waiting configuration.
  if (
    pendingConfig.valid && memcmp(info->src_addr, pendingConfig.sensorMAC, 6) == 0) {

    Serial.print(
      "Pending config found for sensor: ");

    printMAC(
      info->src_addr);

    Serial.println();

    sendSensorConfig(
      info->src_addr,
      pendingConfig.config);
  }

  if (
    pendingLearning.valid && memcmp(info->src_addr, pendingLearning.sensorMAC, 6) == 0) {

    Serial.print(
      "Pending learning command found for sensor: ");

    printMAC(
      info->src_addr);

    Serial.println();

    sendLearningCommand(
      info->src_addr,
      pendingLearning.command,
      pendingLearning.mode);

    // Learning commands are one-shot requests; remove them after they are queued.
    // Unlike configuration, this command is cleared after the send attempt rather
    // than waiting for a dedicated learning acknowledgement packet.
    pendingLearning.valid =
      false;
  }
}


// =====================================================
// CONFIGURATION ACKNOWLEDGEMENT
// =====================================================
/*
Confirms that a sensor node received and applied a configuration update.

The acknowledgement is forwarded to the Raspberry Pi and the pending
configuration is cleared only when both the sensor MAC and version match.
*/

/*
Validates a node's configuration acknowledgement.

pendingConfig is cleared only when both the sender MAC and configVersion match
the waiting configuration, preventing an unrelated or stale ACK from confirming it.
*/
/*
Processes a sensor's configuration acknowledgement and clears the pending config
only when both the sensor MAC and configuration version match.
*/
void handleConfigAck(
  const esp_now_recv_info_t *info,
  const uint8_t *data,
  int len) {

  if (
    len != sizeof(ConfigAckPacket)) {

    return;
  }

  ConfigAckPacket ack;

  memcpy(
    &ack,
    data,
    sizeof(ack));

  if (
    ack.magic != FRIDGE_MANAGER_MAGIC || ack.messageType != MSG_CONFIG_ACK || ack.protocolVersion != PROTOCOL_VERSION) {

    return;
  }

  Serial.println();
  Serial.println(
    "CONFIG ACK RECEIVED");

  Serial.print(
    "Sensor MAC: ");

  printMAC(
    info->src_addr);

  Serial.println();

  Serial.print(
    "Config version: ");

  Serial.println(
    ack.configVersion);

  Serial.print(
    "CONFIG_ACK:{\"node_mac\":\"");

  printMAC(
    info->src_addr);

  Serial.print(
    "\",\"config_version\":");

  Serial.print(
    ack.configVersion);

  Serial.println("}");

  if (
    pendingConfig.valid && memcmp(info->src_addr, pendingConfig.sensorMAC, 6) == 0 && ack.configVersion == pendingConfig.config.configVersion) {

    // Only the matching version confirms that the requested settings were applied.
    pendingConfig.valid =
      false;

    Serial.println(
      "CONFIG_CONFIRMED");
  }
}


// =====================================================
// ESP-NOW CALLBACKS
// =====================================================
/*
Handles low-level ESP-NOW send results and routes incoming packets.

Incoming data is first checked for the Fridge Manager magic value, then the
message type determines whether pairing, sensor data or a configuration
acknowledgement handler should process it.
*/

// ESP-NOW invokes this callback asynchronously after a queued transmission.
/*
ESP-NOW callback used to report the result of gateway transmissions.
*/
void onDataSent(
  const wifi_tx_info_t *info,
  esp_now_send_status_t status) {

  Serial.print(
    "ESP-NOW send: ");

  Serial.println(
    status == ESP_NOW_SEND_SUCCESS
      ? "SUCCESS"
      : "FAILED");
}


/*
Central ESP-NOW receive dispatcher.

The common magic value filters unrelated traffic and messageType routes each
supported packet to the pairing, sensor-data, or configuration-ACK handler.
*/
/*
Validates incoming Fridge Manager packets and dispatches pairing requests, sensor
reports and configuration acknowledgements to their handlers.
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

  switch (messageType) {

    case MSG_PAIR_REQUEST:

      handlePairRequest(
        info,
        data,
        len);

      break;

    case MSG_SENSOR_DATA:

      handleSensorData(
        info,
        data,
        len);

      break;

    case MSG_CONFIG_ACK:

      handleConfigAck(
        info,
        data,
        len);

      break;

    default:

      Serial.print(
        "Unknown message type: ");

      Serial.println(
        messageType);

      break;
  }
}


// =====================================================
// GATEWAY SETUP
// =====================================================
/*
Initialises USB serial, Wi-Fi station mode and ESP-NOW when the gateway starts.

Previously trusted sensors are loaded from persistent storage and restored as
ESP-NOW peers so normal communication can resume after a gateway restart.
*/

/*
Initialises the always-powered gateway.

It starts USB serial and ESP-NOW, restores the trusted sensor list from NVS,
re-adds those MACs to the ESP-NOW peer table, then remains available to bridge
between sleeping sensor nodes and the Raspberry Pi.
*/
/*
Initialises Serial, ESP-NOW and the persistent sensor list so the gateway can bridge
between battery nodes and the Raspberry Pi.
*/
void setup() {

  Serial.begin(
    115200);

  delay(
    1500);

  Serial.println();

  Serial.println(
    "Fridge Manager XIAO S3 Gateway");

  Serial.println(
    "==============================");

  WiFi.mode(
    WIFI_STA);

  delay(
    100);

  uint8_t gatewayMAC[6];

  esp_wifi_get_mac(
    WIFI_IF_STA,
    gatewayMAC);

  Serial.print(
    "Gateway MAC: ");

  printMAC(
    gatewayMAC);

  Serial.println();

  if (
    esp_now_init() != ESP_OK) {

    Serial.println(
      "ESP-NOW initialization FAILED");

    return;
  }

  esp_now_register_send_cb(
    onDataSent);

  esp_now_register_recv_cb(
    onDataReceived);

  loadKnownSensors();

  for (
    uint8_t i = 0;
    i < knownSensorCount;
    i++) {

    addPeer(
      knownSensors[i]);
  }

  Serial.println(
    "ESP-NOW initialized");

  Serial.println(
    "Gateway ready");

  Serial.println(
    "Waiting for sensors...");
}


// =====================================================
// MAIN LOOP
// =====================================================
/*
Maintains the pairing timeout and receives commands from the Raspberry Pi.

Serial input is collected until a complete line is received, then routed by its
PAIR, CONFIG or LEARN prefix to the appropriate gateway command handler.
*/

/*
Runs the gateway's continuous control loop.

It expires the pairing window and assembles newline-terminated commands from the
Pi's USB serial connection before dispatching PAIR, CONFIG, and LEARN commands.
Unlike the sensor nodes, the gateway does not deep sleep.
*/
/*
Continuously handles pairing timeout and newline-terminated PAIR, CONFIG and LEARN
commands from the Raspberry Pi while ESP-NOW traffic is handled by callbacks.
*/
void loop() {

  if (
    pairingActive && millis() - pairingStartedAt >= PAIRING_WINDOW_MS) {

    pairingActive = false;
    pendingPairValid = false;

    memset(
      pendingPairMAC,
      0,
      sizeof(pendingPairMAC));

    Serial.println(
      "PAIRING_TIMEOUT");
  }

  // Static storage keeps a partially received serial command between loop passes.
  static char serialBuffer[180];

  static size_t serialLength = 0;

  while (
    Serial.available() > 0) {

    char incoming =
      Serial.read();

    if (
      incoming == '\n' || incoming == '\r') {

      if (
        serialLength > 0) {

        serialBuffer[serialLength] = '\0';

        if (
          strncmp(
            serialBuffer,
            "PAIR:",
            5)
          == 0) {

          handleSerialPair(
            serialBuffer);
        }

        else if (
          strncmp(
            serialBuffer,
            "CONFIG:",
            7)
          == 0) {

          handleSerialConfig(
            serialBuffer);
        }

        else if (
          strncmp(
            serialBuffer,
            "LEARN:",
            6)
          == 0) {

          handleSerialLearning(
            serialBuffer);
        }

        else {

          Serial.print(
            "SERIAL_ERROR:unknown_command:");

          Serial.println(
            serialBuffer);
        }

        serialLength = 0;
      }
    }

    else if (
      serialLength < sizeof(serialBuffer) - 1) {

      serialBuffer[serialLength++] =
        incoming;
    }

    else {

      serialLength = 0;

      Serial.println(
        "SERIAL_ERROR:command_too_long");
    }
  }

  delay(10);
}