# =====================================================
# MQTT CLIENT
# =====================================================
"""
Maintains the backend MQTT connection and converts incoming gateway state messages into shared application state.
"""

import json
import paho.mqtt.client as mqtt


door_states = {}
door_warnings = {}
learning_states = {}

pairing_state = {
    "status": "idle",
    "node_mac": None,
    "sensors": []
}

buzzer_state = {
    "sounding": False,
    "active_alerts": [],
    "unacknowledged_alerts": []
}


# Handles a successful backend MQTT connection and subscribes to the topics needed by the API.
def on_mqtt_connect(
    client,
    userdata,
    flags,
    reason_code,
    properties
):
    client.subscribe(
        "fridgemanager/sensor/+/door"
    )

    client.subscribe(
        "fridgemanager/sensor/+/node_door_warning"
    )

    client.subscribe(
        "fridgemanager/sensor/+/state"
    )

    client.subscribe(
        "fridgemanager/buzzer/state"
    )

    client.subscribe(
        "fridgemanager/pairing/state"
    )


# Processes MQTT messages and updates the shared state consumed by FastAPI routes.
def on_mqtt_message(
    client,
    userdata,
    msg
):
    global buzzer_state
    global pairing_state

    if msg.topic == "fridgemanager/pairing/state":
        try:
            pairing_state = json.loads(
                msg.payload.decode()
            )
        except (
            json.JSONDecodeError,
            UnicodeDecodeError
        ):
            pass

        return

    if msg.topic == "fridgemanager/buzzer/state":
        try:
            buzzer_state = json.loads(
                msg.payload.decode()
            )
        except (
            json.JSONDecodeError,
            UnicodeDecodeError
        ):
            pass

        return

    parts = msg.topic.split("/")

    if len(parts) != 4:
        return

    mac = parts[2]
    message_type = parts[3]

    if message_type == "state":
        try:
            data = json.loads(
                msg.payload.decode()
            )
        except (
            json.JSONDecodeError,
            UnicodeDecodeError
        ):
            return

        node_mac = data.get("node_mac")

        if not node_mac:
            return

        learning_states[node_mac] = {
            "learning_mode": data.get(
                "learning_mode",
                1
            ),
            "learning_active": data.get(
                "learning_active",
                False
            ),
            "learning_trained": data.get(
                "learning_trained",
                False
            ),
            "learning_elapsed_seconds": data.get(
                "learning_elapsed_seconds",
                0
            ),
            "learning_sample_count": data.get(
                "learning_sample_count",
                0
            ),
            "temperature_state": data.get(
                "temperature_state",
                0
            ),
        }

        return

    payload = (
        msg.payload
        .decode()
        .strip()
        .lower()
    )

    if message_type == "door":
        door_states[mac] = payload

    elif message_type == "node_door_warning":
        door_warnings[mac] = (
            payload == "true"
        )


mqtt_client = mqtt.Client(
    mqtt.CallbackAPIVersion.VERSION2,
    client_id="fridgemanager-api"
)

mqtt_client.on_connect = on_mqtt_connect
mqtt_client.on_message = on_mqtt_message


# Starts the MQTT client in the background so API requests can use live gateway state.
def start_mqtt():
    mqtt_client.connect(
        "localhost",
        1883,
        60
    )

    mqtt_client.loop_start()
