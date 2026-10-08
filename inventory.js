// =====================================================
// INVENTORY PAGE STATE
// =====================================================
/*
Stores inventory data loaded from FastAPI together with the current filter,
sorting, modal and bulk-NFC state used by the inventory page.
*/

let allInventory = [];
let allProducts = [];

let expiryFilter = "all";
let locationFilter = "all";
let excludeExpired = false;

let sortMode = "expiry";
let sortDirection = "asc";

let selectedProduct = null;
let editingInventory = null;

let bulkNfcUids = [];
let bulkNfcScanning = false;


// Calculates the number of whole days between today and an item's expiry date.
function getDaysUntilExpiry(expiryDate) {
    if (!expiryDate) return null;

    const today = new Date();
    today.setHours(0, 0, 0, 0);

    const expiry =
        new Date(expiryDate + "T00:00:00");

    return Math.round(
        (expiry - today) /
        (1000 * 60 * 60 * 24)
    );
}


// Converts an expiry difference into the short text shown in the inventory list.
function formatExpiry(days) {
    if (days === null) {
        return "No expiry";
    }

    if (days === -1) {
        return "Expired 1 day ago";
    }

    if (days < -1) {
        return (
            "Expired " +
            Math.abs(days) +
            " days ago"
        );
    }

    if (days === 0) {
        return "Today";
    }

    if (days === 1) {
        return "Tomorrow";
    }

    return days + " days";
}


// Updates the active heading and arrow to match the current sort settings.
function updateSortHeadings() {
    document
        .querySelectorAll(".sort-heading")
        .forEach(heading => {
            heading.classList.remove(
                "active"
            );

            const arrow =
                heading.querySelector(
                    ".sort-arrow"
                );

            if (arrow) {
                arrow.textContent = "";
            }
        });

    const activeHeading =
        document.querySelector(
            `.sort-heading[data-sort="${sortMode}"]`
        );

    if (!activeHeading) return;

    activeHeading.classList.add(
        "active"
    );

    const arrow =
        activeHeading.querySelector(
            ".sort-arrow"
        );

    if (arrow) {
        arrow.textContent =
            sortDirection === "asc"
                ? "\u25BC"
                : "\u25B2";
    }
}


// Filters and sorts loaded inventory, then rebuilds the clickable rows shown on the page.
function renderInventory() {
    const list =
        document.getElementById(
            "full-inventory-list"
        );

    if (!list) return;

    // Start with active records before applying optional filters and sorting.
    let items =
        allInventory.filter(
            item =>
                item.status?.toUpperCase() ===
                "ACTIVE"
        );

    if (locationFilter !== "all") {
        items =
            items.filter(
                item =>
                    item.storage_location
                        ?.toLowerCase() ===
                    locationFilter
            );
    }

    if (expiryFilter !== "all") {
        items =
            items.filter(item => {
                const days =
                    getDaysUntilExpiry(
                        item.expiry_date
                    );

                if (days === null) {
                    return false;
                }

                if (
                    expiryFilter ===
                    "expired"
                ) {
                    return days < 0;
                }

                const limit =
                    Number(expiryFilter);

                return days <= limit;
            });
    }

    if (
        excludeExpired &&
        expiryFilter !== "expired"
    ) {
        items =
            items.filter(item => {
                const days =
                    getDaysUntilExpiry(
                        item.expiry_date
                    );

                return (
                    days === null ||
                    days >= 0
                );
            });
    }

    items.sort((a, b) => {
        let result = 0;

        if (sortMode === "name") {
            result =
                (a.name ?? "")
                    .localeCompare(
                        b.name ?? ""
                    );
        }

        else if (
            sortMode === "location"
        ) {
            result =
                (a.storage_location ?? "")
                    .localeCompare(
                        b.storage_location ?? ""
                    );
        }

        else if (
            sortMode === "quantity"
        ) {
            result =
                Number(a.qty ?? 0) -
                Number(b.qty ?? 0);
        }

        else if (
            sortMode === "status"
        ) {
            result =
                (a.status ?? "")
                    .localeCompare(
                        b.status ?? ""
                    );
        }

        else if (
            sortMode === "expiry"
        ) {
            const aDays =
                getDaysUntilExpiry(
                    a.expiry_date
                );

            const bDays =
                getDaysUntilExpiry(
                    b.expiry_date
                );

            if (
                aDays === null &&
                bDays === null
            ) {
                result = 0;
            }

            else if (
                aDays === null
            ) {
                return 1;
            }

            else if (
                bDays === null
            ) {
                return -1;
            }

            else {
                result =
                    aDays - bDays;
            }
        }

        if (
            sortDirection === "desc"
        ) {
            result *= -1;
        }

        return result;
    });

    if (items.length === 0) {
        list.innerHTML = `
            <div class="empty-message">
                No matching inventory items
            </div>
        `;

        return;
    }

    list.innerHTML = "";

    for (const item of items) {
        const days =
            getDaysUntilExpiry(
                item.expiry_date
            );

        const row =
            document.createElement(
                "div"
            );

        row.className =
            "full-inventory-item";

        row.dataset.inventoryId =
            item.inventory_id;

        const displayStatus =
            item.status
                ? item.status
                      .charAt(0)
                      .toUpperCase() +
                  item.status
                      .slice(1)
                      .toLowerCase()
                : "";

        row.innerHTML = `
            <strong>${item.name ?? "Unknown item"}</strong>
            <span>${item.storage_location ?? "--"}</span>
            <span>${item.qty}</span>
            <span>${formatExpiry(days)}</span>
            <span>${displayStatus}</span>
        `;

        row.addEventListener(
            "click",
            () => {
                openEditModal(item);
            }
        );

        list.appendChild(row);
    }
}


