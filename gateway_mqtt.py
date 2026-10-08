import json
import os
import ssl
import threading
import time
import serial
import sqlite3
import paho.mqtt.client as mqtt

from gateway.database import (
    get_sensor_details,
    mark_alert_events_acknowledged,
    record_event,
    record_sensor_reading,
    register_or_update_sensor,
)

from gateway.hardware import hardware


# =====================================================
"""
FRIDGE MANAGER
USB GATEWAY -> MQTT / DATABASE / HARDWARE BRIDGE
"""
# =====================================================

SERIAL_PORT = "/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_68:EE:8F:61:69:B4-if00"
BAUD_RATE = 115200

MQTT_BROKER = "localhost"
MQTT_PORT = 1883

ADAFRUIT_IO_HOST = "io.adafruit.com"
ADAFRUIT_IO_PORT = 8883

ADAFRUIT_IO_USERNAME = os.environ.get("ADAFRUIT_IO_USERNAME")
ADAFRUIT_IO_KEY = os.environ.get("ADAFRUIT_IO_KEY")

BASE_TOPIC = "fridgemanager"

BUZZER_ACK_TOPIC = f"{BASE_TOPIC}/buzzer/acknowledge"
BUZZER_STATE_TOPIC = f"{BASE_TOPIC}/buzzer/state"

LEARNING_CONTROL_TOPIC = f"{BASE_TOPIC}/learning/control"

PAIRING_CONTROL_TOPIC = f"{BASE_TOPIC}/pairing/control"
PAIRING_STATE_TOPIC = f"{BASE_TOPIC}/pairing/state"

LEARNED_TEMPERATURE_MARGIN = 0.5


# =====================================================
# SHARED STATE USED BY THE GATEWAY
# =====================================================

"""
The hardware module owns the alert sets because it also controls the buzzer.
Keeping references here lets MQTT and the database use the same alert state.
"""
active_alerts = hardware.active_alerts
acknowledged_alerts = hardware.acknowledged_alerts
alert_lock = hardware.alert_lock

"""
Stores when each sensor first reported an open door so the warning
only starts after the configured delay.
"""
door_open_since = {}

"""
Config versions stop the same settings being repeatedly sent to a node.
"confirmed" has been acknowledged by the ESP32; "queued" has been sent
but has not been acknowledged yet.
"""
confirmed_config_versions = {}
queued_config_versions = {}

"""
MQTT callbacks run separately from the serial loop, so commands are queued
here and written to the ESP32 gateway safely by the main serial loop.
"""
serial_command_queue = []
serial_command_lock = threading.Lock()


# =====================================================
# MQTT CALLBACKS
# =====================================================

def on_connect(
    client,
    userdata,
    flags,
    reason_code,
    properties=None,
):
    # Subscribe after connecting so dashboard/control messages can be received.
    if reason_code == 0:
        print(
            "Connected to MQTT broker"
        )

        client.subscribe(
            BUZZER_ACK_TOPIC,
            qos=1,
        )

        client.subscribe(
            LEARNING_CONTROL_TOPIC,
            qos=1,
        )

        client.subscribe(
            PAIRING_CONTROL_TOPIC,
            qos=1,
        )

        print(
            f"Listening for acknowledgements on "
            f"{BUZZER_ACK_TOPIC}"
        )

        print(
            f"Listening for learning commands on "
            f"{LEARNING_CONTROL_TOPIC}"
        )

        print(
            f"Listening for pairing commands on "
            f"{PAIRING_CONTROL_TOPIC}"
        )

    else:
        print(
            f"MQTT connection failed: "
            f"{reason_code}"
        )


def on_disconnect(
    client,
    userdata,
    disconnect_flags,
    reason_code,
    properties=None,
):
    print(
        "Disconnected from MQTT broker"
    )


