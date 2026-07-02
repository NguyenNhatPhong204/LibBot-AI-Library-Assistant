import json
import os
import re
import subprocess
import tempfile
import time
from pathlib import Path

import django
import pygame
import requests
import speech_recognition as sr
from dotenv import load_dotenv
from openai import OpenAI


BASE_DIR = Path(__file__).resolve().parent
load_dotenv(BASE_DIR / ".env")

os.environ.setdefault("DJANGO_SETTINGS_MODULE", "libapi.settings")
django.setup()

from robotcore.state import load_bot_state, save_bot_state, append_bot_message


OPENAI_API_KEY = os.getenv("OPENAI_API_KEY", "").strip()

MODEL = os.getenv("OPENAI_MODEL", "gpt-4o-mini")
TTS_MODEL = os.getenv("OPENAI_TTS_MODEL", "tts-1")
TTS_VOICE = os.getenv("OPENAI_TTS_VOICE", "nova")

API_BASE = os.getenv("LIBBOT_API_BASE", "http://127.0.0.1:8000/api")
RETURN_API_URL = f"{API_BASE}/robot/return/"

LISTEN_TIMEOUT = float(os.getenv("LIBBOT_LISTEN_TIMEOUT", "5"))
PHRASE_TIME_LIMIT = float(os.getenv("LIBBOT_PHRASE_TIME_LIMIT", "6"))
IDLE_SLEEP_SECONDS = float(os.getenv("LIBBOT_CHATBOT_IDLE_SLEEP", "0.5"))
AFTER_SPEAK_SLEEP_SECONDS = float(os.getenv("LIBBOT_AFTER_SPEAK_SLEEP", "0.8"))

AUDIO_CARD = os.getenv("LIBBOT_AUDIO_CARD", "4")
AUDIO_CONTROL = os.getenv("LIBBOT_AUDIO_CONTROL", "PCM")
AUDIO_VOLUME = os.getenv("LIBBOT_AUDIO_VOLUME", "100%")


if not OPENAI_API_KEY:
    print("[OPENAI ERROR] Chưa có OPENAI_API_KEY trong .env.")
    print("[OPENAI ERROR] Chatbot worker vẫn chạy, nhưng LLM/TTS sẽ lỗi cho đến khi khai báo key.")

client = OpenAI(api_key=OPENAI_API_KEY) if OPENAI_API_KEY else None
robot_ear = sr.Recognizer()


