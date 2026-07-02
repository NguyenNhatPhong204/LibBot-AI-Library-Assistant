import json
import os
from pathlib import Path

from django.conf import settings
from django.utils import timezone


BOT_STATE_PATH = Path(settings.BASE_DIR) / "static" / "bot_state.json"


def get_default_bot_state():
    return {
        "mode": "idle",
        "robot_status": "idle",
        "target_table": None,
        "current_stop": 0,
        "last_event": "system_start",
        "updated_at": "",
        "messages": []
    }


def load_bot_state():
    try:
        if not BOT_STATE_PATH.exists():
            return get_default_bot_state()

        with open(BOT_STATE_PATH, "r", encoding="utf-8") as file:
            data = json.load(file)

        default_state = get_default_bot_state()
        default_state.update(data)

        if not isinstance(default_state.get("messages"), list):
            default_state["messages"] = []

        return default_state

    except Exception:
        return get_default_bot_state()


def save_bot_state(state):
    BOT_STATE_PATH.parent.mkdir(parents=True, exist_ok=True)

    state["updated_at"] = timezone.localtime().strftime("%Y-%m-%d %H:%M:%S")

    temp_path = BOT_STATE_PATH.with_suffix(".json.tmp")

    with open(temp_path, "w", encoding="utf-8") as file:
        json.dump(state, file, ensure_ascii=False, indent=2)

    os.replace(temp_path, BOT_STATE_PATH)


def append_bot_message(state, sender, text, max_messages=12):
    messages = state.get("messages", [])

    if not isinstance(messages, list):
        messages = []

    messages.append({
        "sender": sender,
        "text": text
    })

    state["messages"] = messages[-max_messages:]
    return state


def set_robot_state(
    mode=None,
    robot_status=None,
    target_table=None,
    current_stop=None,
    last_event=None,
    message=None,
    sender="system",
    clear_target_table=False,
):
    state = load_bot_state()

    if mode is not None:
        state["mode"] = mode

    if robot_status is not None:
        state["robot_status"] = robot_status

    if clear_target_table:
        state["target_table"] = None
    elif target_table is not None:
        state["target_table"] = target_table

    if current_stop is not None:
        state["current_stop"] = current_stop

    if last_event is not None:
        state["last_event"] = last_event

    if message:
        append_bot_message(state, sender, message)

    save_bot_state(state)
    return state


def reset_bot_state(message=None):
    state = get_default_bot_state()

    if message:
        append_bot_message(state, "system", message)

    save_bot_state(state)
    return state