from functools import reduce
import operator
import re

from django.db.models import Q
from django.utils import timezone

from rest_framework import generics
from rest_framework.decorators import api_view
from rest_framework.response import Response

from .models import Book, BorrowRecord, RobotTaskLog
from .serializers import BookSerializer

from robotcore.robot_controller import (
    goto_table,
    ping_robot,
    get_robot_status,
    stop_robot,
    return_to_base,
    parse_status_response,
)

from robotcore.state import (
    load_bot_state,
    save_bot_state,
    append_bot_message,
)


# =====================================================
# ROBOT BUSY / TASK HELPERS
# =====================================================
BUSY_STATUSES = {
    "called",
    "moving",
    "arrived",
    "listening",
    "thinking",
    "speaking",
    "returning",
}


def is_robot_busy_state(state):
    robot_status = state.get("robot_status")
    return robot_status in BUSY_STATUSES


def get_active_robot_task():
    return RobotTaskLog.objects.filter(
        status__in=["created", "moving", "arrived", "returning"]
    ).order_by("-started_at").first()


# =====================================================
# ROBOT STATE UPDATE FROM ESP32 STATUS
# =====================================================
def update_state_from_esp_status(status_data):
    state = load_bot_state()

    esp_mode = status_data.get("mode")
    target = status_data.get("target")
    current = status_data.get("current")
    last = status_data.get("last")

    state["current_stop"] = current if current is not None else state.get("current_stop", 0)

    if target:
        state["target_table"] = target

    if esp_mode == "GOING_TO_TABLE":
        state["mode"] = "moving"
        state["robot_status"] = "moving"
        state["last_event"] = last or "robot_moving"

        active_task = get_active_robot_task()
        if active_task and active_task.status == "created":
            active_task.status = "moving"
            active_task.save()

    elif esp_mode == "WAITING_AT_TABLE":
        current_robot_status = state.get("robot_status")

        # Không ghi đè state khi chatbot đang nghe/xử lý/trả lời.
        if current_robot_status in ["listening", "thinking", "speaking"]:
            state["current_stop"] = current if current is not None else state.get("current_stop", 0)
            state["last_event"] = last or state.get("last_event", "chatbot_active_at_table")
            save_bot_state(state)
            return state

        arrived_table = current or target or state.get("target_table")

        state["mode"] = "arrived"
        state["robot_status"] = "arrived"
        state["target_table"] = arrived_table
        state["last_event"] = last or f"arrived_table_{arrived_table}"

        arrived_text = f"LibBot đã đến bàn {arrived_table}."
        messages = state.get("messages", [])
        already_has_message = any(
            msg.get("text") == arrived_text
            for msg in messages
        )

        if not already_has_message:
            append_bot_message(state, "system", arrived_text)

        active_task = get_active_robot_task()
        if active_task and active_task.status in ["created", "moving"]:
            active_task.status = "arrived"
            active_task.arrived_at = timezone.now()
            active_task.save()

    elif esp_mode == "RETURNING_TO_BASE":
        state["mode"] = "moving"
        state["robot_status"] = "returning"
        state["last_event"] = last or "returning_to_base"

        return_text = "LibBot đang quay về trạm."
        messages = state.get("messages", [])
        already_has_message = any(
            msg.get("text") == return_text
            for msg in messages
        )

        if not already_has_message:
            append_bot_message(state, "system", return_text)

        active_task = get_active_robot_task()
        if active_task and active_task.status != "returning":
            active_task.status = "returning"
            active_task.save()

    elif esp_mode == "IDLE_AT_BASE":
        previous_status = state.get("robot_status")

        state["mode"] = "idle"
        state["robot_status"] = "idle"
        state["target_table"] = None
        state["current_stop"] = 0
        state["last_event"] = last or "idle_at_base"

        if previous_status == "returning":
            append_bot_message(state, "system", "LibBot đã quay về trạm.")

        active_task = get_active_robot_task()
        if active_task and active_task.status in ["returning", "arrived"]:
            active_task.status = "completed"
            active_task.finished_at = timezone.now()
            active_task.save()

    elif esp_mode == "STOPPED":
        state["mode"] = "idle"
        state["robot_status"] = "stopped"
        state["last_event"] = last or "robot_stopped"

        active_task = get_active_robot_task()
        if active_task:
            active_task.status = "stopped"
            active_task.finished_at = timezone.now()
            active_task.save()

    elif esp_mode == "ERROR":
        state["mode"] = "error"
        state["robot_status"] = "error"
        state["last_event"] = last or "robot_error"

        active_task = get_active_robot_task()
        if active_task:
            active_task.status = "error"
            active_task.error_message = last or "ESP32 reported ERROR"
            active_task.finished_at = timezone.now()
            active_task.save()

    save_bot_state(state)
    return state