def on_message(
    client,
    userdata,
    message,
):
    # All control messages arrive here. Decode once, then route by topic.
    payload = message.payload.decode(
        "utf-8",
        errors="ignore",
    ).strip()

    # Acknowledge silences alerts that are currently active without clearing them.
    if message.topic == BUZZER_ACK_TOPIC:
        if payload.lower() in {
            "1",
            "true",
            "yes",
            "ack",
            "acknowledge",
            "silence",
        }:
            acknowledge_current_alerts()

        return

    """
    Pairing commands from the UI/MQTT are converted to the simple serial
    protocol understood by the ESP32 gateway.
    """
    if message.topic == PAIRING_CONTROL_TOPIC:
        try:
            command_data = json.loads(
                payload
            )

        except json.JSONDecodeError:
            print(
                "Invalid pairing command JSON"
            )
            return

        command = str(
            command_data.get(
                "command",
                "",
            )
        ).upper()

        node_mac = command_data.get(
            "node_mac"
        )

        if command == "START":
            serial_command = (
                "PAIR:START\n"
            )

        elif command == "CANCEL":
            serial_command = (
                "PAIR:CANCEL\n"
            )

        elif command == "LIST":
            serial_command = (
                "PAIR:LIST\n"
            )

        elif command in {
            "ACCEPT",
            "REMEMBER",
            "FORGET",
        }:
            if not node_mac:
                print(
                    "Pairing command "
                    "missing node_mac"
                )
                return

            serial_command = (
                f"PAIR:{command},"
                f"{node_mac}\n"
            )

        else:
            print(
                "Invalid pairing command"
            )
            return

        with serial_command_lock:
            serial_command_queue.append(
                serial_command
            )

        print(
            f"Queued pairing command: "
            f"{command}"
        )

        return

    """
    Learning commands tell a sensor node to start/cancel its temperature
    learning mode. Modes 1-3 are the valid modes implemented by the node.
    """
    if message.topic == LEARNING_CONTROL_TOPIC:
        try:
            command_data = json.loads(
                payload
            )

        except json.JSONDecodeError:
            print(
                "Invalid learning command JSON"
            )
            return

        node_mac = command_data.get(
            "node_mac"
        )

        command = str(
            command_data.get(
                "command",
                "",
            )
        ).upper()

        mode = command_data.get(
            "mode",
            0,
        )

        if not node_mac:
            print(
                "Learning command "
                "missing node_mac"
            )
            return

        if command == "START":
            if mode not in {1, 2, 3}:
                print(
                    "Invalid learning mode"
                )
                return

        elif command == "CANCEL":
            mode = 0

        else:
            print(
                "Invalid learning command"
            )
            return

        serial_command = (
            f"LEARN:{node_mac},"
            f"{command},"
            f"{mode}\n"
        )

        with serial_command_lock:
            serial_command_queue.append(
                serial_command
            )

        print(
            f"Queued learning command: "
            f"{command} for {node_mac}"
        )


mqtt_client = mqtt.Client(
    mqtt.CallbackAPIVersion.VERSION2,
    client_id="fridgemanager-gateway",
)

mqtt_client.on_connect = on_connect
mqtt_client.on_disconnect = on_disconnect
mqtt_client.on_message = on_message


# =====================================================
# OPTIONAL ADAFRUIT IO CLOUD MQTT
# =====================================================

def on_adafruit_connect(
    client,
    userdata,
    flags,
    reason_code,
    properties=None,
):
    if reason_code == 0:
        print(
            "Connected to Adafruit IO"
        )

    else:
        print(
            f"Adafruit IO connection failed: "
            f"{reason_code}"
        )


def on_adafruit_disconnect(
    client,
    userdata,
    disconnect_flags,
    reason_code,
    properties=None,
):
    print(
        "Disconnected from Adafruit IO"
    )


adafruit_client = mqtt.Client(
    mqtt.CallbackAPIVersion.VERSION2,
    client_id="fridgemanager-adafruit",
)

adafruit_client.on_connect = (
    on_adafruit_connect
)

adafruit_client.on_disconnect = (
    on_adafruit_disconnect
)