// Loads current inventory from FastAPI and redraws the list.
async function loadInventory() {
    try {
        const response =
            await fetch("/inventory");

        if (!response.ok) {
            throw new Error(
                "HTTP " +
                response.status
            );
        }

        allInventory =
            await response.json();

        renderInventory();

    } catch (error) {
        console.error(
            "Inventory load failed:",
            error
        );

        const list =
            document.getElementById(
                "full-inventory-list"
            );

        if (list) {
            list.innerHTML = `
                <div class="empty-message">
                    Unable to load inventory
                </div>
            `;
        }
    }
}


// Loads the Items catalogue used when selecting an existing product.
async function loadProducts() {
    const response =
        await fetch("/items");

    if (!response.ok) {
        throw new Error(
            "Unable to load products"
        );
    }

    allProducts =
        await response.json();
}


// Clears and hides the item form's error message.
function clearFormError() {
    const error =
        document.getElementById(
            "item-form-error"
        );

    error.textContent = "";

    error.classList.add(
        "hidden"
    );
}


// Displays a validation or backend error inside the item modal.
function showFormError(message) {
    const error =
        document.getElementById(
            "item-form-error"
        );

    error.textContent =
        message;

    error.classList.remove(
        "hidden"
    );
}


// Clears the current bulk-tagging session and resets its controls.
function resetBulkNfc() {
    bulkNfcUids = [];
    bulkNfcScanning = false;

    const progress =
        document.getElementById(
            "bulk-nfc-progress"
        );

    const button =
        document.getElementById(
            "bulk-nfc-start"
        );

    if (progress) {
        progress.textContent =
            "Ready to scan tags";
    }

    if (button) {
        button.disabled = false;

        button.textContent =
            "Start Tagging";
    }
}


function setNfcUid(uid = "") {
    const field =
        document.getElementById(
            "item-nfc"
        );

    const cleanUid =
        String(uid || "").trim();

    field.dataset.uid =
        cleanUid;

    field.textContent =
        cleanUid || "No NFC Tag";
}


function getNfcUid() {
    const field =
        document.getElementById(
            "item-nfc"
        );

    return (
        field.dataset.uid || ""
    ).trim();
}


