# =====================================================
# EVENTS
# =====================================================
"""
Provides API endpoints for reading, creating and acknowledging recorded Fridge Manager events.
"""

from fastapi import APIRouter, HTTPException

from backend.database import get_connection
from backend.models import Event


router = APIRouter()


# Returns recorded events in the order expected by the Alerts interface.
@router.get("/events")
def get_events():
    connection = get_connection()

    rows = connection.execute(
        "SELECT * FROM Events ORDER BY event_id DESC"
    ).fetchall()

    connection.close()

    return [
        dict(row)
        for row in rows
    ]


# Stores a new event supplied through the API and returns the created database record.
@router.post("/events")
def add_event(event: Event):
    connection = get_connection()
    cursor = connection.cursor()

    cursor.execute("""
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
    """, (
        event.event_type,
        event.severity,
        event.source,
        event.description,
        event.timestamp,
        event.acknowledged,
    ))

    connection.commit()
    new_id = cursor.lastrowid
    connection.close()

    return {
        "message": "Event added",
        "event_id": new_id
    }


# Marks one recorded event as acknowledged without deleting its history.
@router.put("/events/{event_id}/acknowledge")
def acknowledge_event(event_id: int):
    connection = get_connection()
    cursor = connection.cursor()

    cursor.execute(
        """
        UPDATE Events
        SET acknowledged = 1
        WHERE event_id = ?
        """,
        (event_id,)
    )

    if cursor.rowcount == 0:
        connection.close()

        raise HTTPException(
            status_code=404,
            detail="Event not found"
        )

    connection.commit()
    connection.close()

    return {
        "message": "Event acknowledged",
        "event_id": event_id
    }
