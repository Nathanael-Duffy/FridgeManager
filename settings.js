/* =====================================================
   SETTINGS
   =====================================================
   Settings page behaviour: sensor configuration, temperature learning and ESP32 pairing.
*/

const sensorList = document.getElementById("sensor-list");
const sensorConfig = document.getElementById("sensor-config");
const noSensorSelected = document.getElementById("no-sensor-selected");

const selectedSensorName = document.getElementById("selected-sensor-name");
const selectedSensorMac = document.getElementById("selected-sensor-mac");
const selectedSensorStatus = document.getElementById("selected-sensor-status");

const deviceName = document.getElementById("device-name");
const compartment = document.getElementById("compartment");
const minTemp = document.getElementById("min-temp");
const maxTemp = document.getElementById("max-temp");
const doorWarning = document.getElementById("door-warning");
const sensorEnabled = document.getElementById("sensor-enabled");
const enabledText = document.getElementById("enabled-text");

const recommendedButton = document.getElementById("recommended-values");
const saveButton = document.getElementById("save-config");
const saveMessage = document.getElementById("save-message");

const learningMode = document.getElementById("learning-mode");
const learningStatus = document.getElementById("learning-status");
const learningAction = document.getElementById("learning-action");
const learningProgress = document.getElementById("learning-progress");
const progressBar = document.getElementById("progress-bar");
const learningProgressText = document.getElementById("learning-progress-text");
const learningRemaining = document.getElementById("learning-remaining");
const learningSampleCount = document.getElementById("learning-sample-count");

const pairSensorButton = document.getElementById("pair-sensor");
const pairingModal = document.getElementById("pairing-modal");
const pairingClose = document.getElementById("pairing-close");
const pairingSetup = document.getElementById("pairing-setup");
const pairingSearch = document.getElementById("pairing-search");
const pairingSuccess = document.getElementById("pairing-success");
const pairingError = document.getElementById("pairing-error");

const pairingName = document.getElementById("pairing-name");
const pairingFridge = document.getElementById("pairing-fridge");
const pairingFreezer = document.getElementById("pairing-freezer");
const pairingStart = document.getElementById("pairing-start");
const pairingCancel = document.getElementById("pairing-cancel");
const pairingSearchCancel = document.getElementById("pairing-search-cancel");

const pairingStatusTitle = document.getElementById("pairing-status-title");
const pairingStatusText = document.getElementById("pairing-status-text");

const pairedSensorName = document.getElementById("paired-sensor-name");
const pairedSensorCompartment = document.getElementById(
    "paired-sensor-compartment"
);

const pairingDone = document.getElementById("pairing-done");
const pairingErrorText = document.getElementById("pairing-error-text");
const pairingRetry = document.getElementById("pairing-retry");
const pairingErrorCancel = document.getElementById("pairing-error-cancel");

let sensors = [];
let selectedSensorId = null;
let selectedSensor = null;
let currentLearningState = null;
let pendingLearningCommand = null;

let pairingTimer = null;
let pairingInProgress = false;
let pairingRegistrationSent = false;
let pairingSensorId = null;

const learningDurations = {
    1: 2 * 60 * 60,
    2: 24 * 60 * 60,
    3: 48 * 60 * 60
};

// Converts stored compartment values into the labels shown in Settings.
function formatCompartment(value) {
    if (!value) {
        return "Unassigned";
    }

    return value.charAt(0).toUpperCase() + value.slice(1);
}

// Converts internal sensor/learning states into readable Settings labels.
function formatStatus(value) {
    if (!value) {
        return "--";
    }

    return value.charAt(0).toUpperCase() + value.slice(1);
}

// Formats remaining learning time into a compact duration for the user.
function formatRemaining(seconds) {
    seconds = Math.max(0, Math.round(seconds));

    const hours = Math.floor(seconds / 3600);

    const minutes = Math.floor(
        (seconds % 3600) / 60
    );

    if (hours > 0) {
        return `${hours}h ${minutes}m remaining`;
    }

    return `${minutes}m remaining`;
}