# =====================================================
# BOOK SEARCH API
# =====================================================
class BookSearchView(generics.ListAPIView):
    serializer_class = BookSerializer

    def get_queryset(self):
        query = self.request.query_params.get("search", "").lower()

        if not query:
            return Book.objects.all()

        clean_query = re.sub(r"[^\w\s]", " ", query)
        raw_words = clean_query.split()

        stop_words = {
            "giáo", "trình", "sách", "cuốn", "quyển", "tài", "liệu",
            "tìm", "thông", "tin", "về", "của", "cho", "mình", "tôi", "bạn",
            "phim", "tập", "ở", "đâu", "vị", "trí", "giá", "tiền", "nhiêu",
            "kệ", "nào", "tác", "giả", "ai", "viết", "mua", "bán", "hỏi", "xem",
            "liệt", "kê", "những", "các", "loại", "thể"
        }

        words = [
            word
            for word in raw_words
            if word not in stop_words and len(word) > 1
        ]

        if not words:
            return Book.objects.none()

        q_objects = reduce(
            operator.and_,
            (
                Q(title__icontains=word) |
                Q(author__icontains=word) |
                Q(category__icontains=word)
                for word in words
            )
        )

        return Book.objects.filter(q_objects)


# =====================================================
# BOOK DETAIL APIs
# =====================================================
@api_view(["GET"])
def book_availability(request, pk):
    try:
        book = Book.objects.get(pk=pk)
        return Response({
            "available": book.is_available,
            "quantity": book.quantity
        })

    except Book.DoesNotExist:
        return Response({
            "error": "Book not found"
        }, status=404)


@api_view(["GET"])
def book_location(request, pk):
    try:
        book = Book.objects.select_related("location").get(pk=pk)

        if book.location:
            loc = book.location
            data = {
                "shelf": loc.shelf,
                "row": loc.row,
                "floor": loc.floor,
                "map_image": loc.map_image.url if loc.map_image else None,
            }
        else:
            data = {
                "shelf": None,
                "row": None,
                "floor": None
            }

        return Response(data)

    except Book.DoesNotExist:
        return Response({
            "error": "Book not found"
        }, status=404)


@api_view(["GET"])
def borrowed_books(request):
    records = BorrowRecord.objects.filter(
        return_date__isnull=True
    ).select_related("book")

    data = [
        {
            "book_title": record.book.title,
            "borrower": record.borrower_name,
            "due_date": record.due_date
        }
        for record in records
    ]

    return Response(data)


# =====================================================
# ROBOT CALL API - BUSY PROTECTION + TASK LOG
# =====================================================
@api_view(["POST"])
def call_robot(request):
    table_id = request.data.get("table_id")

    try:
        table_id = int(table_id)
    except (TypeError, ValueError):
        return Response({
            "ok": False,
            "error": "table_id không hợp lệ."
        }, status=400)

    if table_id not in [1, 2, 3, 4]:
        return Response({
            "ok": False,
            "error": "Chỉ hỗ trợ bàn số 1, 2, 3, 4."
        }, status=400)

    state = load_bot_state()

    if is_robot_busy_state(state):
        return Response({
            "ok": False,
            "error": "LibBot đang bận, vui lòng thử lại sau.",
            "robot_status": state.get("robot_status"),
            "target_table": state.get("target_table")
        }, status=409)

    active_task = get_active_robot_task()
    if active_task:
        return Response({
            "ok": False,
            "error": "LibBot đang có task chưa hoàn tất.",
            "active_task": {
                "id": active_task.id,
                "table_id": active_task.table_id,
                "command": active_task.command,
                "status": active_task.status,
            }
        }, status=409)

    task_log = RobotTaskLog.objects.create(
        table_id=table_id,
        command=f"GOTO:{table_id}",
        status="created"
    )

    state["mode"] = "called"
    state["robot_status"] = "called"
    state["target_table"] = table_id
    state["last_event"] = f"call_table_{table_id}"

    append_bot_message(
        state,
        "system",
        f"Có yêu cầu gọi LibBot đến bàn {table_id}."
    )
    save_bot_state(state)

    result = goto_table(table_id)

    if result.ok:
        task_log.status = "moving"
        task_log.esp_response = result.response or ""
        task_log.save()

        state = load_bot_state()
        state["mode"] = "moving"
        state["robot_status"] = "moving"
        state["target_table"] = table_id
        state["last_event"] = f"goto_table_{table_id}_sent"

        append_bot_message(
            state,
            "system",
            f"LibBot đang di chuyển đến bàn {table_id}."
        )
        save_bot_state(state)

        return Response({
            "ok": True,
            "message": f"LibBot đang di chuyển đến bàn {table_id}.",
            "table_id": table_id,
            "robot_status": "moving",
            "esp_response": result.response
        })

    task_log.status = "error"
    task_log.error_message = result.error or ""
    task_log.esp_response = result.response or ""
    task_log.finished_at = timezone.now()
    task_log.save()

    state = load_bot_state()
    state["mode"] = "error"
    state["robot_status"] = "error"
    state["target_table"] = table_id
    state["last_event"] = "esp32_connection_failed"

    append_bot_message(
        state,
        "system",
        f"Lỗi: không gửi được lệnh đến ESP32. {result.error}"
    )
    save_bot_state(state)

    return Response({
        "ok": False,
        "error": result.error,
        "table_id": table_id,
        "robot_status": "error"
    }, status=503)


