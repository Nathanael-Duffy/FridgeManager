# =====================================================
# NFC
# =====================================================
"""
Connects NFC scans and tag UIDs to inventory lookup and registration through the backend API.
"""

import sqlite3
import time

import RPi.GPIO as GPIO
from fastapi import APIRouter, HTTPException
from mfrc522 import MFRC522

from backend.database import get_connection
from backend.models import NFCRegistration


router = APIRouter()


# Reads the latest NFC UID made available to the backend and returns it to the Scan interface.
@router.get("/nfc/scan")
def scan_nfc():
    reader = MFRC522()
    timeout = 10
    start_time = time.time()

    try:
        while time.time() - start_time < timeout:
            status, _ = reader.MFRC522_Request(
                reader.PICC_REQIDL
            )

            if status == reader.MI_OK:
                status, uid = reader.MFRC522_Anticoll()

                if status == reader.MI_OK:
                    uid_text = "".join(
                        str(value)
                        for value in uid
                    )

                    return {
                        "scanned": True,
                        "uid": uid_text
                    }

            time.sleep(0.1)

        raise HTTPException(
            status_code=408,
            detail="No NFC tag detected"
        )

    finally:
        GPIO.cleanup()


# Looks up an NFC UID and returns the linked inventory/product details when the tag is registered.
@router.get("/nfc/{uid}")
def lookup_nfc(uid: str):
    connection = get_connection()

    row = connection.execute("""
        SELECT
            Inventory.inventory_id,
            Inventory.nfc_uid,
            Inventory.qty,
            Inventory.storage_location,
            Inventory.date_added,
            Inventory.expiry_date,
            Inventory.status,
            Inventory.notes,
            Items.item_id,
            Items.name,
            Items.category
        FROM Inventory
        LEFT JOIN Items
            ON Inventory.item_id = Items.item_id
        WHERE Inventory.nfc_uid = ?
    """, (uid,)).fetchone()

    connection.close()

    if row is None:
        return {
            "registered": False,
            "nfc_uid": uid
        }

    return {
        "registered": True,
        "inventory": dict(row)
    }


# Associates an NFC UID with a new inventory record, optionally creating the product definition first.
@router.post("/nfc/{uid}/register")
def register_nfc(
    uid: str,
    registration: NFCRegistration
):
    uid = uid.strip()

    if not uid:
        raise HTTPException(
            status_code=400,
            detail="NFC UID cannot be empty"
        )

    connection = get_connection()
    cursor = connection.cursor()

    try:
        existing = cursor.execute(
            """
            SELECT inventory_id
            FROM Inventory
            WHERE nfc_uid = ?
            """,
            (uid,)
        ).fetchone()

        if existing is not None:
            raise HTTPException(
                status_code=409,
                detail="NFC tag is already registered"
            )

        if registration.item_id is not None:
            item = cursor.execute(
                """
                SELECT *
                FROM Items
                WHERE item_id = ?
                """,
                (registration.item_id,)
            ).fetchone()

            if item is None:
                raise HTTPException(
                    status_code=404,
                    detail="Item not found"
                )

            item_id = registration.item_id
            item_created = False

        else:
            name = (
                registration.name or ""
            ).strip()

            category = (
                registration.category or ""
            ).strip()

            if not name:
                raise HTTPException(
                    status_code=400,
                    detail=(
                        "Provide either item_id for an "
                        "existing item or name for a new item"
                    )
                )

            if not category:
                raise HTTPException(
                    status_code=400,
                    detail=(
                        "Category is required for a new item"
                    )
                )

            existing_item = cursor.execute(
                """
                SELECT item_id
                FROM Items
                WHERE LOWER(name) = LOWER(?)
                """,
                (name,)
            ).fetchone()

            if existing_item is not None:
                raise HTTPException(
                    status_code=409,
                    detail=(
                        "An item with this name already exists"
                    )
                )

            cursor.execute(
                """
                INSERT INTO Items
                (name, category)
                VALUES (?, ?)
                """,
                (
                    name,
                    category
                )
            )

            item_id = cursor.lastrowid
            item_created = True

        cursor.execute("""
            INSERT INTO Inventory
            (
                item_id,
                nfc_uid,
                qty,
                storage_location,
                date_added,
                expiry_date,
                status,
                notes
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, ?)
        """, (
            item_id,
            uid,
            registration.qty,
            registration.storage_location,
            registration.date_added,
            registration.expiry_date,
            registration.status,
            registration.notes,
        ))

        inventory_id = cursor.lastrowid
        connection.commit()

        row = cursor.execute("""
            SELECT
                Inventory.inventory_id,
                Inventory.nfc_uid,
                Inventory.qty,
                Inventory.storage_location,
                Inventory.date_added,
                Inventory.expiry_date,
                Inventory.status,
                Inventory.notes,
                Items.item_id,
                Items.name,
                Items.category
            FROM Inventory
            JOIN Items
                ON Inventory.item_id = Items.item_id
            WHERE Inventory.inventory_id = ?
        """, (inventory_id,)).fetchone()

        return {
            "message": "NFC tag registered",
            "item_created": item_created,
            "inventory": dict(row),
        }

    except HTTPException:
        connection.rollback()
        raise

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
