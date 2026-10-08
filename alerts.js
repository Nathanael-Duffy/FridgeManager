/* =====================================================
   ALERTS
   =====================================================
   Alerts page behaviour: live active conditions, event history, filtering and buzzer acknowledgement.
*/

const activeAlertList = document.getElementById("active-alert-list");
const historyList = document.getElementById("history-list");
const historyFilter = document.getElementById("history-filter");
const silenceButton = document.getElementById("silence-alerts");

let events = [];

// Converts internal event identifiers into readable labels for the Alerts page.
function formatEventType(type) {
    if (!type) return "Unknown";

    return type
        .replaceAll("_", " ")
        .replace(/\b\w/g, character => character.toUpperCase());
}

// Formats stored event timestamps for display in the local touchscreen interface.
function formatTimestamp(timestamp) {
    if (!timestamp) return "--";

    const date = new Date(timestamp);

    if (Number.isNaN(date.getTime())) {
        return timestamp;
    }

    return date.toLocaleString([], {
        day: "2-digit",
        month: "2-digit",
        year: "2-digit",
        hour: "2-digit",
        minute: "2-digit"
    });
}

// Builds the Active Alerts display from the live alert state returned by the backend.
function renderActiveAlerts(state) {
    const activeAlerts = Array.isArray(state.active_alerts)
        ? state.active_alerts
        : [];

    const unacknowledgedAlerts = Array.isArray(state.unacknowledged_alerts)
        ? state.unacknowledged_alerts
        : [];

    activeAlertList.innerHTML = "";

    if (activeAlerts.length === 0) {
        activeAlertList.innerHTML = `
            <div class="empty-message">
                No active alerts
            </div>
        `;

        silenceButton.style.display = "none";
        return;
    }

    activeAlerts.forEach(alert => {
        const separatorIndex = alert.lastIndexOf(":");

		const nodeMac = separatorIndex >= 0
			? alert.substring(0, separatorIndex)
			: alert;

		const alertType = separatorIndex >= 0
			? alert.substring(separatorIndex + 1)
			: "alert";

        const acknowledged =
            !unacknowledgedAlerts.includes(alert);

        const item = document.createElement("div");
        item.className = "active-alert-item";

        item.innerHTML = `
            <div class="active-alert-type">
                ${formatEventType(alertType)}
            </div>

            <div class="active-alert-description">
                ${nodeMac}
            </div>

            <div class="active-alert-status">
                ${acknowledged ? "Silenced" : "Active"}
            </div>
        `;

        activeAlertList.appendChild(item);
    });

    silenceButton.style.display =
        unacknowledgedAlerts.length > 0
            ? "block"
            : "none";
}

// Applies the selected history filter without modifying the underlying event records.
function eventMatchesFilter(event) {
    const filter = historyFilter.value;

    if (filter === "all") {
        return true;
    }

    if (filter === "active") {
        return Number(event.acknowledged) === 0;
    }

    return String(event.severity).toLowerCase() === filter;
}

// Builds the visible event-history rows from the currently loaded and filtered event data.
function renderHistory() {
    historyList.innerHTML = "";

    const filteredEvents = events.filter(eventMatchesFilter);

    if (filteredEvents.length === 0) {
        historyList.innerHTML = `
            <div class="empty-message">
                No matching events
            </div>
        `;
        return;
    }

    filteredEvents.forEach(event => {
        const item = document.createElement("div");

        const severity =
            String(event.severity || "info").toLowerCase();

        item.className =
            `history-item severity-${severity}`;

        const status =
            Number(event.acknowledged) === 1
                ? "Acknowledged"
                : "Unacknowledged";

        item.innerHTML = `
            <div class="history-time">
                ${formatTimestamp(event.timestamp)}
            </div>

            <div class="history-source">
                ${event.source || "--"}
            </div>

            <div class="history-type">
                ${formatEventType(event.event_type)}
            </div>

            <div class="history-description">
                ${event.description || "--"}
            </div>

            <div class="history-status">
                ${status}
            </div>
        `;

        historyList.appendChild(item);
    });
}

// Fetches the backend live alert state used by the Active Alerts section.
async function loadAlertState() {
    try {
        const response = await fetch("/alert-state");

        if (!response.ok) {
            throw new Error("Could not load alert state");
        }

        const state = await response.json();
        renderActiveAlerts(state);
    } catch (error) {
        console.error(error);

        activeAlertList.innerHTML = `
            <div class="empty-message">
                Unable to load active alerts
            </div>
        `;

        silenceButton.style.display = "none";
    }
}

// Fetches recorded event history from the backend for the Alerts page.
async function loadHistory() {
    try {
        const response = await fetch("/events");

        if (!response.ok) {
            throw new Error("Could not load event history");
        }

        events = await response.json();
        renderHistory();
    } catch (error) {
        console.error(error);

        historyList.innerHTML = `
            <div class="empty-message">
                Unable to load alert history
            </div>
        `;
    }
}

// Acknowledges current alerts through FastAPI so the buzzer is silenced while persistent conditions remain visible.
async function silenceAlerts() {
    silenceButton.disabled = true;

    try {
        const response = await fetch(
            "/alerts/acknowledge",
            {
                method: "POST"
            }
        );

        if (!response.ok) {
            throw new Error("Could not acknowledge alerts");
        }

        await loadAlertState();
        await loadHistory();
    } catch (error) {
        console.error(error);
    } finally {
        silenceButton.disabled = false;
    }
}

historyFilter.addEventListener("change", renderHistory);
silenceButton.addEventListener("click", silenceAlerts);

// Refreshes both live alerts and recorded history so the page stays synchronised.
async function refreshAlerts() {
    await Promise.all([
        loadAlertState(),
        loadHistory()
    ]);
}

refreshAlerts();

setInterval(loadAlertState, 5000);
setInterval(loadHistory, 15000);