// Returns the fridge/freezer selection currently chosen for a newly discovered sensor.
function getPairingCompartment() {
    if (pairingFreezer.checked) {
        return "freezer";
    }

    return "fridge";
}

// Builds the selectable sensor list from the records returned by FastAPI.
function renderSensorList() {
    sensorList.innerHTML = "";

    if (sensors.length === 0) {
        sensorList.innerHTML = `
            <div class="empty-message">
                No sensors registered
            </div>
        `;

        return;
    }

    sensors.forEach(sensor => {
        const button = document.createElement("button");

        button.type = "button";
        button.className = "sensor-list-item";

        if (sensor.sensor_id === selectedSensorId) {
            button.classList.add("selected");
        }

        button.innerHTML = `
            <span class="sensor-list-name">
                ${sensor.device_name || "Unassigned Sensor"}
            </span>

            <span class="sensor-list-detail">
                <span>
                    ${formatCompartment(sensor.compartment)}
                </span>

                <span>
                    ${formatStatus(sensor.status)}
                </span>
            </span>
        `;

        button.addEventListener("click", () => {
            selectSensor(sensor.sensor_id);
        });

        sensorList.appendChild(button);
    });
}

// Loads one sensor’s current configuration into the Settings controls.
function displaySensor(sensor) {
    selectedSensor = sensor;
    selectedSensorId = sensor.sensor_id;

    noSensorSelected.hidden = true;
    sensorConfig.hidden = false;

    selectedSensorName.textContent =
        sensor.device_name || "Unassigned Sensor";

    selectedSensorMac.textContent =
        sensor.mac_address || "";

    selectedSensorStatus.textContent =
        formatStatus(sensor.status);

    deviceName.value =
        sensor.device_name || "";

    compartment.value =
        sensor.compartment || "fridge";

    minTemp.value =
        sensor.min_temp ?? "";

    maxTemp.value =
        sensor.max_temp ?? "";

    doorWarning.value =
        sensor.door_warning_seconds ?? 30;

    sensorEnabled.checked =
        Number(sensor.enabled) === 1;

    updateEnabledText();

    renderSensorList();
}

// Updates the learning panel from the latest node learning state/profile.
function displayLearningState(state) {
    currentLearningState = state;

    if (!state.online_state_available) {
        learningStatus.textContent =
            "Waiting for Sensor";

        learningAction.disabled = true;
        learningMode.disabled = false;
        learningProgress.hidden = true;

        return;
    }

    if (
        pendingLearningCommand === "start" &&
        state.learning_active
    ) {
        pendingLearningCommand = null;
    }

    if (
        pendingLearningCommand === "cancel" &&
        !state.learning_active
    ) {
        pendingLearningCommand = null;
    }

    if (pendingLearningCommand === "start") {
        learningStatus.textContent =
            "Starting...";

        learningAction.textContent =
            "Starting...";

        learningAction.disabled = true;
        learningMode.disabled = true;

        return;
    }

    if (pendingLearningCommand === "cancel") {
        learningStatus.textContent =
            "Cancelling...";

        learningAction.textContent =
            "Cancelling...";

        learningAction.disabled = true;
        learningMode.disabled = true;

        return;
    }

    const mode =
        Number(state.learning_mode) || 1;

    const elapsed =
        Number(
            state.learning_elapsed_seconds
        ) || 0;

    const samples =
        Number(
            state.learning_sample_count
        ) || 0;

    if (state.learning_active) {
        const duration =
            learningDurations[mode] ||
            learningDurations[1];

        const progress = Math.min(
            100,
            (elapsed / duration) * 100
        );

        const remaining = Math.max(
            0,
            duration - elapsed
        );

        learningMode.value =
            String(mode);

        learningMode.disabled =
            true;

        learningStatus.textContent =
            "Learning";

        learningAction.textContent =
            "Cancel Learning";

        learningAction.disabled =
            false;

        learningProgress.hidden =
            false;

        progressBar.style.width =
            `${progress}%`;

        learningProgressText.textContent =
            `${Math.floor(progress)}%`;

        learningRemaining.textContent =
            formatRemaining(remaining);

        learningSampleCount.textContent =
            samples;

        return;
    }

    learningMode.disabled = false;
    learningProgress.hidden = true;
    learningAction.disabled = false;

    if (state.learning_trained) {
        learningStatus.textContent =
            "Trained";

        learningAction.textContent =
            "Relearn";
    } else {
        learningStatus.textContent =
            "Not Trained";

        learningAction.textContent =
            "Start Learning";
    }
}