// Resets modal state and fields before adding or editing an item.
function resetItemForm() {
    selectedProduct = null;
    editingInventory = null;

    document.querySelector(
        'input[name="product-mode"][value="existing"]'
    ).checked = true;

    document.querySelector(
        ".product-mode"
    ).classList.remove(
        "hidden"
    );

    document.getElementById(
        "existing-product-fields"
    ).classList.remove(
        "hidden"
    );

    document.getElementById(
        "new-product-fields"
    ).classList.add(
        "hidden"
    );

    document.getElementById(
        "edit-product-fields"
    ).classList.add(
        "hidden"
    );

    document.getElementById(
        "open-product-picker"
    ).textContent =
        "Select product...";

    document.getElementById(
        "new-item-name"
    ).value = "";

    document.getElementById(
        "new-item-category"
    ).value = "";

    document.getElementById(
        "edit-product-name"
    ).value = "";

    document.getElementById(
        "edit-product-category"
    ).value = "";

    document.getElementById(
        "item-quantity"
    ).value = "1";

    document.getElementById(
        "item-quantity"
    ).disabled = false;

    document.getElementById(
        "item-location"
    ).value = "Fridge";

    document.getElementById(
        "item-expiry"
    ).value = "";

    document.getElementById(
        "item-status"
    ).value = "ACTIVE";

    setNfcUid("");

    document.getElementById(
        "item-notes"
    ).value = "";

    const nfcButton =
        document.getElementById(
            "assign-nfc"
        );

    nfcButton.classList.add(
        "hidden"
    );

    nfcButton.textContent =
        "Scan Tag";

    document.getElementById(
        "remove-item"
    ).classList.add(
        "hidden"
    );

    document.getElementById(
        "individual-nfc"
    ).checked = false;

    document.getElementById(
        "individual-nfc"
    ).disabled = false;

    document.getElementById(
        "bulk-nfc-section"
    ).classList.add(
        "hidden"
    );

    document.getElementById(
        "bulk-nfc-controls"
    ).classList.add(
        "hidden"
    );

    resetBulkNfc();
    clearFormError();
}


// Opens the shared item modal in add mode.
function openItemModal() {
    resetItemForm();

    document.getElementById(
        "item-modal-title"
    ).textContent =
        "Add Item";

    document.getElementById(
        "save-item"
    ).textContent =
        "Add Item";

    updateBulkNfcAvailability();

    const nfcButton =
        document.getElementById(
            "assign-nfc"
        );

    nfcButton.textContent =
        "Scan Tag";

    nfcButton.classList.remove(
        "hidden"
    );

    document.getElementById(
        "item-modal"
    ).classList.remove(
        "hidden"
    );
}


// Opens the shared item modal in edit mode and fills it with the selected record.
function openEditModal(item) {
    resetItemForm();

    editingInventory = item;

    updateBulkNfcAvailability();

    document.getElementById(
        "item-modal-title"
    ).textContent =
        "Edit Item";

    document.getElementById(
        "save-item"
    ).textContent =
        "Save";

    document.querySelector(
        ".product-mode"
    ).classList.add(
        "hidden"
    );

    document.getElementById(
        "existing-product-fields"
    ).classList.add(
        "hidden"
    );

    document.getElementById(
        "new-product-fields"
    ).classList.add(
        "hidden"
    );

    document.getElementById(
        "edit-product-fields"
    ).classList.remove(
        "hidden"
    );

    document.getElementById(
        "edit-product-name"
    ).value =
        item.name ?? "";

    document.getElementById(
        "edit-product-category"
    ).value =
        item.category ?? "";

    document.getElementById(
        "item-quantity"
    ).value =
        item.qty ?? 1;

    document.getElementById(
        "item-location"
    ).value =
        item.storage_location ??
        "Fridge";

    document.getElementById(
        "item-expiry"
    ).value =
        item.expiry_date ?? "";

    document.getElementById(
        "item-status"
    ).value =
        (
            item.status ??
            "ACTIVE"
        ).toUpperCase();

    setNfcUid(
        item.nfc_uid ?? ""
    );

    document.getElementById(
        "item-notes"
    ).value =
        item.notes ?? "";

    document.getElementById(
        "remove-item"
    ).classList.remove(
        "hidden"
    );

    const nfcButton =
        document.getElementById(
            "assign-nfc"
        );

    if (!item.nfc_uid) {
        nfcButton.textContent =
            "Assign NFC Tag";

        nfcButton.classList.remove(
            "hidden"
        );
    }
    else {
        nfcButton.classList.add(
            "hidden"
        );
    }

    document.getElementById(
        "item-modal"
    ).classList.remove(
        "hidden"
    );
}


