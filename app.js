/* =====================================================
   APP
   =====================================================
   Home dashboard behaviour: live sensor status, alerts, inventory summary and acknowledgement.
*/

async function updateSensors() {
    try {
       const [sensorsResponse, readingsResponse, doorsResponse, warningsResponse, alertsResponse ] = 
	await Promise.all([
    fetch("/sensors"),
    fetch("/sensor-readings"),
    fetch("/door-states"),
    fetch("/door-warnings"),
    fetch("/alert-state")
]);

        const sensors = await sensorsResponse.json();
        const readings = await readingsResponse.json();
        const doorStates = await doorsResponse.json();
        const doorWarnings = await warningsResponse.json();
        const alertState = await alertsResponse.json();
        const activeAlerts = alertState.active_alerts || [];

        for (const sensor of sensors) {
            if (!sensor.enabled) continue;

            const compartment = sensor.compartment?.toLowerCase();

            // -------------------------
            // TEMPERATURE
            // -------------------------

            const sensorReadings = readings
                .filter(r => r.sensor_id === sensor.sensor_id)
                .sort((a, b) => b.reading_id - a.reading_id);

            const latestReading =
                sensorReadings.length > 0 ? sensorReadings[0] : null;

            if (latestReading) {
                if (compartment === "fridge") {
                    const element =
                        document.getElementById("fridge-temp");

                    if (element) {
                        element.textContent =
                            `${latestReading.value.toFixed(1)}°C`;
                    }
                }

                if (compartment === "freezer") {
                    const element =
                        document.getElementById("freezer-temp");

                    if (element) {
                        element.textContent =
                            `${latestReading.value.toFixed(1)}°C`;
                    }
                }
            }

            // -------------------------
            // DOOR STATE
            // -------------------------

            const mac = sensor.mac_address
                ?.replaceAll(":", "")
                .toLowerCase();

            const doorState = doorStates[mac];
			const doorWarning = doorWarnings[mac] == true;
			
            if (doorState) {
                if (compartment === "fridge") {
                    const element =
                        document.getElementById("fridge-door");

                    if (element) {
                        element.textContent =
                            doorState === "open" ? "Open" : "Closed";
                    }
                }

                if (compartment === "freezer") {
                    const element =
                        document.getElementById("freezer-door");

                    if (element) {
                        element.textContent =
                            doorState === "open" ? "Open" : "Closed";
                    }
                }
            }

            // -------------------------
            // OVERALL SENSOR STATUS
            // -------------------------

            let statusText = "Normal";

            // 1. Connectivity - offline after 2 minutes
            if (sensor.last_seen) {
                const lastSeen = new Date(sensor.last_seen);
                const ageSeconds =
                    (Date.now() - lastSeen.getTime()) / 1000;

                if (ageSeconds > 120) {
                    statusText = "Sensor Offline";
                }
            } else {
                statusText = "Sensor Offline";
            }

			// 2. Missing or invalid temperature
			if (
			statusText === "Normal" &&
				(
				!latestReading ||
				!Number.isFinite(latestReading.value)
				)
			) {
				statusText = "Sensor Fault";
			}

			// 3. Door has remained open beyond configured warning time
			if (
				statusText === "Normal" &&
				doorWarning
			) {
				statusText = "Door Warning";
			}
			
			// 4. Temperature warning confirmed by the gateway alert state
			const temperatureWarning = activeAlerts.some(alert =>
			alert.includes(sensor.mac_address) &&
			alert.endsWith(":temperature_warning")
			);
			
			if (
			statusText === "Normal" &&
			temperatureWarning
			) {
				statusText = "Temperature Alert";
				}

            // -------------------------
            // DISPLAY STATUS
            // -------------------------

            if (compartment === "fridge") {
                const element =
                    document.getElementById("fridge-status");

                if (element) {
                    element.textContent = statusText;
                }
            }

            if (compartment === "freezer") {
                const element =
                    document.getElementById("freezer-status");

                if (element) {
                    element.textContent = statusText;
                }
            }
        }

    } catch (error) {
        console.error("Sensor update failed:", error);
    }
}

// Loads the shared alert state and updates the Home page alert summary.
async function updateAlerts() {
    try {
        const response = await fetch("/alert-state");
        const alertState = await response.json();

        const alertList = document.getElementById("alert-list");

        if (!alertList) {
            return;
        }

        const activeAlerts = alertState.active_alerts || [];

        if (activeAlerts.length === 0) {
            alertList.innerHTML = `
                <div class="empty-message">
                    No active alerts
                </div>
            `;
            return;
        }

        alertList.innerHTML = "";

        for (const alert of activeAlerts) {
            const lastColon = alert.lastIndexOf(":");
            const alertType = alert.substring(lastColon + 1);

            let alertName = alertType;

            if (alertType === "door_warning") {
				alertName = "Door Warning";
			} else if (alertType === "temperature_warning") {
				alertName = "Temperature Warning";
			} else if (alertType === "sensor_fault") {
				alertName = "Sensor Fault";
			} else if (alertType === "learned_temperature_anomaly") {
				alertName = "Learned Temperature Anomaly";
			}

            const alertElement = document.createElement("div");
            alertElement.className = "alert-item";
            alertElement.textContent = alertName;

            alertList.appendChild(alertElement);
        }

    } catch (error) {
        console.error("Alert update failed:", error);
    }
}

