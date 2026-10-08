# =====================================================
# MAIN
# =====================================================
"""
Creates the FastAPI application, exposes the touchscreen GUI, registers API routes, and starts the database and MQTT services.
"""

from fastapi import FastAPI
from fastapi.staticfiles import StaticFiles

from backend.database import initialise_database
from backend.mqtt_client import start_mqtt
from backend.routes import (
    system,
    inventory,
    nfc,
    pairing,
    sensors,
    events,
)


# =====================================================
# FRIDGE MANAGER API
# =====================================================

app = FastAPI(title="Fridge Manager API")


# =====================================================
# GUI
# =====================================================

app.mount("/gui", StaticFiles(directory="gui"), name="gui")


# =====================================================
# ROUTES
# =====================================================

app.include_router(system.router)
app.include_router(inventory.router)
app.include_router(nfc.router)
app.include_router(pairing.router)
app.include_router(sensors.router)
app.include_router(events.router)


# =====================================================
# STARTUP
# =====================================================

initialise_database()
start_mqtt()