def setup_audio():
    try:
        print(f"[AUDIO] Đang cài đặt âm lượng hệ thống: card={AUDIO_CARD}, control={AUDIO_CONTROL}, volume={AUDIO_VOLUME}")
        subprocess.run(
            ["amixer", "-c", AUDIO_CARD, "set", AUDIO_CONTROL, AUDIO_VOLUME],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        print("[AUDIO] Đã cài đặt âm lượng thành công.")
    except Exception as exc:
        print(f"[AUDIO WARNING] Không thể cài đặt âm lượng bằng amixer: {exc}")

    pygame.mixer.init()
    pygame.mixer.music.set_volume(1.0)


def set_state(mode, robot_status=None, last_event=None):
    state = load_bot_state()
    state["mode"] = mode
    state["robot_status"] = robot_status or mode

    if last_event:
        state["last_event"] = last_event

    save_bot_state(state)
    return state


def add_message(sender, text):
    state = load_bot_state()
    append_bot_message(state, sender, text)
    save_bot_state(state)


def should_chatbot_listen():
    state = load_bot_state()
    return (
        state.get("robot_status") == "arrived"
        and state.get("mode") == "arrived"
    )


def search_books(query):
    try:
        resp = requests.get(
            f"{API_BASE}/books/search/",
            params={"search": query},
            timeout=3,
        )
        if resp.status_code == 200:
            return resp.json()
        return []
    except Exception as exc:
        print(f"[API ERROR] search_books: {exc}")
        return []


def get_availability(book_id):
    try:
        resp = requests.get(
            f"{API_BASE}/books/{book_id}/availability/",
            timeout=3,
        )
        if resp.status_code == 200:
            return resp.json()
        return None
    except Exception as exc:
        print(f"[API ERROR] get_availability: {exc}")
        return None


def get_location(book_id):
    try:
        resp = requests.get(
            f"{API_BASE}/books/{book_id}/location/",
            timeout=3,
        )
        if resp.status_code == 200:
            return resp.json()
        return None
    except Exception as exc:
        print(f"[API ERROR] get_location: {exc}")
        return None


def get_borrowed_books():
    try:
        resp = requests.get(
            f"{API_BASE}/borrowed/",
            timeout=3,
        )
        if resp.status_code == 200:
            return resp.json()
        return []
    except Exception as exc:
        print(f"[API ERROR] get_borrowed_books: {exc}")
        return []


def is_end_conversation_phrase(text):
    normalized = text.lower().strip()

    end_phrases = [
        "xong rồi",
        "xong",
        "hết rồi",
        "được rồi",
        "ok rồi",
        "cảm ơn",
        "cảm ơn bạn",
        "cảm ơn đã hỗ trợ",
        "cảm ơn libbot",
        "không cần nữa",
        "không cần hỗ trợ nữa",
        "tạm biệt",
        "bye",
        "bai",
        "thôi",
        "thôi được rồi",
        "tôi đã hỏi xong",
    ]

    return any(phrase in normalized for phrase in end_phrases)


def request_robot_return_to_base():
    try:
        resp = requests.post(RETURN_API_URL, timeout=5)

        if resp.status_code == 200:
            print("[ROBOT RETURN] OK", resp.text)
            return True

        print("[ROBOT RETURN] FAILED", resp.status_code, resp.text)
        return False

    except Exception as exc:
        print(f"[ROBOT RETURN ERROR] {exc}")
        return False


def parse_intent_with_llm(question):
    if not client:
        return {
            "intent": "general",
            "search_query": "",
        }

    system_prompt = """
        Bạn là hệ thống trích xuất thông tin thư viện. Luôn trả về JSON hợp lệ, chính xác.

        Phân tích câu hỏi người dùng và trả về:
        - intent: chọn một trong ba giá trị: search_book, check_borrowed, general.
        - search_query: tên sách, tác giả hoặc thể loại cốt lõi nhất.

        Quy tắc:
        - Nếu hỏi vị trí sách, tác giả, số lượng, còn hay hết, thể loại, kệ nào: intent = search_book.
        - Nếu hỏi sách nào đang được mượn, ai mượn, hạn trả: intent = check_borrowed.
        - Nếu chỉ chào hỏi hoặc hỏi chung không liên quan dữ liệu thư viện: intent = general.
        - search_query phải loại bỏ từ nhiễu như: sách, giáo trình, cuốn, quyển, tìm, vị trí, ở đâu, ở vị trí nào, kệ nào, ai viết, cho tôi hỏi.
        """

    try:
        response = client.chat.completions.create(
            model=MODEL,
            messages=[
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": question},
            ],
            response_format={"type": "json_object"},
            temperature=0,
        )

        content = response.choices[0].message.content
        return json.loads(content)

    except Exception as exc:
        print(f"[LLM ERROR] parse_intent_with_llm: {exc}")
        return {
            "intent": "general",
            "search_query": "",
        }


def get_library_data(question):
    intent_data = parse_intent_with_llm(question)

    intent = intent_data.get("intent", "general")
    query = intent_data.get("search_query", "")

    print(f"[INTENT] {intent_data}")

    if intent == "check_borrowed":
        borrowed = get_borrowed_books()

        if borrowed:
            items = []
            for item in borrowed:
                items.append(
                    f"'{item.get('book_title')}' đang được mượn bởi "
                    f"{item.get('borrower')}, hạn trả {item.get('due_date')}"
                )
            return "Dữ liệu DB: " + "; ".join(items)

        return "Dữ liệu DB: Hiện không có sách nào đang được mượn."

    if intent == "search_book" and query:
        books = search_books(query)

        if not books:
            return f"Dữ liệu DB: Không tìm thấy sách hoặc tác giả nào khớp với từ khóa '{query}'."

        results = []

        for book in books[:5]:
            availability = get_availability(book["id"])
            location = get_location(book["id"])

            info = (
                f"- Sách '{book.get('title', 'Không rõ')}', "
                f"tác giả {book.get('author', 'Không rõ')}, "
                f"thể loại {book.get('category', 'Chưa phân loại')}. "
            )

            if availability:
                info += (
                    f"Trạng thái: {'có sẵn' if availability.get('available') else 'đã hết'}, "
                    f"số lượng {availability.get('quantity', 0)} cuốn. "
                )

            if location and location.get("shelf"):
                info += (
                    f"Vị trí: {location.get('shelf')}, "
                    f"{location.get('row')}, "
                    f"tầng {location.get('floor')}."
                )
            else:
                info += "Chưa có thông tin vị trí cụ thể."

            results.append(info)

        return "Dữ liệu DB:\n" + "\n".join(results)

    return "Người dùng đang trò chuyện chung. Không có dữ liệu DB cụ thể."


def generate_answer(question, api_context):
    if not client:
        return "Xin lỗi, hệ thống AI chưa được cấu hình API key."

    try:
        completion = client.chat.completions.create(
            model=MODEL,
            messages=[
                {
                    "role": "system",
                    "content": (
                        "Bạn là LibBot, robot trợ lý thư viện. "
                        "Trả lời tiếng Việt tự nhiên, ngắn gọn, rõ ràng. "
                        "Dựa vào Dữ liệu DB nếu có. "
                        "Không bịa vị trí sách, số lượng hoặc tình trạng mượn trả. "
                        "Thư viện chỉ cho mượn, không bán sách. "
                        "Không dùng markdown."
                    ),
                },
                {
                    "role": "user",
                    "content": f"Câu hỏi: {question}\n\n{api_context}",
                },
            ],
            temperature=0.5,
            max_completion_tokens=160,
        )

        answer = completion.choices[0].message.content
        answer = re.sub(r"\*\*", "", answer)
        return answer.strip()

    except Exception as exc:
        print(f"[LLM ERROR] generate_answer: {exc}")
        return "Xin lỗi, tôi đang gặp lỗi kết nối hệ thống. Bạn vui lòng thử lại sau."


def speak_tts(text):
    temp_filename = None

    try:
        clean_text = re.sub(r"\*\*", "", text or "").strip()

        if not clean_text:
            print("[TTS WARNING] Nội dung rỗng, bỏ qua phát âm thanh.")
            return

        if not client:
            print("[TTS ERROR] Chưa có OpenAI client.")
            return

        audio_response = client.audio.speech.create(
            model=TTS_MODEL,
            voice=TTS_VOICE,
            input=clean_text,
        )

        with tempfile.NamedTemporaryFile(delete=False, suffix=".mp3") as file:
            temp_filename = file.name
            file.write(audio_response.content)

        pygame.mixer.music.load(temp_filename)
        pygame.mixer.music.play()

        while pygame.mixer.music.get_busy():
            pygame.time.wait(100)

        pygame.mixer.music.unload()

    except Exception as exc:
        print(f"[TTS ERROR] OpenAI TTS: {exc}")

    finally:
        if temp_filename and os.path.exists(temp_filename):
            try:
                os.remove(temp_filename)
            except Exception as exc:
                print(f"[TTS WARNING] Không thể xóa file tạm: {exc}")


def listen_once():
    with sr.Microphone() as mic:
        print("[ASR] Listening...")
        robot_ear.adjust_for_ambient_noise(mic, duration=0.5)

        try:
            audio = robot_ear.listen(
                mic,
                timeout=LISTEN_TIMEOUT,
                phrase_time_limit=PHRASE_TIME_LIMIT,
            )
        except sr.WaitTimeoutError:
            print("[ASR] No speech detected.")
            return ""

    try:
        text = robot_ear.recognize_google(audio, language="vi-VN")
        return text.strip()

    except sr.UnknownValueError:
        print("[ASR] Could not understand audio.")
        return ""

    except sr.RequestError as exc:
        print(f"[ASR ERROR] Google Speech Recognition: {exc}")
        return ""


def handle_conversation_once():
    set_state("listening", "listening", "listening")

    question = listen_once()

    if not question:
        set_state("arrived", "arrived", "no_speech_detected")
        return

    print(f"[USER] {question}")
    add_message("user", question)

    if is_end_conversation_phrase(question):
        answer = (
            "Cảm ơn bạn đã sử dụng LibBot. "
            "Tôi sẽ quay về trạm để sẵn sàng hỗ trợ người dùng tiếp theo."
        )

        add_message("bot", answer)

        set_state("speaking", "speaking", "speaking_goodbye")
        speak_tts(answer)

        set_state("moving", "returning", "return_requested_by_voice")
        request_robot_return_to_base()

        return

    set_state("thinking", "thinking", "processing_question")

    api_context = get_library_data(question)
    print(f"[DB CONTEXT]\n{api_context}")

    answer = generate_answer(question, api_context)

    print(f"[BOT] {answer}")
    add_message("bot", answer)

    set_state("speaking", "speaking", "speaking_answer")
    speak_tts(answer)

    time.sleep(AFTER_SPEAK_SLEEP_SECONDS)

    set_state("arrived", "arrived", "ready_for_next_question")


def main():
    setup_audio()

    print("[LibBot Chatbot Worker] Started.")
    print("[LibBot Chatbot Worker] Waiting for robot_status=arrived and mode=arrived.")

    while True:
        try:
            if should_chatbot_listen():
                handle_conversation_once()
            else:
                time.sleep(IDLE_SLEEP_SECONDS)

        except KeyboardInterrupt:
            print("[LibBot Chatbot Worker] Stopped.")
            break

        except Exception as exc:
            print(f"[WORKER ERROR] {exc}")

            state = load_bot_state()
            state["mode"] = "error"
            state["robot_status"] = "error"
            state["last_event"] = "chatbot_worker_error"

            append_bot_message(
                state,
                "system",
                f"Lỗi chatbot worker: {exc}",
            )

            save_bot_state(state)
            time.sleep(2)


if __name__ == "__main__":
    main()