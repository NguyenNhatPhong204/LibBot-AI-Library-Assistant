import os
import time
from pathlib import Path

import django
from dotenv import load_dotenv


BASE_DIR = Path(__file__).resolve().parent
load_dotenv(BASE_DIR / ".env")

os.environ.setdefault("DJANGO_SETTINGS_MODULE", "libapi.settings")
django.setup()

from django.utils import timezone

from books.models import RobotTaskLog
from books.views import update_state_from_esp_status
from robotcore.robot_controller import (
    get_robot_status,
    parse_status_response,
    stop_robot,
)
from robotcore.state import load_bot_state, save_bot_state, append_bot_message


POLL_INTERVAL_SECONDS = float(os.getenv("LIBBOT_POLL_INTERVAL", "1.0"))
MAX_CONSECUTIVE_ERRORS = int(os.getenv("LIBBOT_MAX_POLL_ERRORS", "3"))

ARRIVAL_TIMEOUT_SECONDS = int(os.getenv("LIBBOT_ARRIVAL_TIMEOUT", "90"))
RETURN_TIMEOUT_SECONDS = int(os.getenv("LIBBOT_RETURN_TIMEOUT", "90"))


def get_active_task_for_timeout():
    return RobotTaskLog.objects.filter(
        status__in=["created", "moving", "returning"]
    ).order_by("-started_at").first()


def get_active_task_for_error():
    return RobotTaskLog.objects.filter(
        status__in=["created", "moving", "arrived", "returning"]
    ).order_by("-started_at").first()


def append_unique_system_message(state, text):
    messages = state.get("messages", [])

    already_has_message = any(
        msg.get("text") == text
        for msg in messages
    )

    if not already_has_message:
        append_bot_message(state, "system", text)

    return state


def mark_timeout_and_stop(task, reason):
    print("[TIMEOUT]", reason)

    stop_result = stop_robot()

    state = load_bot_state()
    state["mode"] = "error"
    state["robot_status"] = "error"
    state["last_event"] = "robot_timeout"

    append_unique_system_message(state, reason)
    save_bot_state(state)

    task.status = "timeout"
    task.error_message = reason

    if stop_result.response:
        task.esp_response = stop_result.response

    if stop_result.error:
        task.error_message = f"{reason} | STOP error: {stop_result.error}"

    task.finished_at = timezone.now()
    task.save()


def check_task_timeout():
    task = get_active_task_for_timeout()

    if not task:
        return False

    now = timezone.now()

    if task.status in ["created", "moving"]:
        elapsed = (now - task.started_at).total_seconds()

        if elapsed > ARRIVAL_TIMEOUT_SECONDS:
            mark_timeout_and_stop(
                task,
                f"Timeout: LibBot không đến bàn {task.table_id} trong {ARRIVAL_TIMEOUT_SECONDS} giây.",
            )
            return True

    if task.status == "returning":
        start_time = task.arrived_at or task.started_at
        elapsed = (now - start_time).total_seconds()

        if elapsed > RETURN_TIMEOUT_SECONDS:
            mark_timeout_and_stop(
                task,
                f"Timeout: LibBot không quay về trạm trong {RETURN_TIMEOUT_SECONDS} giây.",
            )
            return True

    return False


def mark_connection_lost(error_text):
    state = load_bot_state()
    state["mode"] = "error"
    state["robot_status"] = "error"
    state["last_event"] = "esp32_connection_lost"

    append_unique_system_message(state, error_text)
    save_bot_state(state)

    active_task = get_active_task_for_error()

    if active_task:
        active_task.status = "error"
        active_task.error_message = error_text
        active_task.finished_at = timezone.now()
        active_task.save()


def main():
    print("[LibBot Worker] Robot status worker started.")

    last_esp_response = None
    last_error = None
    consecutive_errors = 0

    while True:
        try:
            if check_task_timeout():
                time.sleep(POLL_INTERVAL_SECONDS)
                continue

            result = get_robot_status()

            if result.ok:
                consecutive_errors = 0
                last_error = None

                if result.response != last_esp_response:
                    print("[ESP32]", result.response)
                    last_esp_response = result.response

                status_data = parse_status_response(result.response)
                update_state_from_esp_status(status_data)

            else:
                consecutive_errors += 1

                if result.error != last_error:
                    print("[ESP32 ERROR]", result.error)
                    last_error = result.error

                if consecutive_errors >= MAX_CONSECUTIVE_ERRORS:
                    error_text = f"Mất kết nối ESP32: {result.error}"
                    mark_connection_lost(error_text)

            time.sleep(POLL_INTERVAL_SECONDS)

        except KeyboardInterrupt:
            print("[LibBot Worker] Robot status worker stopped.")
            break

        except Exception as exc:
            print(f"[WORKER ERROR] robot_status_worker: {exc}")

            state = load_bot_state()
            state["mode"] = "error"
            state["robot_status"] = "error"
            state["last_event"] = "robot_status_worker_error"

            append_unique_system_message(
                state,
                f"Lỗi robot_status_worker: {exc}",
            )

            save_bot_state(state)
            time.sleep(2)


if __name__ == "__main__":
    main()