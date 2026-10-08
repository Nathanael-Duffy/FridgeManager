import sqlite3

from fastapi import APIRouter, HTTPException

from backend.database import get_connection
from backend.models import (
    Item,
    InventoryItem,
    InventoryRegistration,
    BulkInventoryRegistration,
)


router = APIRouter()


# =====================================================
# INVENTORY AND PRODUCT ROUTES
# =====================================================
"""
Provides the FastAPI endpoints used by the inventory interface. Products are
stored in Items, while physical fridge/freezer entries are stored in Inventory
and can optionally be linked to unique NFC tags.
"""


# Returns the product catalogue stored in Items so the frontend can select existing products.
@router.get("/items")
def get_items():
    connection = get_connection()

    rows = connection.execute(
        "SELECT * FROM Items"
    ).fetchall()

    connection.close()

    return [dict(row) for row in rows]


# Creates a basic product record in Items and returns its new database ID.
@router.post("/items")
def add_item(item: Item):
    connection = get_connection()
    cursor = connection.cursor()

    cursor.execute(
        "INSERT INTO Items (name, category) VALUES (?, ?)",
        (item.name, item.category),
    )

    connection.commit()
    new_id = cursor.lastrowid
    connection.close()

    return {
        "message": "Item added",
        "item_id": new_id
    }


# Returns inventory records joined with product details for display in the UI.
@router.get("/inventory")
def get_inventory():
    connection = get_connection()

    rows = connection.execute("""
        SELECT
            Inventory.inventory_id,
            Inventory.item_id,
            Items.name AS name,
            Items.category AS category,
            Inventory.nfc_uid,
            Inventory.qty,
            Inventory.storage_location,
            Inventory.date_added,
            Inventory.expiry_date,
            Inventory.status,
            Inventory.notes
        FROM Inventory
        LEFT JOIN Items
            ON Inventory.item_id = Items.item_id
    """).fetchall()

    connection.close()

    return [dict(row) for row in rows]


# Adds a direct Inventory record when the caller already has the required item details.
@router.post("/inventory")
def add_inventory(inventory: InventoryItem):
    connection = get_connection()
    cursor = connection.cursor()

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
        inventory.item_id,
        inventory.nfc_uid,
        inventory.qty,
        inventory.storage_location,
        inventory.date_added,
        inventory.expiry_date,
        inventory.status,
        inventory.notes,
    ))

    connection.commit()
    new_id = cursor.lastrowid
    connection.close()

    return {
        "message": "Inventory item added",
        "inventory_id": new_id
    }