// Closes the item/product modals and clears temporary editing state.
function closeItemModal() {
    document.getElementById(
        "product-picker"
    ).classList.add(
        "hidden"
    );

    document.getElementById(
        "item-modal"
    ).classList.add(
        "hidden"
    );

    editingInventory = null;

    resetBulkNfc();
}


// Filters products by name/category and rebuilds the product-picker results.
function renderProductResults(
    searchText = ""
) {
    const results =
        document.getElementById(
            "product-results"
        );

    const search =
        searchText
            .trim()
            .toLowerCase();

    const products =
        allProducts.filter(
            product => {
                const name =
                    (
                        product.name ??
                        ""
                    ).toLowerCase();

                const category =
                    (
                        product.category ??
                        ""
                    ).toLowerCase();

                return (
                    name.includes(
                        search
                    ) ||
                    category.includes(
                        search
                    )
                );
            }
        );

    results.innerHTML = "";

    if (
        products.length === 0
    ) {
        results.innerHTML = `
            <div class="empty-message">
                No matching products
            </div>
        `;

        return;
    }

    for (
        const product of products
    ) {
        const button =
            document.createElement(
                "button"
            );

        button.type = "button";

        button.className =
            "product-result";

        button.innerHTML = `
            <strong>${product.name ?? "Unnamed product"}</strong>
            <span>${product.category ?? ""}</span>
        `;

        button.addEventListener(
            "click",
            () => {
                selectedProduct =
                    product;

                document.getElementById(
                    "open-product-picker"
                ).textContent =
                    product.name;

                document.getElementById(
                    "product-search"
                ).blur();

                document.getElementById(
                    "product-picker"
                ).classList.add(
                    "hidden"
                );
            }
        );

        results.appendChild(
            button
        );
    }
}


// Loads available products and opens the existing-product picker.
async function openProductPicker() {
    const search =
        document.getElementById(
            "product-search"
        );

    search.value = "";

    try {
        await loadProducts();

        renderProductResults();

        document.getElementById(
            "product-picker"
        ).classList.remove(
            "hidden"
        );

    } catch (error) {
        console.error(
            "Product load failed:",
            error
        );

        showFormError(
            "Unable to load existing products."
        );
    }
}


// Closes the existing-product picker.
function closeProductPicker() {
    document.getElementById(
        "product-search"
    ).blur();

    document.getElementById(
        "product-picker"
    ).classList.add(
        "hidden"
    );
}


// Reads and validates the quantity field, returning null when invalid.
function getFormQuantity() {
    const quantity =
        Number(
            document.getElementById(
                "item-quantity"
            ).value
        );

    if (
        !Number.isInteger(
            quantity
        ) ||
        quantity < 1
    ) {
        showFormError(
            "Quantity must be at least 1."
        );

        return null;
    }

    return quantity;
}