// Fetches current learning progress/profile for the selected sensor.
async function loadLearningState() {
    if (selectedSensorId === null) {
        return;
    }

    const sensorId = selectedSensorId;

    try {
        const response = await fetch(
            `/sensors/${sensorId}/learning`
        );

        if (!response.ok) {
            throw new Error(
                "Could not load learning state"
            );
        }

        const state =
            await response.json();

        if (
            sensorId !==
            selectedSensorId
        ) {
            return;
        }

        displayLearningState(state);
    } catch (error) {
        console.error(error);

        learningStatus.textContent =
            "Unavailable";

        learningAction.disabled =
            true;
    }
}

// Requests that the selected sensor begin the chosen temperature-learning mode.
async function startLearning() {
    if (selectedSensorId === null) {
        return;
    }

    const mode =
        Number(learningMode.value);

    pendingLearningCommand =
        "start";

    learningAction.disabled =
        true;

    learningAction.textContent =
        "Starting...";

    learningStatus.textContent =
        "Starting...";

    learningMode.disabled =
        true;

    try {
        const response = await fetch(
            `/sensors/${selectedSensorId}/learning/start`,
            {
                method: "POST",
                headers: {
                    "Content-Type":
                        "application/json"
                },
                body: JSON.stringify({
                    mode: mode
                })
            }
        );

        const result =
            await response.json();

        if (!response.ok) {
            throw new Error(
                result.detail ||
                "Could not start learning"
            );
        }

        learningStatus.textContent =
            "Waiting for Sensor";

        learningAction.textContent =
            "Starting...";

        setTimeout(
            loadLearningState,
            2000
        );
    } catch (error) {
        console.error(error);

        pendingLearningCommand =
            null;

        learningStatus.textContent =
            "Start Failed";

        learningAction.textContent =
            "Start Learning";

        learningAction.disabled =
            false;

        learningMode.disabled =
            false;
    }
}

// Requests cancellation of learning while retaining any previously completed profile.
async function cancelLearning() {
    if (selectedSensorId === null) {
        return;
    }

    pendingLearningCommand =
        "cancel";

    learningAction.disabled =
        true;

    learningAction.textContent =
        "Cancelling...";

    learningStatus.textContent =
        "Cancelling...";

    learningMode.disabled =
        true;

    try {
        const response = await fetch(
            `/sensors/${selectedSensorId}/learning/cancel`,
            {
                method: "POST"
            }
        );

        const result =
            await response.json();

        if (!response.ok) {
            throw new Error(
                result.detail ||
                "Could not cancel learning"
            );
        }

        learningStatus.textContent =
            "Waiting for Sensor";

        learningAction.textContent =
            "Cancelling...";

        setTimeout(
            loadLearningState,
            2000
        );
    } catch (error) {
        console.error(error);

        pendingLearningCommand =
            null;

        learningStatus.textContent =
            "Cancel Failed";

        learningAction.textContent =
            "Cancel Learning";

        learningAction.disabled =
            false;

        learningMode.disabled =
            true;
    }
}

// Chooses the correct learning action from the current UI state.
async function handleLearningAction() {
    if (
        pendingLearningCommand !== null
    ) {
        return;
    }

    if (
        currentLearningState &&
        currentLearningState.learning_active
    ) {
        await cancelLearning();
    } else {
        await startLearning();
    }
}

