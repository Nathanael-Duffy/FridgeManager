# =====================================================
# SENSORS
# =====================================================
"""
Provides sensor configuration, readings and temperature-learning API endpoints used by the Settings and dashboard interfaces.
"""

import json

from fastapi import APIRouter, HTTPException

from backend.database import get_connection
from backend.models import (
    SensorConfig,
    LearningStart,
    SensorReading,
)
from backend.mqtt_client import (
    mqtt_client,
    learning_states,
)


router = APIRouter()


# Returns all configured sensor records for the dashboard and Settings interface.
@router.get("/sensors")
def get_sensors():
    connection = get_connection()

    rows = connection.execute(
        "SELECT * FROM Sensors ORDER BY sensor_id"
    ).fetchall()

    connection.close()

    return [dict(row) for row in rows]


# Returns one sensor configuration or a 404 response when the requested sensor does not exist.
@router.get("/sensors/{sensor_id}")
def get_sensor(sensor_id: int):
    connection = get_connection()

    row = connection.execute(
        "SELECT * FROM Sensors WHERE sensor_id = ?",
        (sensor_id,)
    ).fetchone()

    connection.close()

    if row is None:
        raise HTTPException(
            status_code=404,
            detail="Sensor not found"
        )

    return dict(row)


# Validates a sensor configuration, stores it locally and publishes the versioned configuration for delivery to the node.
@router.put("/sensors/{sensor_id}/config")
def configure_sensor(
    sensor_id: int,
    config: SensorConfig
):
    compartment = (
        config.compartment
        .strip()
        .lower()
    )

    if compartment not in ("fridge", "freezer"):
        raise HTTPException(
            status_code=400,
            detail="Compartment must be Fridge or Freezer"
        )

    if config.min_temp >= config.max_temp:
        raise HTTPException(
            status_code=400,
            detail=(
                "Minimum temperature must be lower "
                "than maximum temperature"
            )
        )

    if config.door_warning_seconds < 1:
        raise HTTPException(
            status_code=400,
            detail=(
                "Door warning must be at least 1 second"
            )
        )

    if config.enabled not in (0, 1):
        raise HTTPException(
            status_code=400,
            detail="Enabled must be 0 or 1"
        )

    connection = get_connection()
    cursor = connection.cursor()

    cursor.execute(
        """
        SELECT sensor_id
        FROM Sensors
        WHERE sensor_id = ?
        """,
        (sensor_id,)
    )

    if cursor.fetchone() is None:
        connection.close()

        raise HTTPException(
            status_code=404,
            detail="Sensor not found"
        )

    cursor.execute("""
        UPDATE Sensors
        SET
            device_name = ?,
            compartment = ?,
            min_temp = ?,
            max_temp = ?,
            door_warning_seconds = ?,
            temperature_hysteresis = ?,
            temperature_warning_seconds = ?,
            temperature_recovery_seconds = ?,
            enabled = ?,
            config_version = config_version + 1
        WHERE sensor_id = ?
    """, (
        config.device_name,
        compartment,
        config.min_temp,
        config.max_temp,
        config.door_warning_seconds,
        config.temperature_hysteresis,
        config.temperature_warning_seconds,
        config.temperature_recovery_seconds,
        config.enabled,
        sensor_id,
    ))

    connection.commit()

    updated_sensor = cursor.execute(
        """
        SELECT *
        FROM Sensors
        WHERE sensor_id = ?
        """,
        (sensor_id,)
    ).fetchone()

    connection.close()

    return {
        "message": "Sensor configuration updated",
        "sensor": dict(updated_sensor)
    }