"""
Cloud publishing is optional. Credentials are read from environment
variables rather than being stored directly in the source code.
"""
if (
    ADAFRUIT_IO_USERNAME
    and ADAFRUIT_IO_KEY
):
    adafruit_client.username_pw_set(
        ADAFRUIT_IO_USERNAME,
        ADAFRUIT_IO_KEY,
    )

    adafruit_client.tls_set(
        cert_reqs=ssl.CERT_REQUIRED,
        tls_version=ssl.PROTOCOL_TLS_CLIENT,
    )


# =====================================================
# SENSOR CONFIGURATION
# =====================================================

def build_sensor_config_command(
    node_mac,
    sensor,
):
    # The ESP32 protocol uses numbers instead of compartment names.
    compartment_map = {
        None: 0,
        "unassigned": 0,
        "fridge": 1,
        "freezer": 2,
    }

    compartment = compartment_map.get(
        sensor["compartment"],
        0,
    )

    command = (
        f"CONFIG:{node_mac},"
        f"{sensor['config_version']},"
        f"{compartment},"
        f"{sensor['min_temp']},"
        f"{sensor['max_temp']},"
        f"{sensor['temperature_hysteresis']},"
        f"{sensor['temperature_warning_seconds']},"
        f"{sensor['temperature_recovery_seconds']},"
        f"{sensor['door_warning_seconds']},"
        f"{1 if sensor['enabled'] else 0}\n"
    )

    return command


def send_sensor_config(
    ser,
    node_mac,
    sensor,
):
    desired_version = (
        sensor["config_version"]
    )

    confirmed_version = (
        confirmed_config_versions.get(
            node_mac
        )
    )

    queued_version = (
        queued_config_versions.get(
            node_mac
        )
    )

    """
    Do not resend a configuration the node already has or is already
    waiting to acknowledge.
    """
    if confirmed_version == desired_version:
        return

    if queued_version == desired_version:
        return

    command = build_sensor_config_command(
        node_mac,
        sensor,
    )

    ser.write(
        command.encode("utf-8")
    )

    ser.flush()

    queued_config_versions[
        node_mac
    ] = desired_version

    print(
        f"Queued config version "
        f"{desired_version} "
        f"for {node_mac}"
    )


# =====================================================
# ALERT MANAGEMENT
# =====================================================

def publish_buzzer_state():
    with alert_lock:
        active = sorted(
            f"{node_mac}:{alert_type}"
            for node_mac, alert_type
            in active_alerts
        )

        unacknowledged = sorted(
            f"{node_mac}:{alert_type}"
            for node_mac, alert_type
            in (
                active_alerts
                - acknowledged_alerts
            )
        )

    """
    The buzzer sounds only when at least one active alert has not been
    acknowledged. Active alerts are still reported after acknowledgement.
    """
    state = {
        "sounding": bool(
            unacknowledged
        ),
        "active_alerts": active,
        "unacknowledged_alerts": (
            unacknowledged
        ),
    }

    mqtt_client.publish(
        BUZZER_STATE_TOPIC,
        json.dumps(state),
        qos=1,
        retain=True,
    )


def acknowledge_current_alerts():
    """
    Silence the current hardware alerts and keep the database event history
    in sync with the acknowledgement.
    """
    acknowledged_count = (
        hardware.acknowledge_alerts()
    )

    try:
        mark_alert_events_acknowledged()

    except sqlite3.Error as error:
        print(
            "Could not acknowledge "
            "database events:"
        )
        print(error)

    print(
        f"Buzzer acknowledged "
        f"({acknowledged_count} "
        f"active alert(s))"
    )

    publish_buzzer_state()


def calculate_door_warning(
    node_mac,
    data,
    warning_seconds,
):
    door_open = bool(
        data.get("door_open")
    )

    """
    monotonic() is used for elapsed time because it is not affected by
    system clock changes.
    """
    now = time.monotonic()

    if not door_open:
        door_open_since.pop(
            node_mac,
            None,
        )

        return False

    if node_mac not in door_open_since:
        door_open_since[
            node_mac
        ] = now

    return (
        now
        - door_open_since[node_mac]
    ) >= warning_seconds


