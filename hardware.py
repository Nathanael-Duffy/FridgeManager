# =====================================================
# HARDWARE
# =====================================================
"""
Controls the Raspberry Pi buzzer and fan while keeping hardware activity separate from gateway message processing.
"""

import threading
import time

from gpiozero import OutputDevice


BUZZER_GPIO = 18
FAN_GPIO = 25

FAN_ON_TEMPERATURE = 50.0
FAN_OFF_TEMPERATURE = 45.0

CPU_TEMPERATURE_PATH = (
    "/sys/class/thermal/thermal_zone0/temp"
)


# Owns the Raspberry Pi GPIO-facing actuator state. One shared controller instance
# coordinates buzzer/fan behaviour with the alert state maintained by gateway_mqtt.py.
class HardwareController:
    # Initialises the controller state and shared objects required by this class.
    def __init__(self):
        self.buzzer = None
        self.fan = None

        self.buzzer_thread = None
        self.fan_thread = None

        self.stop_event = threading.Event()

        self.alert_lock = threading.Lock()

        self.active_alerts = set()
        self.acknowledged_alerts = set()

        self.fan_state = False

    # Initialises the GPIO outputs and starts the background buzzer and fan worker threads.
    def start(self):
        self.buzzer = OutputDevice(
            BUZZER_GPIO,
            active_high=True,
            initial_value=False,
        )

        self.fan = OutputDevice(
            FAN_GPIO,
            active_high=True,
            initial_value=False,
        )

        self.stop_event.clear()

        self.buzzer_thread = threading.Thread(
            target=self._buzzer_worker,
            name="fridgemanager-buzzer",
            daemon=True,
        )

        self.fan_thread = threading.Thread(
            target=self._fan_worker,
            name="fridgemanager-fan",
            daemon=True,
        )

        self.buzzer_thread.start()
        self.fan_thread.start()

        print(
            f"Buzzer ready on GPIO{BUZZER_GPIO}"
        )

        print(
            f"Fan ready on GPIO{FAN_GPIO}"
        )

    # Stops background hardware workers and safely turns off/closes the GPIO outputs.
    def stop(self):
        self.stop_event.set()

        if self.buzzer_thread is not None:
            self.buzzer_thread.join(
                timeout=2
            )

        if self.fan_thread is not None:
            self.fan_thread.join(
                timeout=2
            )

        if self.buzzer is not None:
            self.buzzer.off()
            self.buzzer.close()

        if self.fan is not None:
            self.fan.off()
            self.fan.close()

        self.fan_state = False

    # Returns whether any active alert still requires the buzzer because it has not been acknowledged.
    def buzzer_required(self):
        with self.alert_lock:
            return bool(
                self.active_alerts
                - self.acknowledged_alerts
            )

    # Marks the current active alerts as acknowledged and immediately silences the buzzer output.
    def acknowledge_alerts(self):
        with self.alert_lock:
            self.acknowledged_alerts.update(
                self.active_alerts
            )

            if self.buzzer is not None:
                self.buzzer.off()

            return len(
                self.active_alerts
            )

    # Returns a thread-safe snapshot of active and acknowledged alert state for the API/MQTT layers.
    def get_alert_state(self):
        with self.alert_lock:
            active = set(
                self.active_alerts
            )

            acknowledged = set(
                self.acknowledged_alerts
            )

        return active, acknowledged

    # Handles the update alert sets operation used by this part of Fridge Manager.
    def update_alert_sets(
        self,
        started,
        cleared,
    ):
        with self.alert_lock:
            self.active_alerts.update(
                started
            )

            self.active_alerts.difference_update(
                cleared
            )

            self.acknowledged_alerts.difference_update(
                cleared
            )

    # Handles the buzzer worker operation used by this part of Fridge Manager.
    def _buzzer_worker(self):
        while not self.stop_event.is_set():
            if self.buzzer_required():
                self.buzzer.on()

                if self.stop_event.wait(0.25):
                    break

                self.buzzer.off()

                if self.stop_event.wait(0.75):
                    break

            else:
                self.buzzer.off()

                if self.stop_event.wait(0.10):
                    break

        self.buzzer.off()

    # Handles the read cpu temperature operation used by this part of Fridge Manager.
    def read_cpu_temperature(self):
        try:
            with open(
                CPU_TEMPERATURE_PATH,
                "r",
                encoding="utf-8",
            ) as temperature_file:
                raw_value = (
                    temperature_file
                    .read()
                    .strip()
                )

            return float(raw_value) / 1000.0

        except (
            OSError,
            ValueError,
        ) as error:
            print(
                "Could not read CPU temperature:"
            )
            print(error)

            return None

    # Handles the update fan operation used by this part of Fridge Manager.
    def update_fan(self):
        temperature = (
            self.read_cpu_temperature()
        )

        if temperature is None:
            return

        if (
            not self.fan_state
            and temperature
            >= FAN_ON_TEMPERATURE
        ):
            self.fan.on()
            self.fan_state = True

            print(
                f"Fan ON at "
                f"{temperature:.1f} C"
            )

        elif (
            self.fan_state
            and temperature
            <= FAN_OFF_TEMPERATURE
        ):
            self.fan.off()
            self.fan_state = False

            print(
                f"Fan OFF at "
                f"{temperature:.1f} C"
            )

    # Handles the fan worker operation used by this part of Fridge Manager.
    def _fan_worker(self):
        while not self.stop_event.is_set():
            self.update_fan()

            if self.stop_event.wait(5):
                break

        if self.fan is not None:
            self.fan.off()

        self.fan_state = False


hardware = HardwareController()