// Changes the active sensor and loads its configuration and learning information.
async function selectSensor(sensorId) {
    try {
        const response = await fetch(
            `/sensors/${sensorId}`
        );

        if (!response.ok) {
            throw new Error(
                "Could not load sensor"
            );
        }

        const sensor =
            await response.json();

        displaySensor(sensor);

        currentLearningState =
            null;

        pendingLearningCommand =
            null;

        learningStatus.textContent =
            "Loading...";

        learningAction.disabled =
            true;

        learningProgress.hidden =
            true;

        await loadLearningState();
    } catch (error) {
        console.error(error);

        saveMessage.textContent =
            "Unable to load sensor";
    }
}

// Retrieves configured sensors and refreshes the Settings sensor selector.
async function loadSensors() {
    try {
        const response =
            await fetch("/sensors");

        if (!response.ok) {
            throw new Error(
                "Could not load sensors"
            );
        }

        sensors =
            await response.json();

        renderSensorList();

        if (sensors.length > 0) {
            const sensorToSelect =
                sensors.find(
                    sensor =>
                        sensor.sensor_id ===
                        selectedSensorId
                ) || sensors[0];

            await selectSensor(
                sensorToSelect.sensor_id
            );
        }
    } catch (error) {
        console.error(error);

        sensorList.innerHTML = `
            <div class="empty-message">
                Unable to load sensors
            </div>
        `;
    }
}

// Keeps the human-readable enabled/disabled label aligned with the toggle control.
function updateEnabledText() {
    enabledText.textContent =
        sensorEnabled.checked
            ? "Enabled"
            : "Disabled";
}

// Loads backend defaults for the selected fridge/freezer compartment into the form.
async function loadRecommendedValues() {
    const selectedCompartment =
        compartment.value;

    try {
        const response = await fetch(
            `/sensor-defaults/${selectedCompartment}`
        );

        if (!response.ok) {
            throw new Error(
                "Could not load recommended values"
            );
        }

        const defaults =
            await response.json();

        minTemp.value =
            defaults.min_temp;

        maxTemp.value =
            defaults.max_temp;

        doorWarning.value =
            defaults.door_warning_seconds;

        saveMessage.textContent =
            "Recommended values loaded";
    } catch (error) {
        console.error(error);

        saveMessage.textContent =
            "Unable to load recommended values";
    }
}

// Validates and submits the selected sensor configuration to FastAPI for storage and gateway delivery.
async function saveConfiguration() {
    if (
        selectedSensorId === null ||
        !selectedSensor
    ) {
        return;
    }

    const name =
        deviceName.value.trim();

    const minimum =
        Number(minTemp.value);

    const maximum =
        Number(maxTemp.value);

    const warningSeconds =
        Number(doorWarning.value);

    if (!name) {
        saveMessage.textContent =
            "Sensor name is required";

        return;
    }

    if (
        !Number.isFinite(minimum) ||
        !Number.isFinite(maximum)
    ) {
        saveMessage.textContent =
            "Enter valid temperature values";

        return;
    }

    if (minimum >= maximum) {
        saveMessage.textContent =
            "Minimum must be lower than maximum";

        return;
    }

    if (
        !Number.isInteger(warningSeconds) ||
        warningSeconds < 1
    ) {
        saveMessage.textContent =
            "Door warning must be at least 1 second";

        return;
    }

    const payload = {
        device_name:
            name,

        compartment:
            compartment.value,

        min_temp:
            minimum,

        max_temp:
            maximum,

        door_warning_seconds:
            warningSeconds,

        temperature_hysteresis:
            selectedSensor.temperature_hysteresis ??
            1.0,

        temperature_warning_seconds:
            selectedSensor.temperature_warning_seconds ??
            900,

        temperature_recovery_seconds:
            selectedSensor.temperature_recovery_seconds ??
            300,

        enabled:
            sensorEnabled.checked
                ? 1
                : 0
    };

    saveButton.disabled =
        true;

    saveMessage.textContent =
        "Saving...";

    try {
        const response = await fetch(
            `/sensors/${selectedSensorId}/config`,
            {
                method: "PUT",
                headers: {
                    "Content-Type":
                        "application/json"
                },
                body:
                    JSON.stringify(payload)
            }
        );

        const result =
            await response.json();

        if (!response.ok) {
            throw new Error(
                result.detail ||
                "Could not save configuration"
            );
        }

        selectedSensor =
            result.sensor;

        saveMessage.textContent =
            "Configuration saved";

        await loadSensors();
    } catch (error) {
        console.error(error);

        saveMessage.textContent =
            error.message ||
            "Unable to save configuration";
    } finally {
        saveButton.disabled =
            false;
    }
}