def update_alerts_for_packet(data):
    node_mac = data.get(
        "node_mac"
    )

    if not node_mac:
        return

    sensor = get_sensor_details(
        node_mac
    )

    if sensor is None:
        return

    device_name = (
        sensor["device_name"]
    )

    """
    Build the alerts that SHOULD be active for this packet, then compare
    them with the alerts that were active after the previous packet.
    """
    desired_alerts = {}

    if not sensor["enabled"]:
        door_open_since.pop(
            node_mac,
            None,
        )

    else:
        temperature = data.get(
            "temperature"
        )

        temperature_state = data.get(
            "temperature_state"
        )

        """
        DS18B20 failures commonly appear as an invalid/non-numeric value or
        approximately -127 C, so these readings are treated as sensor faults.
        """
        sensor_fault = (
            not isinstance(
                temperature,
                (int, float),
            )
            or temperature <= -126.0
        )

        if sensor_fault:
            desired_alerts[
                "sensor_fault"
            ] = (
                "critical",
                (
                    f"{device_name} "
                    f"temperature sensor fault"
                ),
            )

        else:
            minimum = sensor[
                "min_temp"
            ]

            maximum = sensor[
                "max_temp"
            ]

            """
            temperature_state is calculated by the node using the configured
            thresholds/persistence. State 2 means a confirmed temperature warning.
            """
            if temperature_state == 2:
                if (
                    minimum is not None
                    and maximum is not None
                ):
                    desired_alerts[
                        "temperature_warning"
                    ] = (
                        "critical",
                        (
                            f"{device_name} "
                            f"temperature is "
                            f"{temperature:.2f} C; "
                            f"safe configured range "
                            f"is {minimum:.1f} to "
                            f"{maximum:.1f} C"
                        ),
                    )

                else:
                    desired_alerts[
                        "temperature_warning"
                    ] = (
                        "critical",
                        (
                            f"{device_name} "
                            f"temperature warning"
                        ),
                    )

            """
            Once learning is trained, compare live temperature with the learned
            normal range plus a small margin to identify unusual behaviour.
            """
            learning_trained = bool(
                data.get(
                    "learning_trained",
                    False,
                )
            )

            learned_minimum = data.get(
                "learned_minimum_temperature"
            )

            learned_maximum = data.get(
                "learned_maximum_temperature"
            )

            learned_range_valid = (
                isinstance(
                    learned_minimum,
                    (int, float),
                )
                and isinstance(
                    learned_maximum,
                    (int, float),
                )
                and learned_minimum
                <= learned_maximum
            )

            if (
                learning_trained
                and learned_range_valid
            ):
                learned_low_threshold = (
                    learned_minimum
                    - LEARNED_TEMPERATURE_MARGIN
                )

                learned_high_threshold = (
                    learned_maximum
                    + LEARNED_TEMPERATURE_MARGIN
                )

                learned_anomaly = (
                    temperature
                    < learned_low_threshold
                    or temperature
                    > learned_high_threshold
                )

                if learned_anomaly:
                    desired_alerts[
                        "learned_temperature_anomaly"
                    ] = (
                        "warning",
                        (
                            f"{device_name} "
                            f"temperature is "
                            f"{temperature:.2f} C; "
                            f"learned range is "
                            f"{learned_minimum:.2f} to "
                            f"{learned_maximum:.2f} C "
                            f"with "
                            f"{LEARNED_TEMPERATURE_MARGIN:.1f} C "
                            f"margin"
                        ),
                    )

        # Door timing is checked on the Pi as another automation rule.
        door_warning = (
            calculate_door_warning(
                node_mac,
                data,
                sensor[
                    "door_warning_seconds"
                ],
            )
        )

        if door_warning:
            desired_alerts[
                "door_warning"
            ] = (
                "warning",
                (
                    f"{device_name} door "
                    f"has remained open for "
                    f"{sensor['door_warning_seconds']} "
                    f"seconds"
                ),
            )

    """
    Convert the newly calculated alerts into the same (node, type) format
    used by the shared active-alert set.
    """
    desired_keys = {
        (node_mac, alert_type)
        for alert_type
        in desired_alerts
    }

    with alert_lock:
        current_keys = {
            key
            for key in active_alerts
            if key[0] == node_mac
        }

        temperature_key = (
            node_mac,
            "temperature_warning",
        )

        sensor_fault_key = (
            node_mac,
            "sensor_fault",
        )

        learned_anomaly_key = (
            node_mac,
            "learned_temperature_anomaly",
        )

        """
        If the temperature sensor temporarily fails, keep an existing
        temperature/anomaly alert active until valid temperature data returns.
        This prevents a fault packet from falsely clearing a real warning.
        """
        if (
            sensor["enabled"]
            and sensor_fault_key
            in desired_keys
        ):
            if (
                temperature_key
                in current_keys
            ):
                desired_keys.add(
                    temperature_key
                )

            if (
                learned_anomaly_key
                in current_keys
            ):
                desired_keys.add(
                    learned_anomaly_key
                )

        # Set differences tell us exactly which alerts changed state.
        started = (
            desired_keys
            - current_keys
        )

        cleared = (
            current_keys
            - desired_keys
        )

    """
    The hardware module updates the shared sets and decides whether the
    physical buzzer should be on or off.
    """
    hardware.update_alert_sets(
        started,
        cleared,
    )

    for _, alert_type in started:
        if alert_type not in desired_alerts:
            continue

        severity, description = (
            desired_alerts[alert_type]
        )

        print(
            f"ALERT STARTED: "
            f"{description}"
        )

        record_event(
            alert_type,
            severity,
            device_name,
            description,
            acknowledged=0,
        )

    for _, alert_type in cleared:
        description = (
            f"{device_name} "
            f"{alert_type.replace('_', ' ')} "
            f"cleared"
        )

        print(
            f"ALERT CLEARED: "
            f"{description}"
        )

        record_event(
            f"{alert_type}_cleared",
            "info",
            device_name,
            description,
            acknowledged=1,
        )

    # Only republish buzzer state when an alert actually changes.
    if started or cleared:
        publish_buzzer_state()