# Registers one inventory entry, linking it to an existing product or creating a new product first.
# The database work is transactional so a failed registration can be rolled back cleanly.
@router.post("/inventory/register")
def register_inventory(
    registration: InventoryRegistration
):
    if registration.qty < 1:
        raise HTTPException(
            status_code=400,
            detail="Quantity must be at least 1"
        )

    location = (
        registration.storage_location
        .strip()
        .lower()
    )

    if location not in ("fridge", "freezer"):
        raise HTTPException(
            status_code=400,
            detail="Location must be Fridge or Freezer"
        )

    status = (
        registration.status
        .strip()
        .upper()
    )

    if status not in ("ACTIVE", "INACTIVE"):
        raise HTTPException(
            status_code=400,
            detail="Status must be ACTIVE or INACTIVE"
        )

    connection = get_connection()
    cursor = connection.cursor()

    try:
        # Use the selected Items record when an item_id is supplied; otherwise
        # create the product first and use its new ID for the inventory entry.
        if registration.item_id is not None:
            item = cursor.execute(
                "SELECT * FROM Items WHERE item_id = ?",
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
                    detail="Item name is required"
                )

            if not category:
                raise HTTPException(
                    status_code=400,
                    detail="Category is required"
                )

            existing = cursor.execute(
                """
                SELECT item_id
                FROM Items
                WHERE LOWER(name) = LOWER(?)
                """,
                (name,)
            ).fetchone()

            if existing is not None:
                raise HTTPException(
                    status_code=409,
                    detail=(
                        "An item with this name "
                        "already exists"
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

        nfc_uid = registration.nfc_uid

        if nfc_uid is not None:
            nfc_uid = nfc_uid.strip()

            if not nfc_uid:
                nfc_uid = None

        if nfc_uid is not None:
            # NFC UIDs must stay unique so one physical tag cannot identify
            # more than one inventory record.
            existing_tag = cursor.execute(
                """
                SELECT inventory_id
                FROM Inventory
                WHERE nfc_uid = ?
                """,
                (nfc_uid,)
            ).fetchone()

            if existing_tag is not None:
                raise HTTPException(
                    status_code=409,
                    detail=(
                        "NFC tag is already registered"
                    )
                )

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
            nfc_uid,
            registration.qty,
            location.capitalize(),
            registration.date_added,
            registration.expiry_date,
            status,
            registration.notes
        ))

        inventory_id = cursor.lastrowid

        connection.commit()

        row = cursor.execute("""
            SELECT
                Inventory.inventory_id,
                Inventory.item_id,
                Items.name,
                Items.category,
                Inventory.nfc_uid,
                Inventory.qty,
                Inventory.storage_location,
                Inventory.date_added,
                Inventory.expiry_date,
                Inventory.status,
                Inventory.notes
            FROM Inventory
            JOIN Items
                ON Inventory.item_id = Items.item_id
            WHERE Inventory.inventory_id = ?
        """, (inventory_id,)).fetchone()

        return {
            "message": "Inventory item added",
            "item_created": item_created,
            "inventory": dict(row)
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


# Registers multiple individually tagged units of the same product in one operation.
# Each NFC UID becomes its own Inventory record while sharing the same product item_id.
@router.post("/inventory/register-bulk")
def register_bulk_inventory(
    registration: BulkInventoryRegistration
):
    if len(registration.nfc_uids) < 2:
        raise HTTPException(
            status_code=400,
            detail="At least 2 NFC tags are required"
        )

    nfc_uids = [
        uid.strip()
        for uid in registration.nfc_uids
        if uid.strip()
    ]

    if len(nfc_uids) != len(registration.nfc_uids):
        raise HTTPException(
            status_code=400,
            detail="NFC UID cannot be empty"
        )

    if len(set(nfc_uids)) != len(nfc_uids):
        raise HTTPException(
            status_code=400,
            detail="Duplicate NFC tags were supplied"
        )

    location = (
        registration.storage_location
        .strip()
        .lower()
    )

    if location not in (
        "fridge",
        "freezer"
    ):
        raise HTTPException(
            status_code=400,
            detail="Location must be Fridge or Freezer"
        )

    status = (
        registration.status
        .strip()
        .upper()
    )

    if status not in (
        "ACTIVE",
        "INACTIVE"
    ):
        raise HTTPException(
            status_code=400,
            detail="Status must be ACTIVE or INACTIVE"
        )

    connection = get_connection()
    cursor = connection.cursor()

    try:
        if registration.item_id is not None:
            item = cursor.execute(
                """
                SELECT item_id
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
                    detail="Item name is required"
                )

            if not category:
                raise HTTPException(
                    status_code=400,
                    detail="Category is required"
                )

            existing = cursor.execute(
                """
                SELECT item_id
                FROM Items
                WHERE LOWER(name) = LOWER(?)
                """,
                (name,)
            ).fetchone()

            if existing is not None:
                raise HTTPException(
                    status_code=409,
                    detail=(
                        "An item with this name "
                        "already exists"
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

        # Check every supplied UID before inserting rows so one conflict
        # cannot leave a partially completed bulk registration.
        placeholders = ",".join(
            "?"
            for _ in nfc_uids
        )

        existing_tags = cursor.execute(
            f"""
            SELECT nfc_uid
            FROM Inventory
            WHERE nfc_uid IN ({placeholders})
            """,
            nfc_uids
        ).fetchall()

        if existing_tags:
            raise HTTPException(
                status_code=409,
                detail=(
                    "One or more NFC tags "
                    "are already registered"
                )
            )

        inventory_ids = []

        # Each tag represents one physical unit, so bulk registration creates
        # one Inventory row per UID rather than one combined quantity row.
        for uid in nfc_uids:
            cursor.execute(
                """
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
                """,
                (
                    item_id,
                    uid,
                    1,
                    location.capitalize(),
                    registration.date_added,
                    registration.expiry_date,
                    status,
                    registration.notes
                )
            )

            inventory_ids.append(
                cursor.lastrowid
            )

        connection.commit()

        return {
            "message": "Bulk inventory added",
            "item_created": item_created,
            "item_id": item_id,
            "quantity": len(inventory_ids),
            "inventory_ids": inventory_ids
        }

    except HTTPException:
        connection.rollback()
        raise

    except sqlite3.IntegrityError as error:
        connection.rollback()

        raise HTTPException(
            status_code=400,
            detail=(
                "Database integrity error: "
                f"{error}"
            )
        )

    except Exception:
        connection.rollback()
        raise

    finally:
        connection.close()


# Updates an existing inventory record while preserving any NFC tag already assigned to it.
@router.put("/inventory/{inventory_id}")
def update_inventory(
    inventory_id: int,
    inventory: InventoryItem
):
    if inventory.qty < 1:
        raise HTTPException(
            status_code=400,
            detail="Quantity must be at least 1"
        )

    location = (
        inventory.storage_location
        .strip()
        .lower()
    )

    if location not in ("fridge", "freezer"):
        raise HTTPException(
            status_code=400,
            detail="Location must be Fridge or Freezer"
        )

    status = (
        inventory.status
        .strip()
        .upper()
    )

    if status not in ("ACTIVE", "INACTIVE"):
        raise HTTPException(
            status_code=400,
            detail="Status must be ACTIVE or INACTIVE"
        )

    connection = get_connection()
    cursor = connection.cursor()

    try:
        current = cursor.execute(
            """
            SELECT *
            FROM Inventory
            WHERE inventory_id = ?
            """,
            (inventory_id,)
        ).fetchone()

        if current is None:
            raise HTTPException(
                status_code=404,
                detail="Inventory item not found"
            )

        # Keep an existing NFC assignment. A new tag is accepted only when
        # this inventory record does not already have one.
        nfc_uid = current["nfc_uid"]

        if not nfc_uid and inventory.nfc_uid:
            new_uid = inventory.nfc_uid.strip()

            if new_uid:
                existing_tag = cursor.execute(
                    """
                    SELECT inventory_id
                    FROM Inventory
                    WHERE nfc_uid = ?
                      AND inventory_id != ?
                    """,
                    (
                        new_uid,
                        inventory_id
                    )
                ).fetchone()

                if existing_tag is not None:
                    raise HTTPException(
                        status_code=409,
                        detail=(
                            "NFC tag is already "
                            "registered"
                        )
                    )

                nfc_uid = new_uid

        cursor.execute(
            """
            UPDATE Inventory
            SET
                qty = ?,
                storage_location = ?,
                expiry_date = ?,
                status = ?,
                notes = ?,
                nfc_uid = ?
            WHERE inventory_id = ?
            """,
            (
                inventory.qty,
                location.capitalize(),
                inventory.expiry_date,
                status,
                inventory.notes,
                nfc_uid,
                inventory_id
            )
        )

        connection.commit()

        return {
            "message": "Inventory item updated",
            "inventory_id": inventory_id,
            "nfc_uid": nfc_uid
        }

    except HTTPException:
        connection.rollback()
        raise

    finally:
        connection.close()


# Removes one inventory record by inventory_id and reports when the record does not exist.
@router.delete("/inventory/{inventory_id}")
def delete_inventory(inventory_id: int):
    connection = get_connection()
    cursor = connection.cursor()

    cursor.execute(
        """
        DELETE FROM Inventory
        WHERE inventory_id = ?
        """,
        (inventory_id,)
    )

    if cursor.rowcount == 0:
        connection.close()

        raise HTTPException(
            status_code=404,
            detail="Inventory item not found"
        )

    connection.commit()
    connection.close()

    return {
        "message": "Inventory item removed",
        "inventory_id": inventory_id
    }