// Switches the pairing modal between discovery, found-sensor, registration and completion views.
function showPairingSection(section) {
    pairingSetup.hidden =
        section !== "setup";

    pairingSearch.hidden =
        section !== "search";

    pairingSuccess.hidden =
        section !== "success";

    pairingError.hidden =
        section !== "error";
}

// Opens the pairing workflow and resets its temporary interface state.
function openPairingModal() {
    pairingRegistrationSent =
        false;

    pairingSensorId =
        null;

    pairingName.value =
        "";

    pairingFridge.checked =
        true;

    pairingFreezer.checked =
        false;

    pairingStatusTitle.textContent =
        "Searching for Sensor";

    pairingStatusText.textContent =
        "Turn on or reset the new sensor.";

    showPairingSection("setup");

    pairingModal.hidden =
        false;

    setTimeout(() => {
        pairingName.focus();
    }, 50);
}

// Requests that backend/gateway pairing mode stop.
async function stopPairing() {
    if (pairingTimer !== null) {
        clearInterval(pairingTimer);
        pairingTimer = null;
    }

    if (pairingInProgress) {
        try {
            await fetch(
                "/pairing/cancel",
                {
                    method: "POST"
                }
            );
        } catch (error) {
            console.error(error);
        }
    }

    pairingInProgress =
        false;

    pairingRegistrationSent =
        false;
}

// Stops any active pairing attempt before closing the pairing interface.
async function closePairingModal() {
    await stopPairing();

    pairingModal.hidden =
        true;
}

// Displays a pairing-specific error without discarding the rest of the Settings state.
function showPairingError(message) {
    if (pairingTimer !== null) {
        clearInterval(pairingTimer);
        pairingTimer = null;
    }

    pairingInProgress =
        false;

    pairingRegistrationSent =
        false;

    pairingErrorText.textContent =
        message;

    showPairingSection("error");
}

// Submits the discovered sensor and chosen compartment/name for backend registration.
async function registerFoundSensor() {
    if (pairingRegistrationSent) {
        return;
    }

    pairingRegistrationSent =
        true;

    pairingStatusTitle.textContent =
        "Sensor Found";

    pairingStatusText.textContent =
        "Registering sensor...";

    try {
        const response = await fetch(
            "/pairing/register",
            {
                method: "POST",
                headers: {
                    "Content-Type":
                        "application/json"
                },
                body: JSON.stringify({
                    device_name:
                        pairingName.value.trim(),

                    compartment:
                        getPairingCompartment()
                })
            }
        );

        const result =
            await response.json();

        if (!response.ok) {
            throw new Error(
                result.detail ||
                "Could not register sensor"
            );
        }

        pairingSensorId =
            result.sensor_id ??
            result.sensor?.sensor_id ??
            null;

        pairingStatusTitle.textContent =
            "Connecting";

        pairingStatusText.textContent =
            "Waiting for sensor...";
    } catch (error) {
        console.error(error);

        showPairingError(
            error.message ||
            "Could not register sensor"
        );
    }
}

