/* =====================================================
   SCAN
   =====================================================
   NFC Scan page behaviour: scanning a UID, looking it up and directing registered/unregistered tags to the correct workflow.
*/

const scanButton =
    document.getElementById("scan-button");

const scanInstruction =
    document.getElementById("scan-instruction");

const scanResult =
    document.getElementById("scan-result");


// Requests the latest NFC scan from the backend and starts lookup when a UID is received.
async function scanTag() {
    scanResult.classList.add("hidden");
    scanResult.innerHTML = "";

    scanButton.disabled = true;
    scanButton.textContent = "Waiting for Tag...";

    scanInstruction.textContent =
        "Place an NFC tag on the reader.";

    try {
        const scanResponse =
            await fetch("/nfc/scan");

        const scan =
            await scanResponse.json();

        if (!scanResponse.ok) {
            throw new Error(
                scan.detail ||
                "Unable to scan NFC tag"
            );
        }

        if (!scan.scanned || !scan.uid) {
            throw new Error(
                "No NFC tag detected"
            );
        }

        await lookupTag(scan.uid);

    } catch (error) {
        scanInstruction.textContent =
            error.message;

    } finally {
        scanButton.disabled = false;
        scanButton.textContent =
            "Scan NFC Tag";
    }
}


// Checks whether a scanned NFC UID is already associated with an inventory item.
async function lookupTag(uid) {
    const response =
        await fetch(
            "/nfc/" +
            encodeURIComponent(uid)
        );

    const result =
        await response.json();

    if (!response.ok) {
        throw new Error(
            result.detail ||
            "Unable to look up NFC tag"
        );
    }

    if (result.registered) {
        showRegisteredItem(
            uid,
            result.inventory
        );
    } else {
        showUnregisteredTag(uid);
    }
}


// Displays the inventory/product details returned for a known NFC tag.
function showRegisteredItem(uid, item) {
    scanInstruction.textContent =
        "Registered item found.";

    scanResult.innerHTML = `
        <h3>${escapeHtml(item.name)}</h3>

        <div class="scan-result-row">
            <span>Quantity</span>
            <strong>${item.qty}</strong>
        </div>

        <div class="scan-result-row">
            <span>Location</span>
            <strong>${escapeHtml(item.storage_location)}</strong>
        </div>

        <div class="scan-result-row">
            <span>Expiry</span>
            <strong>${escapeHtml(item.expiry_date || "No expiry")}</strong>
        </div>

        <div class="scan-result-row">
            <span>Status</span>
            <strong>${escapeHtml(item.status)}</strong>
        </div>

        <div class="scan-result-row">
            <span>NFC UID</span>
            <strong>${escapeHtml(uid)}</strong>
        </div>
    `;

    scanResult.classList.remove("hidden");
}


// Displays the registration path when the scanned NFC UID is not yet known.
function showUnregisteredTag(uid) {
    scanInstruction.textContent =
        "This NFC tag is not registered.";

    scanResult.innerHTML = `
        <h3>New NFC Tag</h3>

        <div class="scan-result-row">
            <span>NFC UID</span>
            <strong>${escapeHtml(uid)}</strong>
        </div>

        <div class="scan-actions">
            <button id="add-scanned-item"
                    type="button">
                Add Item
            </button>
        </div>
    `;

    scanResult.classList.remove("hidden");

    document
        .getElementById("add-scanned-item")
        .addEventListener("click", () => {
            openAddItem(uid);
        });
}


// Opens the inventory workflow with the scanned UID carried forward for registration.
function openAddItem(uid) {
    window.location.href =
        "/gui/inventory.html?add=1&nfc=" +
        encodeURIComponent(uid);
}


// Escapes user/data values before inserting them into generated HTML to avoid interpreting them as markup.
function escapeHtml(value) {
    return String(value ?? "")
        .replaceAll("&", "&amp;")
        .replaceAll("<", "&lt;")
        .replaceAll(">", "&gt;")
        .replaceAll('"', "&quot;")
        .replaceAll("'", "&#039;");
}


scanButton.addEventListener(
    "click",
    scanTag
);