updateSensors();
updateAlerts();

let inventoryItems = [];
let inventoryStart = 0;
const INVENTORY_VISIBLE = 3;

// Calculates the number of days until expiry for sorting, colouring and user-facing labels.
function getDaysUntilExpiry(expiryDate) {
    if (!expiryDate) return null;

    const today = new Date();
    today.setHours(0, 0, 0, 0);

    const expiry = new Date(expiryDate + "T00:00:00");

    return Math.round(
        (expiry - today) / (1000 * 60 * 60 * 24)
    );
}

// Converts an expiry offset into the text shown beside an inventory item.
function formatExpiry(days) {
    if (days === null) return "No expiry";
    if (days < 0) return "Expired";
    if (days === 0) return "Today";
    if (days === 1) return "Tomorrow";

    return `${days} days`;
}

// Applies filtering/sorting and rebuilds the visible inventory table from the current client-side data.
function renderInventory() {
    const list = document.getElementById("inventory-list");
    const summary = document.getElementById("expiry-summary");
    const upButton = document.getElementById("inventory-up");
    const downButton = document.getElementById("inventory-down");

    if (!list) return;

    if (inventoryItems.length === 0) {
        list.innerHTML = `
            <div class="empty-message">
                No inventory items
            </div>
        `;

        if (summary) summary.textContent = "";
        if (upButton) upButton.disabled = true;
        if (downButton) downButton.disabled = true;
        return;
    }

    const expiringSoon = inventoryItems.filter(item => {
        const days = getDaysUntilExpiry(item.expiry_date);
        return days !== null && days >= 0 && days <= 7;
    }).length;

    if (summary) {
        summary.textContent =
            expiringSoon > 0
                ? `${expiringSoon} expiring soon`
                : "";
    }

    const visibleItems = inventoryItems.slice(
        inventoryStart,
        inventoryStart + INVENTORY_VISIBLE
    );

    list.innerHTML = "";

    for (const item of visibleItems) {
        const days = getDaysUntilExpiry(item.expiry_date);

        const element = document.createElement("div");
        element.className = "inventory-item";

        element.innerHTML = `
            <span class="inventory-name">${item.name ?? "Unknown item"}</span>
            <span>${item.storage_location ?? ""}</span>
            <span>Qty ${item.qty}</span>
            <span class="inventory-expiry">${formatExpiry(days)}</span>
        `;

        list.appendChild(element);
    }

    if (upButton) {
        upButton.disabled = inventoryStart === 0;
    }

    if (downButton) {
        downButton.disabled =
            inventoryStart + INVENTORY_VISIBLE >= inventoryItems.length;
    }
}

// Fetches the latest inventory data before re-rendering the Home-page inventory card.
async function updateInventory() {
    try {
        const response = await fetch("/inventory");
        const inventory = await response.json();

        inventoryItems = inventory
            .filter(item => item.status?.toUpperCase() === "ACTIVE")
            .sort((a, b) => {
                const aDays = getDaysUntilExpiry(a.expiry_date);
                const bDays = getDaysUntilExpiry(b.expiry_date);

                if (aDays === null && bDays === null) return 0;
                if (aDays === null) return 1;
                if (bDays === null) return -1;

                return aDays - bDays;
            });

        const maxStart = Math.max(
            0,
            inventoryItems.length - INVENTORY_VISIBLE
        );

        inventoryStart = Math.min(inventoryStart, maxStart);

        renderInventory();

    } catch (error) {
        console.error("Inventory update failed:", error);
    }
}

document.getElementById("inventory-up")?.addEventListener("click", () => {
    if (inventoryStart > 0) {
        inventoryStart--;
        renderInventory();
    }
});

document.getElementById("inventory-down")?.addEventListener("click", () => {
    if (inventoryStart + INVENTORY_VISIBLE < inventoryItems.length) {
        inventoryStart++;
        renderInventory();
    }
});

updateInventory();
setInterval(updateSensors, 5000);
setInterval(updateAlerts, 5000);
setInterval(updateInventory, 10000);

// -------------------------
// ALERT ACKNOWLEDGEMENT BUTTON
// -------------------------

async function updateAcknowledgeButton() {
    try {
        const response = await fetch("/alert-state");
        const state = await response.json();

        const button = document.getElementById("acknowledge-alerts");
        if (!button) return;

        const unacknowledged = state.unacknowledged_alerts || [];

        button.style.display =
            unacknowledged.length > 0 ? "block" : "none";

    } catch (error) {
        console.error("Alert acknowledgement state failed:", error);
    }
}


// Sends the alert acknowledgement request; this silences/acknowledges alerts without removing an active physical condition.
async function acknowledgeAlerts() {
    try {
        const response = await fetch("/alerts/acknowledge", {
            method: "POST"
        });

        if (!response.ok) {
            throw new Error("Acknowledgement failed");
        }

        await updateAcknowledgeButton();

    } catch (error) {
        console.error("Alert acknowledgement failed:", error);
    }
}


const acknowledgeButton =
    document.getElementById("acknowledge-alerts");

if (acknowledgeButton) {
    acknowledgeButton.addEventListener("click", acknowledgeAlerts);
}

updateAcknowledgeButton();

setInterval(updateAcknowledgeButton, 5000);