# =====================================================
# MQTT PUBLISH
# =====================================================

def publish_adafruit_data(
    data,
    sensor,
):
    # Skip cloud publishing when Adafruit IO has not been configured.
    if (
        not ADAFRUIT_IO_USERNAME
        or not ADAFRUIT_IO_KEY
    ):
        return

    compartment = sensor.get(
        "compartment"
    )

    if compartment not in {
        "fridge",
        "freezer",
    }:
        return

    feed_base = (
        f"{ADAFRUIT_IO_USERNAME}"
        f"/feeds"
    )

    temperature = data.get(
        "temperature"
    )

    door_open = data.get(
        "door_open"
    )

    if (
        isinstance(
            temperature,
            (int, float),
        )
        and temperature > -126.0
    ):
        adafruit_client.publish(
            f"{feed_base}/"
            f"{compartment}-temperature",
            str(temperature),
            qos=1,
        )

    if isinstance(
        door_open,
        bool,
    ):
        adafruit_client.publish(
            f"{feed_base}/"
            f"{compartment}-door",
            (
                "1"
                if door_open
                else "0"
            ),
            qos=1,
        )

    """
    Adafruit receives one simple warning flag per compartment if any alert
    belonging to that node is active.
    """
    warning_active = any(
        alert_node_mac
        == data.get("node_mac")
        for alert_node_mac, alert_type
        in active_alerts
    )

    adafruit_client.publish(
        f"{feed_base}/"
        f"{compartment}-warning",
        (
            "1"
            if warning_active
            else "0"
        ),
        qos=1,
    )