// Builds the add-item payload and chooses single or bulk registration before calling FastAPI.
async function saveNewInventoryItem() {
    clearFormError();

    const quantity =
        getFormQuantity();

    if (quantity === null) {
        return;
    }

    const bulkEnabled =
        document.getElementById(
            "individual-nfc"
        ).checked &&
        quantity > 1;

    if (
        bulkEnabled &&
        bulkNfcUids.length !== quantity
    ) {
        showFormError(
            "Scan all " +
            quantity +
            " NFC tags before adding the items."
        );

        return;
    }

    const mode =
        document.querySelector(
            'input[name="product-mode"]:checked'
        ).value;

    const payload = {
        item_id: null,
        name: null,
        category: null,

        storage_location:
            document.getElementById(
                "item-location"
            ).value,

        date_added:
            new Date()
                .toISOString()
                .slice(0, 10),

        expiry_date:
            document.getElementById(
                "item-expiry"
            ).value ||
            null,

        status:
            document.getElementById(
                "item-status"
            ).value,

        notes:
            document.getElementById(
                "item-notes"
            ).value.trim() ||
            null
    };

    if (mode === "existing") {
        if (!selectedProduct) {
            showFormError(
                "Select an existing product."
            );

            return;
        }

        payload.item_id =
            selectedProduct.item_id;
    }

    else {
        const name =
            document.getElementById(
                "new-item-name"
            ).value.trim();

        const category =
            document.getElementById(
                "new-item-category"
            ).value.trim();

        if (!name) {
            showFormError(
                "Item name is required."
            );

            return;
        }

        if (!category) {
            showFormError(
                "Category is required."
            );

            return;
        }

        payload.name = name;
        payload.category = category;
    }

    // Bulk mode uses a separate route because each scanned tag becomes
    // its own physical Inventory record.
    let endpoint;

    if (bulkEnabled) {
        endpoint =
            "/inventory/register-bulk";

        payload.nfc_uids = [
            ...bulkNfcUids
        ];
    }

    else {
        endpoint =
            "/inventory/register";

        payload.nfc_uid =
            getNfcUid() ||
            null;

        payload.qty = quantity;
    }

    const saveButton =
        document.getElementById(
            "save-item"
        );

    saveButton.disabled = true;

    saveButton.textContent =
        bulkEnabled
            ? "Adding " +
              quantity +
              " Items..."
            : "Adding...";

    try {
        const response =
            await fetch(
                endpoint,
                {
                    method: "POST",

                    headers: {
                        "Content-Type":
                            "application/json"
                    },

                    body:
                        JSON.stringify(
                            payload
                        )
                }
            );

        const result =
            await response.json();

        if (!response.ok) {
            throw new Error(
                result.detail ||
                "Unable to add item"
            );
        }

        closeItemModal();

        await loadInventory();

    } catch (error) {
        console.error(
            "Inventory save failed:",
            error
        );

        showFormError(
            error.message
        );

    } finally {
        saveButton.disabled = false;

        saveButton.textContent =
            "Add Item";
    }
}


// Sends edited inventory fields to FastAPI and refreshes the list after success.
async function saveEditedInventoryItem() {
    if (!editingInventory) {
        return;
    }

    clearFormError();

    const quantity =
        getFormQuantity();

    if (
        quantity === null
    ) {
        return;
    }

    const payload = {
        item_id:
            editingInventory.item_id,

        nfc_uid:
            getNfcUid() ||
            null,

        qty:
            quantity,

        storage_location:
            document.getElementById(
                "item-location"
            ).value,

        date_added:
            editingInventory.date_added,

        expiry_date:
            document.getElementById(
                "item-expiry"
            ).value ||
            null,

        status:
            document.getElementById(
                "item-status"
            ).value,

        notes:
            document.getElementById(
                "item-notes"
            ).value
                .trim() ||
            null
    };

    const saveButton =
        document.getElementById(
            "save-item"
        );

    saveButton.disabled =
        true;

    saveButton.textContent =
        "Saving...";

    try {
        const response =
            await fetch(
                "/inventory/" +
                editingInventory
                    .inventory_id,
                {
                    method: "PUT",

                    headers: {
                        "Content-Type":
                            "application/json"
                    },

                    body:
                        JSON.stringify(
                            payload
                        )
                }
            );

        const result =
            await response.json();

        if (!response.ok) {
            throw new Error(
                result.detail ||
                "Unable to update item"
            );
        }

        closeItemModal();

        await loadInventory();

    } catch (error) {
        console.error(
            "Inventory update failed:",
            error
        );

        showFormError(
            error.message
        );

    } finally {
        saveButton.disabled =
            false;

        saveButton.textContent =
            "Save";
    }
}


// Routes the Save button to the add or edit workflow based on current modal state.
async function saveItem() {
    if (editingInventory) {
        await saveEditedInventoryItem();
    }

    else {
        await saveNewInventoryItem();
    }
}