# Returns the latest learning state/profile associated with a sensor.
@router.get("/sensors/{sensor_id}/learning")
def get_sensor_learning(sensor_id: int):
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

    state = learning_states.get(
        sensor["mac_address"]
    )

    if state is None:
        return {
            "sensor_id": sensor_id,
            "node_mac": sensor["mac_address"],
            "device_name": sensor["device_name"],
            "online_state_available": False,
            "learning_mode": 1,
            "learning_active": False,
            "learning_trained": False,
            "learning_elapsed_seconds": 0,
            "learning_sample_count": 0,
            "temperature_state": None,
        }

    return {
        "sensor_id": sensor_id,
        "node_mac": sensor["mac_address"],
        "device_name": sensor["device_name"],
        "online_state_available": True,
        **state,
    }


# Validates a learning request and publishes the command that starts learning on the selected sensor node.
@router.post("/sensors/{sensor_id}/learning/start")
def start_sensor_learning(
    sensor_id: int,
    request: LearningStart
):
    if request.mode not in (1, 2, 3):
        raise HTTPException(
            status_code=400,
            detail=(
                "Learning mode must be "
                "1, 2 or 3"
            )
        )

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

    command = {
        "node_mac": sensor["mac_address"],
        "command": "START",
        "mode": request.mode,
    }

    mqtt_client.publish(
        "fridgemanager/learning/control",
        json.dumps(command),
        qos=1,
    )

    return {
        "message": "Learning start command sent",
        "sensor_id": sensor_id,
        "node_mac": sensor["mac_address"],
        "mode": request.mode,
    }


# Publishes a command that cancels active learning on the selected sensor node.
@router.post("/sensors/{sensor_id}/learning/cancel")
def cancel_sensor_learning(sensor_id: int):
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

    command = {
        "node_mac": sensor["mac_address"],
        "command": "CANCEL",
        "mode": 0,
    }

    mqtt_client.publish(
        "fridgemanager/learning/control",
        json.dumps(command),
        qos=1,
    )

    return {
        "message": "Learning cancel command sent",
        "sensor_id": sensor_id,
        "node_mac": sensor["mac_address"],
    }


# Returns recommended starting configuration values for a fridge or freezer compartment.
@router.get("/sensor-defaults/{compartment}")
def get_sensor_defaults(compartment: str):
    compartment = (
        compartment
        .strip()
        .lower()
    )

    if compartment == "fridge":
        return {
            "compartment": "fridge",
            "min_temp": 1.0,
            "max_temp": 5.0,
            "door_warning_seconds": 30,
            "temperature_hysteresis": 1.0,
            "temperature_warning_seconds": 900,
            "temperature_recovery_seconds": 300
        }

    if compartment == "freezer":
        return {
            "compartment": "freezer",
            "min_temp": -24.0,
            "max_temp": -18.0,
            "door_warning_seconds": 30,
            "temperature_hysteresis": 1.0,
            "temperature_warning_seconds": 900,
            "temperature_recovery_seconds": 300
        }

    raise HTTPException(
        status_code=400,
        detail="Compartment must be Fridge or Freezer"
    )


# Returns stored temperature readings used by the dashboard and history views.
@router.get("/sensor-readings")
def get_sensor_readings():
    connection = get_connection()

    rows = connection.execute(
        """
        SELECT *
        FROM SensorReadings
        ORDER BY reading_id DESC
        """
    ).fetchall()

    connection.close()

    return [dict(row) for row in rows]


# Stores a temperature reading supplied through the API.
@router.post("/sensor-readings")
def add_sensor_reading(reading: SensorReading):
    connection = get_connection()
    cursor = connection.cursor()

    existing = cursor.execute(
        """
        SELECT sensor_id
        FROM Sensors
        WHERE sensor_id = ?
        """,
        (reading.sensor_id,)
    ).fetchone()

    if existing is None:
        connection.close()

        raise HTTPException(
            status_code=404,
            detail="Sensor not found"
        )

    cursor.execute(
        """
        INSERT INTO SensorReadings
        (sensor_id, value, timestamp)
        VALUES (?, ?, ?)
        """,
        (
            reading.sensor_id,
            reading.value,
            reading.timestamp
        )
    )

    connection.commit()
    new_id = cursor.lastrowid
    connection.close()

    return {
        "message": "Sensor reading added",
        "reading_id": new_id
    }