# =====================================================
# ROBOT BASIC APIs
# =====================================================
@api_view(["GET"])
def robot_ping(request):
    result = ping_robot()

    if result.ok:
        return Response({
            "ok": True,
            "response": result.response
        })

    return Response({
        "ok": False,
        "error": result.error
    }, status=503)


@api_view(["GET"])
def robot_status(request):
    result = get_robot_status()

    if result.ok:
        return Response({
            "ok": True,
            "response": result.response
        })

    return Response({
        "ok": False,
        "error": result.error
    }, status=503)


@api_view(["POST"])
def robot_stop(request):
    result = stop_robot()

    state = load_bot_state()

    if result.ok:
        state["mode"] = "idle"
        state["robot_status"] = "stopped"
        state["last_event"] = "manual_stop"

        append_bot_message(
            state,
            "system",
            "Đã gửi lệnh dừng robot."
        )
        save_bot_state(state)

        active_task = get_active_robot_task()
        if active_task:
            active_task.status = "stopped"
            active_task.esp_response = result.response or ""
            active_task.finished_at = timezone.now()
            active_task.save()

        return Response({
            "ok": True,
            "response": result.response
        })

    state["mode"] = "error"
    state["robot_status"] = "error"
    state["last_event"] = "manual_stop_failed"

    append_bot_message(
        state,
        "system",
        f"Lỗi khi gửi lệnh dừng robot: {result.error}"
    )
    save_bot_state(state)

    active_task = get_active_robot_task()
    if active_task:
        active_task.status = "error"
        active_task.error_message = result.error or ""
        active_task.finished_at = timezone.now()
        active_task.save()

    return Response({
        "ok": False,
        "error": result.error
    }, status=503)


@api_view(["GET"])
def robot_poll_status(request):
    result = get_robot_status()

    if not result.ok:
        state = load_bot_state()
        state["mode"] = "error"
        state["robot_status"] = "error"
        state["last_event"] = "esp32_status_poll_failed"

        append_bot_message(
            state,
            "system",
            f"Lỗi khi đọc trạng thái ESP32: {result.error}"
        )
        save_bot_state(state)

        active_task = get_active_robot_task()
        if active_task:
            active_task.status = "error"
            active_task.error_message = result.error or ""
            active_task.finished_at = timezone.now()
            active_task.save()

        return Response({
            "ok": False,
            "error": result.error,
            "robot_status": state["robot_status"]
        }, status=503)

    status_data = parse_status_response(result.response)
    state = update_state_from_esp_status(status_data)

    return Response({
        "ok": True,
        "esp_response": result.response,
        "status_data": status_data,
        "bot_state": state
    })


@api_view(["POST"])
def robot_return(request):
    result = return_to_base()

    state = load_bot_state()

    if result.ok:
        state["mode"] = "moving"
        state["robot_status"] = "returning"
        state["last_event"] = "return_to_base"

        append_bot_message(
            state,
            "system",
            "LibBot đang quay về trạm."
        )
        save_bot_state(state)

        active_task = get_active_robot_task()
        if active_task:
            active_task.status = "returning"
            active_task.esp_response = result.response or ""
            active_task.save()

        return Response({
            "ok": True,
            "message": "Đã gửi lệnh cho LibBot quay về trạm.",
            "robot_status": "returning",
            "esp_response": result.response
        })

    state["mode"] = "error"
    state["robot_status"] = "error"
    state["last_event"] = "return_to_base_failed"

    append_bot_message(
        state,
        "system",
        f"Lỗi khi gửi lệnh quay về trạm: {result.error}"
    )
    save_bot_state(state)

    active_task = get_active_robot_task()
    if active_task:
        active_task.status = "error"
        active_task.error_message = result.error or ""
        active_task.finished_at = timezone.now()
        active_task.save()

    return Response({
        "ok": False,
        "error": result.error
    }, status=503)