def publish_sensor_data(data):
    node_mac = data.get(
        "node_mac"
    )

    if not node_mac:
        print(
            "Packet missing node_mac"
        )
        return

    # Remove colons from the MAC address to make a clean MQTT topic name.
    node_topic = (
        node_mac
        .replace(":", "")
        .lower()
    )

    base = (
        f"{BASE_TOPIC}/sensor/"
        f"{node_topic}"
    )

    """
    Publish the complete packet for consumers that need all fields, then
    publish individual retained values for simple dashboard subscriptions.
    """
    mqtt_client.publish(
        f"{base}/state",
        json.dumps(data),
        qos=1,
        retain=False,
    )

    if "temperature" in data:
        mqtt_client.publish(
            f"{base}/temperature",
            str(data["temperature"]),
            qos=1,
            retain=True,
        )

    if "door_open" in data:
        mqtt_client.publish(
            f"{base}/door",
            (
                "open"
                if data["door_open"]
                else "closed"
            ),
            qos=1,
            retain=True,
        )

    if "door_warning" in data:
        mqtt_client.publish(
            f"{base}/node_door_warning",
            (
                "true"
                if data["door_warning"]
                else "false"
            ),
            qos=1,
            retain=True,
        )

    if "wake_reason" in data:
        mqtt_client.publish(
            f"{base}/wake_reason",
            str(data["wake_reason"]),
            qos=0,
            retain=False,
        )

    if "temperature_state" in data:
        mqtt_client.publish(
            f"{base}/temperature_state",
            str(
                data[
                    "temperature_state"
                ]
            ),
            qos=1,
            retain=True,
        )

    if "learning_mode" in data:
        mqtt_client.publish(
            f"{base}/learning_mode",
            str(
                data[
                    "learning_mode"
                ]
            ),
            qos=1,
            retain=True,
        )

    if "learning_active" in data:
        mqtt_client.publish(
            f"{base}/learning_active",
            (
                "true"
                if data[
                    "learning_active"
                ]
                else "false"
            ),
            qos=1,
            retain=True,
        )

    if "learning_trained" in data:
        mqtt_client.publish(
            f"{base}/learning_trained",
            (
                "true"
                if data[
                    "learning_trained"
                ]
                else "false"
            ),
            qos=1,
            retain=True,
        )

    if (
        "learning_elapsed_seconds"
        in data
    ):
        mqtt_client.publish(
            f"{base}/"
            f"learning_elapsed_seconds",
            str(
                data[
                    "learning_elapsed_seconds"
                ]
            ),
            qos=1,
            retain=True,
        )

    if (
        "learning_sample_count"
        in data
    ):
        mqtt_client.publish(
            f"{base}/"
            f"learning_sample_count",
            str(
                data[
                    "learning_sample_count"
                ]
            ),
            qos=1,
            retain=True,
        )

    if (
        "learned_minimum_temperature"
        in data
    ):
        mqtt_client.publish(
            f"{base}/"
            f"learned_minimum_temperature",
            str(
                data[
                    "learned_minimum_temperature"
                ]
            ),
            qos=1,
            retain=True,
        )

    if (
        "learned_maximum_temperature"
        in data
    ):
        mqtt_client.publish(
            f"{base}/"
            f"learned_maximum_temperature",
            str(
                data[
                    "learned_maximum_temperature"
                ]
            ),
            qos=1,
            retain=True,
        )

    print(
        f"Published MQTT data "
        f"for {node_mac}"
    )


# =====================================================
# SERIAL PROCESSING
# =====================================================

def send_queued_serial_commands(ser):
    """
    Copy and clear the queue while holding the lock, then release the lock
    before performing the slower serial writes.
    """
    with serial_command_lock:
        commands = (
            serial_command_queue[:]
        )

        serial_command_queue.clear()

    for command in commands:
        ser.write(
            command.encode("utf-8")
        )

        ser.flush()

        print(
            f"Sent to gateway: "
            f"{command.strip()}"
        )


