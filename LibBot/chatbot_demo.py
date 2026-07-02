import json
import os
import re
import subprocess
import tempfile
import time
from pathlib import Path

import pygame
import requests
import speech_recognition as sr
from dotenv import load_dotenv
from openai import OpenAI


# =====================================================
# ENV CONFIG
# =====================================================
BASE_DIR = Path(__file__).resolve().parent
load_dotenv(BASE_DIR / ".env")


OPENAI_API_KEY = os.getenv("OPENAI_API_KEY", "").strip()

MODEL = os.getenv("OPENAI_MODEL", "gpt-4o-mini")
TTS_MODEL = os.getenv("OPENAI_TTS_MODEL", "tts-1")
TTS_VOICE = os.getenv("OPENAI_TTS_VOICE", "nova")

API_BASE = os.getenv("LIBBOT_API_BASE", "http://127.0.0.1:8000/api")

LISTEN_TIMEOUT = float(os.getenv("LIBBOT_LISTEN_TIMEOUT", "5"))
PHRASE_TIME_LIMIT = float(os.getenv("LIBBOT_PHRASE_TIME_LIMIT", "6"))

AUDIO_CARD = os.getenv("LIBBOT_AUDIO_CARD", "4")
AUDIO_CONTROL = os.getenv("LIBBOT_AUDIO_CONTROL", "PCM")
AUDIO_VOLUME = os.getenv("LIBBOT_AUDIO_VOLUME", "100%")


if not OPENAI_API_KEY:
    print("[OPENAI ERROR] Chưa có OPENAI_API_KEY trong .env.")
    print("[OPENAI ERROR] Demo vẫn khởi động, nhưng LLM/TTS sẽ không hoạt động.")

client = OpenAI(api_key=OPENAI_API_KEY) if OPENAI_API_KEY else None
robot_ear = sr.Recognizer()


# =====================================================
# AUDIO SETUP
# =====================================================
def setup_audio():
    try:
        print(
            f"[AUDIO] Đang cài đặt âm lượng: "
            f"card={AUDIO_CARD}, control={AUDIO_CONTROL}, volume={AUDIO_VOLUME}"
        )

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


# =====================================================
# LIBRARY DATABASE API
# =====================================================
def search_books(query):
    try:
        resp = requests.get(
            f"{API_BASE}/books/search/",
            params={"search": query},
            timeout=3,
        )

        if resp.status_code == 200:
            return resp.json()

        print(f"[API WARNING] search_books status={resp.status_code}")
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


# =====================================================
# INTENT + LLM
# =====================================================
def parse_intent_with_llm(question):
    if not client:
        return {
            "intent": "general",
            "search_query": "",
        }

    system_prompt = """
Bạn là hệ thống trích xuất thông tin thư viện. Luôn trả về JSON hợp lệ.

Phân tích câu hỏi người dùng và trả về:
- intent: chọn một trong ba giá trị: search_book, check_borrowed, general.
- search_query: tên sách, tác giả hoặc thể loại cốt lõi nhất.

Quy tắc:
- Nếu hỏi vị trí sách, tác giả, số lượng, còn hay hết, thể loại, kệ nào: intent = search_book.
- Nếu hỏi sách nào đang được mượn, ai mượn, hạn trả: intent = check_borrowed.
- Nếu chỉ chào hỏi hoặc hỏi chung không liên quan dữ liệu thư viện: intent = general.
- search_query phải loại bỏ từ nhiễu như: sách, giáo trình, cuốn, quyển, tìm, vị trí, ở đâu, kệ nào, ai viết.

Ví dụ:
"cho mình vị trí của giáo trình Vật Lý đại cương" -> {"intent":"search_book","search_query":"Vật Lý đại cương"}
"ai là tác giả cuốn đắc nhân tâm" -> {"intent":"search_book","search_query":"đắc nhân tâm"}
"sách nào đang được mượn" -> {"intent":"check_borrowed","search_query":""}
"""

    try:
        response = client.chat.completions.create(
            model=MODEL,
            messages=[
                {
                    "role": "system",
                    "content": system_prompt,
                },
                {
                    "role": "user",
                    "content": question,
                },
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
                        "Không dùng markdown. "
                        "Vì đây là bản demo, hãy trả lời trong khoảng 1 đến 3 câu."
                    ),
                },
                {
                    "role": "user",
                    "content": f"Câu hỏi: {question}\n\n{api_context}",
                },
            ],
            temperature=0.4,
            max_completion_tokens=160,
        )

        answer = completion.choices[0].message.content
        answer = re.sub(r"\*\*", "", answer)
        return answer.strip()

    except Exception as exc:
        print(f"[LLM ERROR] generate_answer: {exc}")
        return "Xin lỗi, tôi đang gặp lỗi kết nối hệ thống. Bạn vui lòng thử lại sau."


# =====================================================
# ASR / TTS
# =====================================================
def listen_once():
    with sr.Microphone() as mic:
        print("[ASR] Đang nghe...")

        robot_ear.adjust_for_ambient_noise(mic, duration=0.5)

        try:
            audio = robot_ear.listen(
                mic,
                timeout=LISTEN_TIMEOUT,
                phrase_time_limit=PHRASE_TIME_LIMIT,
            )

        except sr.WaitTimeoutError:
            print("[ASR] Không phát hiện giọng nói.")
            return ""

    try:
        text = robot_ear.recognize_google(audio, language="vi-VN")
        return text.strip()

    except sr.UnknownValueError:
        print("[ASR] Không nhận diện được nội dung.")
        return ""

    except sr.RequestError as exc:
        print(f"[ASR ERROR] Google Speech Recognition: {exc}")
        return ""


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


# =====================================================
# DEMO CONTROL
# =====================================================
def is_exit_phrase(text):
    normalized = text.lower().strip()

    exit_phrases = [
        "thoát",
        "dừng demo",
        "kết thúc",
        "kết thúc demo",
        "tạm biệt",
        "bye",
        "bai",
    ]

    return any(phrase in normalized for phrase in exit_phrases)


def handle_demo_once():
    question = listen_once()

    if not question:
        return

    print(f"[USER] {question}")

    if is_exit_phrase(question):
        answer = "Cảm ơn bạn. Bản demo tương tác LibBot xin kết thúc."
        print(f"[BOT] {answer}")
        speak_tts(answer)
        raise KeyboardInterrupt

    print("[DEMO] Đang xử lý câu hỏi...")

    api_context = get_library_data(question)
    print(f"[DB CONTEXT]\n{api_context}")

    answer = generate_answer(question, api_context)

    print(f"[BOT] {answer}")
    speak_tts(answer)

    time.sleep(0.5)


def main():
    setup_audio()

    print("========================================")
    print(" LibBot Chatbot Demo")
    print("========================================")
    print("Chế độ: chỉ demo hỏi - đáp bằng giọng nói.")
    print("Không điều khiển ESP32.")
    print("Không yêu cầu robot phải đến bàn.")
    print("Nói 'kết thúc demo' hoặc 'tạm biệt' để thoát.")
    print("========================================")

    intro = "Xin chào, tôi là LibBot. Bạn có thể hỏi tôi về vị trí sách, tình trạng mượn trả, hoặc thông tin thư viện."
    print(f"[BOT] {intro}")
    speak_tts(intro)

    while True:
        try:
            handle_demo_once()

        except KeyboardInterrupt:
            print("[DEMO] Đã dừng chatbot demo.")
            break

        except Exception as exc:
            print(f"[DEMO ERROR] {exc}")
            time.sleep(1)


if __name__ == "__main__":
    main()