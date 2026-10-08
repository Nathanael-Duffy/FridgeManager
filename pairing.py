# =====================================================
# PAIRING
# =====================================================
"""
Provides the API workflow for discovering, registering, remembering and forgetting ESP32 sensor nodes.
"""

import json
import sqlite3

from fastapi import APIRouter, HTTPException

from backend.database import get_connection
from backend.models import SensorPairingRegistration
from backend.mqtt_client import mqtt_client
import backend.mqtt_client as mqtt_state


router = APIRouter()


# Returns the current pairing/discovery state maintained through MQTT.
@router.get("/pairing")
def get_pairing_state():
    return mqtt_state.pairing_state


# Requests that the ESP32 gateway enter pairing mode so a sensor can be discovered.
@router.post("/pairing/start")
def start_pairing():
    mqtt_state.pairing_state = {
        "status": "starting",
        "node_mac": None,
        "sensors": []
    }

    mqtt_client.publish(
        "fridgemanager/pairing/control",
        json.dumps({
            "command": "START"
        }),
        qos=1
    )

    return {
        "message": "Pairing started"
    }


# Stops the current pairing attempt and clears the pending discovery state.
@router.post("/pairing/cancel")
def cancel_pairing():
    mqtt_client.publish(
        "fridgemanager/pairing/control",
        json.dumps({
            "command": "CANCEL"
        }),
        qos=1
    )

    return {
        "message": "Pairing cancelled"
    }


# Returns sensors currently stored as trusted/known by the gateway pairing workflow.
@router.get("/pairing/trusted")
def get_trusted_sensors():
    mqtt_client.publish(
        "fridgemanager/pairing/control",
        json.dumps({
            "command": "LIST"
        }),
        qos=1
    )

    return {
        "message": "Trusted sensor list requested"
    }


# Accepts a discovered sensor, assigns its Fridge Manager details and completes registration.
@router.post("/pairing/register")
def register_paired_sensor(
    registration: SensorPairingRegistration
):
    node_mac = mqtt_state.pairing_state.get(
        "node_mac"
    )

    if (
        mqtt_state.pairing_state.get("status") != "found"
        or not node_mac
    ):
        raise HTTPException(
            status_code=409,
            detail=(
                "No sensor is waiting to be registered"
            )
        )

    device_name = registration.device_name.strip()

    if not device_name:
        raise HTTPException(
            status_code=400,
            detail="Sensor name is required"
        )

    compartment = (
        registration.compartment
        .strip()
        .lower()
    )

    if compartment not in (
        "fridge",
        "freezer"
    ):
        raise HTTPException(
            status_code=400,
            detail=(
                "Compartment must be Fridge or Freezer"
            )
        )

    if compartment == "fridge":
        min_temp = 1.0
        max_temp = 5.0
    else:
        min_temp = -24.0
        max_temp = -18.0

    connection = get_connection()
    cursor = connection.cursor()

    try:
        existing = cursor.execute(
            """
            SELECT sensor_id
            FROM Sensors
            WHERE mac_address = ?
            """,
            (node_mac,)
        ).fetchone()

        if existing is None:
            cursor.execute(
                """
                INSERT INTO Sensors
                (
                    mac_address,
                    device_name,
                    compartment,
                    status,
                    min_temp,
                    max_temp,
                    door_warning_seconds,
                    temperature_hysteresis,
                    temperature_warning_seconds,
                    temperature_recovery_seconds,
                    enabled,
                    config_version
                )
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    node_mac,
                    device_name,
                    compartment,
                    "active",
                    min_temp,
                    max_temp,
                    30,
                    1.0,
                    900,
                    300,
                    1,
                    1
                )
            )

            sensor_id = cursor.lastrowid

        else:
            sensor_id = existing["sensor_id"]

            cursor.execute(
                """
                UPDATE Sensors
                SET
                    device_name = ?,
                    compartment = ?,
                    status = ?,
                    min_temp = ?,
                    max_temp = ?,
                    door_warning_seconds = ?,
                    temperature_hysteresis = ?,
                    temperature_warning_seconds = ?,
                    temperature_recovery_seconds = ?,
                    enabled = 1,
                    config_version = config_version + 1
                WHERE sensor_id = ?
                """,
                (
                    device_name,
                    compartment,
                    "active",
                    min_temp,
                    max_temp,
                    30,
                    1.0,
                    900,
                    300,
                    sensor_id
                )
            )

        connection.commit()

    except sqlite3.IntegrityError as error:
        connection.rollback()

        raise HTTPException(
            status_code=400,
            detail=(
                f"Database integrity error: {error}"
            )
        )

    finally:
        connection.close()

    mqtt_client.publish(
        "fridgemanager/pairing/control",
        json.dumps({
            "command": "ACCEPT",
            "node_mac": node_mac
        }),
        qos=1
    )

    return {
        "message": "Sensor registration accepted",
        "sensor_id": sensor_id,
        "node_mac": node_mac,
        "device_name": device_name,
        "compartment": compartment
    }


# Asks the gateway to add an existing database sensor back to its persistent known-sensor list.
@router.post("/pairing/remember/{sensor_id}")
def remember_existing_sensor(sensor_id: int):
    connection = get_connection()

    sensor = connection.execute(
        """
        SELECT
            sensor_id,
            mac_address,
            device_name
        FROM Sensors
        WHERE sensor_id = ?
        """,
        (sensor_id,)
    ).fetchone()

    connection.close()

    if sensor is None:
        raise HTTPException(
            status_code=404,
            detail="Sensor not found"
        )

    mqtt_client.publish(
        "fridgemanager/pairing/control",
        json.dumps({
            "command": "REMEMBER",
            "node_mac": sensor["mac_address"]
        }),
        qos=1
    )

    return {
        "message": "Sensor trust command sent",
        "sensor_id": sensor_id,
        "node_mac": sensor["mac_address"]
    }


# Removes a sensor from the gateway trusted list while retaining the backend record as required by the route logic.
@router.post("/pairing/forget/{sensor_id}")
def forget_existing_sensor(sensor_id: int):
    connection = get_connection()

    sensor = connection.execute(
        """
        SELECT
            sensor_id,
            mac_address,
            device_name
        FROM Sensors
        WHERE sensor_id = ?
        """,
        (sensor_id,)
    ).fetchone()

    connection.close()

    if sensor is None:
        raise HTTPException(
            status_code=404,
            detail="Sensor not found"
        )

    mqtt_client.publish(
        "fridgemanager/pairing/control",
        json.dumps({
            "command": "FORGET",
            "node_mac": sensor["mac_address"]
        }),
        qos=1
    )

    return {
        "message": "Sensor forget command sent",
        "sensor_id": sensor_id,
        "node_mac": sensor["mac_address"]
    }
