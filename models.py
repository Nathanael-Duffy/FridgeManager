# =====================================================
# MODELS
# =====================================================
"""
Defines the Pydantic data models used to validate structured data entering the Fridge Manager API.
"""

from pydantic import BaseModel


# Defines a reusable product in the Items catalogue; inventory records refer back to this product definition.
class Item(BaseModel):
    name: str
    category: str


# Represents the complete data required for one stored inventory record.
class InventoryItem(BaseModel):
    item_id: int
    nfc_uid: str | None = None
    qty: int
    storage_location: str
    date_added: str
    expiry_date: str
    status: str
    notes: str | None = None


# Validates single-item registration. An existing item_id can be used, or name/category can describe a new product to create first.
class InventoryRegistration(BaseModel):
    item_id: int | None = None
    name: str | None = None
    category: str | None = None
    nfc_uid: str | None = None
    qty: int = 1
    storage_location: str
    date_added: str
    expiry_date: str | None = None
    status: str = "ACTIVE"
    notes: str | None = None


# Validates bulk registration when several individually tagged units of the same product are added in one operation.
class BulkInventoryRegistration(BaseModel):
    item_id: int | None = None
    name: str | None = None
    category: str | None = None
    nfc_uids: list[str]
    storage_location: str
    date_added: str
    expiry_date: str | None = None
    status: str = "ACTIVE"
    notes: str | None = None


# Defines the inventory details accepted when an unregistered NFC UID is associated with a product through the Scan workflow.
class NFCRegistration(BaseModel):
    item_id: int | None = None
    name: str | None = None
    category: str | None = None
    qty: int
    storage_location: str
    date_added: str
    expiry_date: str
    status: str
    notes: str | None = None


# Represents one timestamped temperature value associated with a configured sensor.
class SensorReading(BaseModel):
    sensor_id: int
    value: float
    timestamp: str


# Represents an alert/system-history entry, including whether the user has acknowledged it.
class Event(BaseModel):
    event_type: str
    severity: str
    source: str
    description: str
    timestamp: str
    acknowledged: int = 0


# Validates user-editable sensor settings before they are stored and forwarded toward the ESP32 node.
class SensorConfig(BaseModel):
    device_name: str
    compartment: str
    min_temp: float
    max_temp: float
    door_warning_seconds: int = 30
    temperature_hysteresis: float = 1.0
    temperature_warning_seconds: int = 900
    temperature_recovery_seconds: int = 300
    enabled: int = 1


# Validates the learning mode requested when starting temperature-profile learning on a sensor node.
class LearningStart(BaseModel):
    mode: int = 1


# Carries the user-selected name and compartment used to finish registering a newly paired sensor.
class SensorPairingRegistration(BaseModel):
    device_name: str
    compartment: str
