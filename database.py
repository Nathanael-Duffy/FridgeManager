# =====================================================
# DATABASE
# =====================================================
"""
Contains the gateway-side SQLite operations used to register sensors, store readings and record alert events.
"""

import sqlite3

from datetime import datetime, timezone


DB_PATH = "/home/user1/fridge-manager/FridgeManager.db"


# Creates a sensor database record for a new MAC address or refreshes the existing sensor registration.
def register_or_update_sensor(node_mac):
    now = datetime.now(
        timezone.utc
    ).isoformat()

    with sqlite3.connect(DB_PATH) as conn:
        cursor = conn.cursor()

        cursor.execute(
            """
            SELECT sensor_id
            FROM Sensors
            WHERE mac_address = ?
            """,
            (node_mac,),
        )

        row = cursor.fetchone()

        if row is None:
            cursor.execute(
                """
                INSERT INTO Sensors
                (
                    mac_address,
                    device_name,
                    compartment,
                    status,
                    last_seen
                )
                VALUES (?, ?, NULL, ?, ?)
                """,
                (
                    node_mac,
                    "Unassigned",
                    "active",
                    now,
                ),
            )

            print(
                f"Registered new sensor: "
                f"{node_mac}"
            )

        else:
            cursor.execute(
                """
                UPDATE Sensors
                SET status = ?,
                    last_seen = ?
                WHERE mac_address = ?
                """,
                (
                    "active",
                    now,
                    node_mac,
                ),
            )

        conn.commit()


# Retrieves the stored sensor configuration associated with a gateway node MAC address.
def get_sensor_details(node_mac):
    with sqlite3.connect(DB_PATH) as conn:
        conn.row_factory = sqlite3.Row

        row = conn.execute(
            """
            SELECT sensor_id,
                   device_name,
                   compartment,
                   min_temp,
                   max_temp,
                   door_warning_seconds,
                   enabled,
                   temperature_hysteresis,
                   temperature_warning_seconds,
                   temperature_recovery_seconds,
                   config_version
            FROM Sensors
            WHERE mac_address = ?
            """,
            (node_mac,),
        ).fetchone()

    if row is None:
        return None

    return {
        "sensor_id": row["sensor_id"],
        "device_name": (
            row["device_name"]
            or node_mac
        ),
        "compartment": (
            row["compartment"].strip().lower()
            if row["compartment"]
            else None
        ),
        "min_temp": row["min_temp"],
        "max_temp": row["max_temp"],
        "door_warning_seconds": (
            row["door_warning_seconds"]
            if row["door_warning_seconds"]
            is not None
            else 30
        ),
        "enabled": bool(
            row["enabled"]
        ),
        "temperature_hysteresis": (
            row["temperature_hysteresis"]
            if row["temperature_hysteresis"]
            is not None
            else 1.0
        ),
        "temperature_warning_seconds": (
            row["temperature_warning_seconds"]
            if row["temperature_warning_seconds"]
            is not None
            else 900
        ),
        "temperature_recovery_seconds": (
            row["temperature_recovery_seconds"]
            if row["temperature_recovery_seconds"]
            is not None
            else 300
        ),
        "config_version": (
            row["config_version"]
            if row["config_version"]
            is not None
            else 1
        ),
    }


# Stores a sensor temperature sample in SensorReadings for later display and history.
def record_sensor_reading(
    sensor_id,
    value,
):
    now = datetime.now(
        timezone.utc
    ).isoformat()

    with sqlite3.connect(DB_PATH) as conn:
        conn.execute(
            """
            INSERT INTO SensorReadings
            (sensor_id, value, timestamp)
            VALUES (?, ?, ?)
            """,
            (
                sensor_id,
                value,
                now,
            ),
        )

        conn.commit()


# Writes an alert or system event to the Events table for history and acknowledgement.
def record_event(
    event_type,
    severity,
    source,
    description,
    acknowledged=0,
):
    now = datetime.now(
        timezone.utc
    ).isoformat()

    with sqlite3.connect(DB_PATH) as conn:
        conn.execute(
            """
            INSERT INTO Events
            (
                event_type,
                severity,
                source,
                description,
                timestamp,
                acknowledged
            )
            VALUES (?, ?, ?, ?, ?, ?)
            """,
            (
                event_type,
                severity,
                source,
                description,
                now,
                acknowledged,
            ),
        )

        conn.commit()


# Marks currently active alert events as acknowledged when the user silences them.
def mark_alert_events_acknowledged():
    with sqlite3.connect(DB_PATH) as conn:
        conn.execute(
            """
            UPDATE Events
            SET acknowledged = 1
            WHERE acknowledged = 0
              AND event_type IN
              (
                  'temperature_warning',
                  'door_warning',
                  'sensor_fault',
                  'learned_temperature_anomaly'
              )
            """
        )

        conn.commit()
