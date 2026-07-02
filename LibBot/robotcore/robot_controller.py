import os
import socket
from dataclasses import dataclass
from pathlib import Path

from dotenv import load_dotenv


BASE_DIR = Path(__file__).resolve().parent.parent
load_dotenv(BASE_DIR / ".env")


ESP_IP = os.getenv("LIBBOT_ESP_IP", "172.20.10.3")
ESP_PORT = int(os.getenv("LIBBOT_ESP_PORT", "5000"))
DEFAULT_TIMEOUT = float(os.getenv("LIBBOT_ESP_TIMEOUT", "3.0"))
SIMULATE_ROBOT = os.getenv("LIBBOT_SIMULATE_ROBOT", "0") == "1"


@dataclass
class RobotCommandResult:
    ok: bool
    command: str
    response: str | None = None
    error: str | None = None


def simulate_robot_response(command: str) -> RobotCommandResult:
    if command == "PING":
        return RobotCommandResult(
            ok=True,
            command=command,
            response="PONG:SIM"
        )

    if command == "STATUS":
        return RobotCommandResult(
            ok=True,
            command=command,
            response=(
                "STATUS:IDLE_AT_BASE,"
                "target=0,current=0,count=0,active=0,"
                "crossStable=0,last=SIM_IDLE,ip=SIM"
            )
        )

    if command == "STOP":
        return RobotCommandResult(
            ok=True,
            command=command,
            response="OK:STOP_SIM"
        )

    if command in ["RETURN", "CONTINUE"]:
        return RobotCommandResult(
            ok=True,
            command=command,
            response="OK:RETURN_SIM"
        )

    if command.startswith("GOTO:"):
        return RobotCommandResult(
            ok=True,
            command=command,
            response=f"OK:{command}:SIM"
        )

    return RobotCommandResult(
        ok=False,
        command=command,
        response="ERR:UNKNOWN_COMMAND_SIM",
        error="ERR:UNKNOWN_COMMAND_SIM"
    )


def send_robot_command(
    command: str,
    timeout: float = DEFAULT_TIMEOUT
) -> RobotCommandResult:
    """
    Gửi một lệnh text đến ESP32 qua TCP.

    Protocol:
    - Raspberry Pi gửi: COMMAND\\n
    - ESP32 trả: PONG, OK..., STATUS..., ERR...
    """

    command = (command or "").strip().upper()

    if not command:
        return RobotCommandResult(
            ok=False,
            command=command,
            error="Lệnh robot rỗng."
        )

    if SIMULATE_ROBOT:
        return simulate_robot_response(command)

    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
            sock.settimeout(timeout)
            sock.connect((ESP_IP, ESP_PORT))

            sock.sendall((command + "\n").encode("utf-8"))

            data = sock.recv(1024)

            if not data:
                return RobotCommandResult(
                    ok=False,
                    command=command,
                    error="ESP32 không trả phản hồi."
                )

            response = data.decode("utf-8", errors="ignore").strip()

            if response.startswith("ERR"):
                return RobotCommandResult(
                    ok=False,
                    command=command,
                    response=response,
                    error=response
                )

            return RobotCommandResult(
                ok=True,
                command=command,
                response=response
            )

    except socket.timeout:
        return RobotCommandResult(
            ok=False,
            command=command,
            error=f"Timeout khi gửi lệnh đến ESP32 {ESP_IP}:{ESP_PORT}."
        )

    except ConnectionRefusedError:
        return RobotCommandResult(
            ok=False,
            command=command,
            error=f"ESP32 từ chối kết nối tại {ESP_IP}:{ESP_PORT}."
        )

    except OSError as exc:
        return RobotCommandResult(
            ok=False,
            command=command,
            error=f"Lỗi socket khi kết nối ESP32: {exc}"
        )


def ping_robot() -> RobotCommandResult:
    return send_robot_command("PING")


def stop_robot() -> RobotCommandResult:
    return send_robot_command("STOP")


def get_robot_status() -> RobotCommandResult:
    return send_robot_command("STATUS")


def return_to_base() -> RobotCommandResult:
    return send_robot_command("RETURN")


def continue_robot() -> RobotCommandResult:
    return send_robot_command("CONTINUE")


def goto_table(table_id: int) -> RobotCommandResult:
    try:
        table_id = int(table_id)
    except (TypeError, ValueError):
        return RobotCommandResult(
            ok=False,
            command="GOTO",
            error="table_id không hợp lệ."
        )

    if table_id not in [1, 2, 3, 4]:
        return RobotCommandResult(
            ok=False,
            command=f"GOTO:{table_id}",
            error="Chỉ hỗ trợ bàn số 1, 2, 3, 4."
        )

    return send_robot_command(f"GOTO:{table_id}")


def parse_status_response(response: str | None) -> dict:
    """
    Parse chuỗi STATUS từ ESP32.

    Ví dụ:
    STATUS:WAITING_AT_TABLE,target=2,current=2,count=2,active=0,crossStable=0,last=ARRIVED:2,ip=172.20.10.3
    """

    data = {
        "raw": response or "",
        "mode": None,
        "target": None,
        "current": None,
        "count": None,
        "active": None,
        "crossStable": None,
        "last": None,
        "ip": None,
    }

    if not response:
        return data

    response = response.strip()

    if not response.startswith("STATUS:"):
        return data

    payload = response.replace("STATUS:", "", 1)
    parts = payload.split(",")

    if parts:
        data["mode"] = parts[0].strip()

    int_fields = {"target", "current", "count", "active", "crossStable"}

    for part in parts[1:]:
        if "=" not in part:
            continue

        key, value = part.split("=", 1)
        key = key.strip()
        value = value.strip()

        if key in int_fields:
            try:
                data[key] = int(value)
            except ValueError:
                data[key] = None
        else:
            data[key] = value

    return data


if __name__ == "__main__":
    print("Testing LibBot ESP32 connection...")
    print(f"ESP32: {ESP_IP}:{ESP_PORT}")

    ping_result = ping_robot()
    print(ping_result)

    if ping_result.ok:
        status_result = get_robot_status()
        print(status_result)

        if status_result.ok:
            print(parse_status_response(status_result.response))