def process_serial_line(
    line,
    ser,
):
    """
    The ESP32 gateway sends several plain-text status messages during
    pairing. They are converted into JSON state for the MQTT/UI side.
    """
    pairing_messages = {
        "PAIRING_STARTED": "searching",
        "PAIRING_CANCELLED": "cancelled",
        "PAIRING_TIMEOUT": "timeout",
    }

    if line in pairing_messages:
        state = {
            "status": pairing_messages[
                line
            ],
            "node_mac": None,
        }

        mqtt_client.publish(
            PAIRING_STATE_TOPIC,
            json.dumps(state),
            qos=1,
            retain=True,
        )

        print(
            f"Pairing state: "
            f"{state['status']}"
        )

        return

    # Other pairing responses include JSON after a known text prefix.
    pairing_prefixes = {
        "PAIR_FOUND:": "found",
        "PAIRING_COMPLETE:": "complete",
        "PAIR_REMEMBERED:": "remembered",
        "PAIR_FORGOTTEN:": "forgotten",
        "KNOWN_SENSOR_REPAIR:": "repairing",
        "PAIR_RESTORED:": "restored",
    }

    for prefix, status in (
        pairing_prefixes.items()
    ):
        if line.startswith(prefix):
            json_text = line[
                len(prefix):
            ]

            try:
                pairing_data = json.loads(
                    json_text
                )

            except json.JSONDecodeError:
                print(
                    "Invalid pairing JSON:"
                )
                print(
                    json_text
                )
                return

            state = {
                "status": status,
                "node_mac": (
                    pairing_data.get(
                        "node_mac"
                    )
                ),
            }

            mqtt_client.publish(
                PAIRING_STATE_TOPIC,
                json.dumps(state),
                qos=1,
                retain=True,
            )

            print(
                f"Pairing state: "
                f"{status} "
                f"{state['node_mac']}"
            )

            return

    if line.startswith(
        "PAIR_LIST:"
    ):
        json_text = line[
            len("PAIR_LIST:"):
        ]

        try:
            pairing_data = json.loads(
                json_text
            )

        except json.JSONDecodeError:
            print(
                "Invalid PAIR_LIST JSON:"
            )
            print(
                json_text
            )
            return

        state = {
            "status": "list",
            "count": pairing_data.get(
                "count",
                0,
            ),
            "sensors": pairing_data.get(
                "sensors",
                [],
            ),
        }

        mqtt_client.publish(
            PAIRING_STATE_TOPIC,
            json.dumps(state),
            qos=1,
            retain=True,
        )

        print(
            f"Known sensor list: "
            f"{state['sensors']}"
        )

        return

    if line.startswith(
        "PAIR_ERROR:"
    ):
        error_message = line[
            len("PAIR_ERROR:"):
        ]

        state = {
            "status": "error",
            "error": error_message,
            "node_mac": None,
        }

        mqtt_client.publish(
            PAIRING_STATE_TOPIC,
            json.dumps(state),
            qos=1,
            retain=True,
        )

        print(
            f"Pairing error: "
            f"{error_message}"
        )

        return

    """
    A CONFIG_ACK proves the sensor node received a particular settings
    version, allowing that version to be removed from the queued list.
    """
    if line.startswith(
        "CONFIG_ACK:"
    ):
        json_text = line[
            len("CONFIG_ACK:"):
        ]

        try:
            ack = json.loads(
                json_text
            )

        except json.JSONDecodeError:
            print(
                "Invalid CONFIG_ACK received:"
            )
            print(
                json_text
            )
            return

        node_mac = ack.get(
            "node_mac"
        )

        config_version = ack.get(
            "config_version"
        )

        if (
            node_mac
            and isinstance(
                config_version,
                int,
            )
        ):
            confirmed_config_versions[
                node_mac
            ] = config_version

            if (
                queued_config_versions.get(
                    node_mac
                )
                == config_version
            ):
                queued_config_versions.pop(
                    node_mac,
                    None,
                )

            print(
                f"Sensor {node_mac} "
                f"confirmed config version "
                f"{config_version}"
            )

        return

    """
    Normal sensor packets are prefixed with JSON:. Any unrelated serial
    debugging output is ignored.
    """
    if not line.startswith(
        "JSON:"
    ):
        return

    json_text = line[5:]

    try:
        data = json.loads(
            json_text
        )

    except json.JSONDecodeError:
        print(
            "Invalid JSON received:"
        )
        print(
            json_text
        )
        return

    print(
        "Sensor packet received:"
    )

    print(
        data
    )

    node_mac = data.get(
        "node_mac"
    )

    if node_mac:
        try:
            """
            Seeing a valid packet registers a new sensor or refreshes the
            existing sensor record in the database.
            """
            register_or_update_sensor(
                node_mac
            )

            sensor = get_sensor_details(
                node_mac
            )

            temperature = data.get(
                "temperature"
            )

            """
            Only valid temperatures from enabled sensors are stored as
            historical readings. Fault values are not added to the history.
            """
            if (
                sensor is not None
                and sensor["enabled"]
                and isinstance(
                    temperature,
                    (int, float),
                )
                and temperature > -126.0
            ):
                record_sensor_reading(
                    sensor["sensor_id"],
                    temperature,
                )

            # Use the packet to start/clear alerts before publishing cloud state.
            update_alerts_for_packet(
                data
            )

            """
            Send updated settings back to the sensor when its database config
            version differs from the version already confirmed by the node.
            """
            if sensor is not None:
                send_sensor_config(
                    ser,
                    node_mac,
                    sensor,
                )

        except sqlite3.Error as error:
            print(
                "Database error:"
            )
            print(
                error
            )

    # Local MQTT is always updated, even if the database operation above fails.
    publish_sensor_data(
        data
    )

    if node_mac:
        sensor = get_sensor_details(
            node_mac
        )

        if sensor is not None:
            publish_adafruit_data(
                data,
                sensor,
            )