// Completes the pairing UI and reloads sensors so the newly registered node becomes selectable.
async function finishPairing() {
    if (pairingTimer !== null) {
        clearInterval(pairingTimer);
        pairingTimer = null;
    }

    pairingInProgress =
        false;

    pairedSensorName.textContent =
        pairingName.value.trim();

    pairedSensorCompartment.textContent =
        formatCompartment(
            getPairingCompartment()
        );

    showPairingSection("success");

    await loadSensors();

    if (pairingSensorId !== null) {
        await selectSensor(
            pairingSensorId
        );
    }
}

// Polls pairing state so gateway discovery results can appear in the modal without a page reload.
async function checkPairingState() {
    try {
        const response =
            await fetch("/pairing");

        if (!response.ok) {
            throw new Error(
                "Could not read pairing state"
            );
        }

        const state =
            await response.json();

        if (state.status === "searching") {
            pairingStatusTitle.textContent =
                "Searching for Sensor";

            pairingStatusText.textContent =
                "Turn on or reset the new sensor.";

            return;
        }

        if (state.status === "found") {
            await registerFoundSensor();
            return;
        }

        if (
            state.status === "complete" ||
            state.status === "remembered" ||
            state.status === "restored"
        ) {
            await finishPairing();
            return;
        }

        if (state.status === "timeout") {
            showPairingError(
                "No sensor was found. Reset the sensor and try again."
            );

            return;
        }

        if (state.status === "cancelled") {
            showPairingError(
                "Pairing was cancelled."
            );

            return;
        }

        if (state.status === "error") {
            showPairingError(
                state.message ||
                state.error ||
                "The sensor could not be paired."
            );
        }
    } catch (error) {
        console.error(error);

        showPairingError(
            "Unable to communicate with the pairing service."
        );
    }
}

// Requests gateway pairing mode and begins polling for a discovered ESP32 node.
async function startPairing() {
    const name =
        pairingName.value.trim();

    if (!name) {
        pairingName.focus();
        return;
    }

    pairingStart.disabled =
        true;

    pairingRegistrationSent =
        false;

    pairingSensorId =
        null;

    try {
        const response = await fetch(
            "/pairing/start",
            {
                method: "POST"
            }
        );

        const result =
            await response.json();

        if (!response.ok) {
            throw new Error(
                result.detail ||
                "Could not start pairing"
            );
        }

        pairingInProgress =
            true;

        showPairingSection(
            "search"
        );

        await checkPairingState();

        pairingTimer = setInterval(
            checkPairingState,
            1000
        );
    } catch (error) {
        console.error(error);

        showPairingError(
            error.message ||
            "Could not start pairing"
        );
    } finally {
        pairingStart.disabled =
            false;
    }
}

// Restarts discovery after a pairing attempt times out or fails.
async function retryPairing() {
    await stopPairing();

    pairingRegistrationSent =
        false;

    pairingSensorId =
        null;

    showPairingSection(
        "setup"
    );
}

sensorEnabled.addEventListener(
    "change",
    updateEnabledText
);

recommendedButton.addEventListener(
    "click",
    loadRecommendedValues
);

saveButton.addEventListener(
    "click",
    saveConfiguration
);

learningAction.addEventListener(
    "click",
    handleLearningAction
);

pairSensorButton.addEventListener(
    "click",
    openPairingModal
);

pairingStart.addEventListener(
    "click",
    startPairing
);

pairingCancel.addEventListener(
    "click",
    closePairingModal
);

pairingSearchCancel.addEventListener(
    "click",
    closePairingModal
);

pairingClose.addEventListener(
    "click",
    closePairingModal
);

pairingDone.addEventListener(
    "click",
    closePairingModal
);

pairingRetry.addEventListener(
    "click",
    retryPairing
);

pairingErrorCancel.addEventListener(
    "click",
    closePairingModal
);

enableTouchInputScrolling(
    document.querySelector(
        ".sensor-config-panel"
    ),
    'input[type="number"], input[type="text"], select'
);

loadSensors();

setInterval(
    loadLearningState,
    5000
);