// Confirms and deletes the inventory record currently open in edit mode.
async function removeInventoryItem() {
    if (!editingInventory) {
        return;
    }

    const itemName =
        editingInventory.name ??
        "this item";

    const confirmed =
        window.confirm(
            "Remove " +
            itemName +
            " from inventory?"
        );

    if (!confirmed) {
        return;
    }

    const removeButton =
        document.getElementById(
            "remove-item"
        );

    removeButton.disabled =
        true;

    removeButton.textContent =
        "Removing...";

    try {
        const response =
            await fetch(
                "/inventory/" +
                editingInventory
                    .inventory_id,
                {
                    method: "DELETE"
                }
            );

        const result =
            await response.json();

        if (!response.ok) {
            throw new Error(
                result.detail ||
                "Unable to remove item"
            );
        }

        closeItemModal();

        await loadInventory();

    } catch (error) {
        console.error(
            "Inventory remove failed:",
            error
        );

        showFormError(
            error.message
        );

    } finally {
        removeButton.disabled =
            false;

        removeButton.textContent =
            "Remove Item";
    }
}


// Scans one NFC tag for an existing untagged inventory record.
async function beginNfcAssignment() {
    const assignButton =
        document.getElementById(
            "assign-nfc"
        );

    clearFormError();

    assignButton.disabled =
        true;

    assignButton.textContent =
        "Waiting for Tag...";

    try {
        const response =
            await fetch(
                "/nfc/scan"
            );

        const result =
            await response.json();

        if (!response.ok) {
            throw new Error(
                result.detail ||
                "Unable to scan NFC tag"
            );
        }

        if (
            !result.scanned ||
            !result.uid
        ) {
            throw new Error(
                "No NFC tag detected"
            );
        }

        const lookupResponse =
            await fetch(
                "/nfc/" +
                encodeURIComponent(
                    result.uid
                )
            );

        const lookup =
            await lookupResponse.json();

        if (!lookupResponse.ok) {
            throw new Error(
                lookup.detail ||
                "Unable to check NFC tag"
            );
        }

        if (lookup.registered) {
            throw new Error(
                "This NFC tag is already registered."
            );
        }

        setNfcUid(
            result.uid
        );

        assignButton.classList.add(
            "hidden"
        );

    } catch (error) {
        console.error(
            "NFC scan failed:",
            error
        );

        showFormError(
            error.message
        );

    } finally {
        assignButton.disabled =
            false;

        assignButton.textContent =
            editingInventory
                ? "Assign NFC Tag"
                : "Scan Tag";
    }
}


// Shows bulk NFC controls only when adding two or more new units.
function updateBulkNfcAvailability() {
    const quantity =
        Number(
            document.getElementById(
                "item-quantity"
            ).value
        );

    const section =
        document.getElementById(
            "bulk-nfc-section"
        );

    const checkbox =
        document.getElementById(
            "individual-nfc"
        );

    const controls =
        document.getElementById(
            "bulk-nfc-controls"
        );

    if (
        editingInventory ||
        quantity < 2
    ) {
        section.classList.add(
            "hidden"
        );

        checkbox.checked =
            false;

        controls.classList.add(
            "hidden"
        );

        resetBulkNfc();

        return;
    }

    section.classList.remove(
        "hidden"
    );

    controls.classList.toggle(
        "hidden",
        !checkbox.checked
    );
}


// Switches the form between normal quantity and individually tagged units.
function handleIndividualNfcChange() {
    const checkbox =
        document.getElementById(
            "individual-nfc"
        );

    const controls =
        document.getElementById(
            "bulk-nfc-controls"
        );

    resetBulkNfc();

    controls.classList.toggle(
        "hidden",
        !checkbox.checked
    );

    if (
        checkbox.checked
    ) {
        setNfcUid("");
    }
}