# =====================================================
# MAIN LOOP
# =====================================================

def main():
    # Start GPIO/hardware handling before accepting sensor alerts.
    hardware.start()

    # Local MQTT connects the gateway service to the FastAPI/dashboard side.
    mqtt_client.connect(
        MQTT_BROKER,
        MQTT_PORT,
        keepalive=60,
    )

    mqtt_client.loop_start()

    publish_buzzer_state()

    # Start the optional cloud MQTT connection separately from local MQTT.
    if (
        ADAFRUIT_IO_USERNAME
        and ADAFRUIT_IO_KEY
    ):
        try:
            adafruit_client.connect(
                ADAFRUIT_IO_HOST,
                ADAFRUIT_IO_PORT,
                keepalive=60,
            )

            adafruit_client.loop_start()

        except Exception as error:
            print(
                "Could not connect "
                "to Adafruit IO:"
            )
            print(
                error
            )

    else:
        print(
            "Adafruit IO credentials "
            "not available"
        )

    try:
        while True:
            try:
                print(
                    f"Opening gateway on "
                    f"{SERIAL_PORT}..."
                )

                # Reopen the USB serial connection automatically after disconnects.
                with serial.Serial(
                    SERIAL_PORT,
                    BAUD_RATE,
                    timeout=1,
                ) as ser:
                    print(
                        "Gateway connected"
                    )

                    while True:
                        """
                        Send UI/control commands first, then process the next line
                        received from the ESP32 gateway.
                        """
                        send_queued_serial_commands(
                            ser
                        )

                        line = (
                            ser.readline()
                            .decode(
                                "utf-8",
                                errors="ignore",
                            )
                            .strip()
                        )

                        if not line:
                            continue

                        process_serial_line(
                            line,
                            ser,
                        )

            except serial.SerialException as error:
                print(
                    "Serial gateway disconnected:"
                )
                print(
                    error
                )

                print(
                    "Retrying in 5 seconds..."
                )

                time.sleep(
                    5
                )

    except KeyboardInterrupt:
        print()

        print(
            "Stopping gateway service"
        )

    finally:
        # Always leave GPIO, MQTT threads and network connections cleanly closed.
        hardware.stop()

        mqtt_client.loop_stop()
        mqtt_client.disconnect()

        if (
            ADAFRUIT_IO_USERNAME
            and ADAFRUIT_IO_KEY
        ):
            adafruit_client.loop_stop()
            adafruit_client.disconnect()


if __name__ == "__main__":
    main()
