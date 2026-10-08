# =====================================================
# SYSTEM
# =====================================================
"""
Exposes live system state such as door conditions, warnings and alert acknowledgement to the GUI.
"""

from fastapi import APIRouter
from fastapi.responses import FileResponse

from backend.mqtt_client import (
    mqtt_client,
    door_states,
    door_warnings,
)
import backend.mqtt_client as mqtt_state


router = APIRouter()


# Redirects the root API request to the touchscreen home page.
@router.get("/", include_in_schema=False)
def home():
    return FileResponse("gui/index.html")


# Returns the latest door state received for each sensor node.
@router.get("/door-states")
def get_door_states():
    return door_states


# Returns which sensor nodes currently have a prolonged-door warning.
@router.get("/door-warnings")
def get_door_warnings():
    return door_warnings


# Returns a thread-safe snapshot of active and acknowledged alert state for the API/MQTT layers.
@router.get("/alert-state")
def get_alert_state():
    return mqtt_state.buzzer_state


# Marks the current active alerts as acknowledged and immediately silences the buzzer output.
@router.post("/alerts/acknowledge")
def acknowledge_alerts():
    mqtt_client.publish(
        "fridgemanager/buzzer/acknowledge",
        "acknowledge",
        qos=1
    )

    return {
        "message": "Alert acknowledgement sent"
    }