// Scans tags one at a time, rejecting duplicate or already-registered UIDs.
async function startBulkNfcTagging() {
    if (
        editingInventory ||
        bulkNfcScanning
    ) {
        return;
    }

    const quantity =
        getFormQuantity();

    if (
        quantity === null ||
        quantity < 2
    ) {
        showFormError(
            "Bulk NFC tagging requires a quantity of at least 2."
        );

        return;
    }

    const checkbox =
        document.getElementById(
            "individual-nfc"
        );

    if (!checkbox.checked) {
        return;
    }

    clearFormError();

    bulkNfcUids = [];
    bulkNfcScanning = true;

    const progress =
        document.getElementById(
            "bulk-nfc-progress"
        );

    const button =
        document.getElementById(
            "bulk-nfc-start"
        );

    const quantityField =
        document.getElementById(
            "item-quantity"
        );

    button.disabled = true;
    quantityField.disabled = true;
    checkbox.disabled = true;

    try {
        for (
            let index = 0;
            index < quantity;
            index++
        ) {
            progress.textContent =
                "Tag " +
                (index + 1) +
                " of " +
                quantity +
                " - Waiting for tag...";

            const response =
                await fetch(
                    "/nfc/scan"
                );

            const result =
                await response.json();

            if (!response.ok) {
                throw new Error(
                    result.detail ||
                    "Unable to scan NFC tag"
                );
            }

            if (
                !result.scanned ||
                !result.uid
            ) {
                throw new Error(
                    "No NFC tag detected"
                );
            }

            if (
                bulkNfcUids.includes(
                    result.uid
                )
            ) {
                throw new Error(
                    "This tag has already been scanned."
                );
            }

            // Verify each UID before accepting it into the batch.
            const lookupResponse =
                await fetch(
                    "/nfc/" +
                    encodeURIComponent(
                        result.uid
                    )
                );

            const lookup =
                await lookupResponse.json();

            if (
                !lookupResponse.ok
            ) {
                throw new Error(
                    lookup.detail ||
                    "Unable to check NFC tag"
                );
            }

            if (
                lookup.registered
            ) {
                throw new Error(
                    "This NFC tag is already registered."
                );
            }

            bulkNfcUids.push(
                result.uid
            );

            progress.textContent =
                bulkNfcUids.length +
                " of " +
                quantity +
                " tags scanned";

            if (
                index <
                quantity - 1
            ) {
                await new Promise(
                    resolve =>
                        setTimeout(
                            resolve,
                            750
                        )
                );
            }
        }

        progress.textContent =
            quantity +
            " of " +
            quantity +
            " tags scanned - Complete";

        button.textContent =
            "Tagging Complete";

    } catch (error) {
        console.error(
            "Bulk NFC tagging failed:",
            error
        );

        showFormError(
            error.message
        );

        progress.textContent =
            bulkNfcUids.length +
            " of " +
            quantity +
            " tags scanned";

        button.disabled =
            false;

        button.textContent =
            bulkNfcUids.length > 0
                ? "Restart Tagging"
                : "Start Tagging";

    } finally {
        bulkNfcScanning =
            false;

        quantityField.disabled =
            false;

        checkbox.disabled =
            false;
    }
}



// =====================================================
// PAGE EVENT HANDLERS
// =====================================================
/*
Connects filter, sort, modal and NFC controls to the workflows above.
*/

document
    .querySelectorAll(
        "[data-expiry]"
    )
    .forEach(button => {
        button.addEventListener(
            "click",
            () => {
                expiryFilter =
                    button.dataset.expiry;

                document
                    .querySelectorAll(
                        "[data-expiry]"
                    )
                    .forEach(item => {
                        item.classList.remove(
                            "active"
                        );
                    });

                button.classList.add(
                    "active"
                );

                const excludeCheckbox =
                    document.getElementById(
                        "exclude-expired"
                    );

                if (
                    expiryFilter ===
                    "expired"
                ) {
                    excludeExpired =
                        false;

                    if (
                        excludeCheckbox
                    ) {
                        excludeCheckbox.checked =
                            false;

                        excludeCheckbox.disabled =
                            true;
                    }
                }

                else {
                    if (
                        excludeCheckbox
                    ) {
                        excludeCheckbox.disabled =
                            false;
                    }
                }

                if (
                    expiryFilter !==
                    "all"
                ) {
                    sortMode =
                        "expiry";

                    sortDirection =
                        "asc";

                    updateSortHeadings();
                }

                renderInventory();
            }
        );
    });


document
    .getElementById(
        "location-filter"
    )
    ?.addEventListener(
        "change",
        event => {
            locationFilter =
                event.target.value;

            renderInventory();
        }
    );


document
    .getElementById(
        "exclude-expired"
    )
    ?.addEventListener(
        "change",
        event => {
            excludeExpired =
                event.target.checked;

            renderInventory();
        }
    );


document
    .querySelectorAll(
        ".sort-heading"
    )
    .forEach(button => {
        button.addEventListener(
            "click",
            () => {
                const selectedSort =
                    button.dataset.sort;

                if (
                    sortMode ===
                    selectedSort
                ) {
                    sortDirection =
                        sortDirection ===
                        "asc"
                            ? "desc"
                            : "asc";
                }

                else {
                    sortMode =
                        selectedSort;

                    sortDirection =
                        "asc";
                }

                updateSortHeadings();
                renderInventory();
            }
        );
    });


document
    .getElementById(
        "add-item"
    )
    ?.addEventListener(
        "click",
        openItemModal
    );


document
    .getElementById(
        "close-item-modal"
    )
    ?.addEventListener(
        "click",
        closeItemModal
    );


document
    .getElementById(
        "cancel-item"
    )
    ?.addEventListener(
        "click",
        closeItemModal
    );


document
    .getElementById(
        "open-product-picker"
    )
    ?.addEventListener(
        "click",
        openProductPicker
    );


document
    .getElementById(
        "close-product-picker"
    )
    ?.addEventListener(
        "click",
        closeProductPicker
    );


document
    .getElementById(
        "product-search"
    )
    ?.addEventListener(
        "input",
        event => {
            renderProductResults(
                event.target.value
            );
        }
    );


document
    .querySelectorAll(
        'input[name="product-mode"]'
    )
    .forEach(input => {
        input.addEventListener(
            "change",
            event => {
                const existing =
                    document.getElementById(
                        "existing-product-fields"
                    );

                const newFields =
                    document.getElementById(
                        "new-product-fields"
                    );

                if (
                    event.target.value ===
                    "existing"
                ) {
                    existing.classList.remove(
                        "hidden"
                    );

                    newFields.classList.add(
                        "hidden"
                    );
                }

                else {
                    existing.classList.add(
                        "hidden"
                    );

                    newFields.classList.remove(
                        "hidden"
                    );
                }
            }
        );
    });


document
    .getElementById(
        "save-item"
    )
    ?.addEventListener(
        "click",
        saveItem
    );


document
    .getElementById(
        "remove-item"
    )
    ?.addEventListener(
        "click",
        removeInventoryItem
    );


document
    .getElementById(
        "assign-nfc"
    )
    ?.addEventListener(
        "click",
        beginNfcAssignment
    );


document
    .getElementById(
        "item-quantity"
    )
    ?.addEventListener(
        "input",
        updateBulkNfcAvailability
    );


document
    .getElementById(
        "individual-nfc"
    )
    ?.addEventListener(
        "change",
        handleIndividualNfcChange
    );


document
    .getElementById(
        "bulk-nfc-start"
    )
    ?.addEventListener(
        "click",
        startBulkNfcTagging
    );


const urlParams =
    new URLSearchParams(
        window.location.search
    );

const scannedNfc =
    urlParams.get("nfc");

const openAddItem =
    urlParams.get("add") === "1";

if (
    openAddItem &&
    scannedNfc
) {
    openItemModal();

    setNfcUid(
        scannedNfc
    );
}


enableTouchInputScrolling(
    document.querySelector(".item-modal-panel"),
    '.item-modal-panel input[type="number"], .item-modal-panel input[type="text"], .item-modal-panel input[type="date"], .item-modal-panel select'
);


updateSortHeadings();
loadInventory();
