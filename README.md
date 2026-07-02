# LibBot — Mobile Library Consulting Robot with Voice Interaction

> Đồ án Tốt nghiệp · HCM-UTE · Khoa Điện - Điện Tử · 2026  
> Ngành: Công Nghệ Kỹ Thuật Điện Tử - Viễn Thông  
> GVHD: TS. Trương Quang Phúc

**LibBot** là robot di động tự hành có khả năng tương tác giọng nói tiếng Việt, được thiết kế để tự động hóa quy trình tra cứu và hỗ trợ tìm kiếm thông tin sách tại thư viện trường học. Người dùng quét mã QR tại bàn để gọi robot; robot tự di chuyển đến nơi và trả lời câu hỏi về sách bằng giọng nói, dựa trên dữ liệu thực của thư viện.

---

## Tính năng nổi bật

- **Tương tác giọng nói hai chiều** — nhận diện tiếng Việt (Google ASR), xử lý bằng LLM (GPT-4o-mini), phát âm thanh (OpenAI TTS)
- **RAG (Retrieval-Augmented Generation)** — AI tra cứu database thực của thư viện trước khi trả lời, không bịa thông tin
- **Bám vạch PID** — thuật toán PID với bộ lọc EMA, điều khiển 5 cảm biến IR, tự dò đường trên sàn
- **Nhận diện điểm dừng V6 Cross-Score** — phát hiện vạch ngang với stable counter mềm, chống nhiễu
- **Giao diện Web QR** — mỗi bàn có trang web riêng, gọi/dừng robot bằng một nút bấm
- **Giao diện mặt robot Canvas** — animation real-time phản ánh trạng thái idle/listening/thinking/speaking
- **Kiến trúc phân tán** — ESP32 xử lý phần cứng thời gian thực, Raspberry Pi xử lý AI và web server

---

## Kiến trúc hệ thống

```
┌─────────────────────────────────────────────────────┐
│                 Raspberry Pi 4                      │
│                                                     │
│  ┌──────────────┐  ┌───────────────┐  ┌─────────┐   │
│  │ Django REST  │  │robot_status   │  │chatbot  │   │
│  │ API (8000)   │  │_worker.py     │  │_worker  │   │
│  └──────┬───────┘  └──────┬────────┘  └────┬────┘   │
│         │                 │                │        │
│         └─────────────────▼────────────────▼──────  │
│                    bot_state.json (shared state)    │
│                    SQLite (books, borrow, tasks)    │
└────────────────────────┬────────────────────────────┘
                         │ TCP Socket · LAN · port 5000
┌────────────────────────▼────────────────────────────┐
│                    ESP32-WROOM-32                   │
│                                                     │
│   PID Line Following · Cross-Score Stop Detection   │
│   L298N Motor Driver · HW-871 IR Sensor Array       │
│   TCP Server · Commands: GOTO/RETURN/STOP/STATUS    │
└─────────────────────────────────────────────────────┘
```

### Luồng tương tác người dùng

```
[Người dùng quét QR bàn N]
        ↓
[Web table_call → POST /api/robot/call/]
        ↓
[Django → TCP GOTO:N → ESP32]
        ↓
[ESP32 bám vạch PID → dừng tại bàn N]
        ↓
[robot_status_worker phát hiện WAITING_AT_TABLE]
        ↓
[chatbot_worker kích hoạt: ASR → Intent → DB → LLM → TTS]
        ↓
[Người dùng nói "cảm ơn" → Robot tự quay về trạm]
```

---

## Cấu trúc thư mục

```
libapi/
├── books/                      # Django app chính
│   ├── models.py               # Book, Location, BorrowRecord, RobotTaskLog
│   ├── views.py                # API endpoints + update_state_from_esp_status()
│   ├── serializers.py
│   ├── urls.py
│   └── admin.py
├── robotcore/
│   ├── robot_controller.py     # Giao tiếp TCP với ESP32
│   └── state.py                # Load/save bot_state.json (atomic write)
├── static/
│   ├── bot_state.json          # Shared state giữa các worker
│   └── table_call.js
├── templates/
│   ├── robot_display.html      # Giao diện mặt robot (Canvas animation)
│   └── table_call.html         # Trang web gọi robot theo bàn
├── robot_status_worker.py      # Worker: poll ESP32, quản lý timeout
├── chatbot_worker.py           # Worker: ASR → LLM → TTS pipeline
├── chatbot_demo.py             # Demo standalone (không cần robot)
└── manage.py

esp32/
└── main.cpp                    # Firmware ESP32 (C++, PlatformIO)

```

---

## Phần cứng

| Thành phần | Model | Vai trò |
|---|---|---|
| Máy tính nhúng | Raspberry Pi 4 Model B | Não trung tâm: AI, web server, DB |
| Vi điều khiển | ESP32-WROOM-32 | Điều khiển phần cứng thời gian thực |
| Cảm biến dò line | HW-871 5 kênh IR | Bám vạch S1–S5 |
| Driver động cơ | L298N | Điều khiển 2 động cơ DC |
| Động cơ | DC giảm tốc 2 trục | Truyền động 4 bánh trượt |
| Microphone | USB Microphone | Thu âm giọng nói |
| Loa | USB Mini Stereo | Phát âm TTS |
| Màn hình | LCD 7" cảm ứng | Hiển thị mặt robot |

### Sơ đồ kết nối ESP32

| ESP32 Pin | Kết nối |
|---|---|
| GPIO 36 (S1) | Cảm biến trái ngoài |
| GPIO 39 (S2) | Cảm biến trái trong |
| GPIO 34 (S3) | Cảm biến giữa |
| GPIO 35 (S4) | Cảm biến phải trong |
| GPIO 32 (S5) | Cảm biến phải ngoài |
| GPIO 16/17/21 | Motor phải (IN1, IN2, ENA) |
| GPIO 18/19/22 | Motor trái (IN3, IN4, ENB) |

---

## Cài đặt & Chạy

### Yêu cầu

- Raspberry Pi 4 với Raspberry Pi OS (Python 3.10+)
- ESP32 với PlatformIO
- Python packages: `django`, `djangorestframework`, `openai`, `SpeechRecognition`, `pygame`, `python-dotenv`, `requests`

### 1. Clone repository

```bash
git clone https://github.com/<your-username>/libbot.git
cd libbot
```

### 2. Cài đặt Python dependencies

```bash
python -m venv venv
source venv/bin/activate
pip install -r requirements.txt
```

### 3. Cấu hình `.env`

```env
# OpenAI
OPENAI_API_KEY=sk-...
OPENAI_MODEL=gpt-4o-mini
OPENAI_TTS_MODEL=tts-1
OPENAI_TTS_VOICE=nova

# ESP32
LIBBOT_ESP_IP=172.20.10.3
LIBBOT_ESP_PORT=5000
LIBBOT_ESP_TIMEOUT=3.0
LIBBOT_SIMULATE_ROBOT=0       # Set 1 để chạy không cần phần cứng thật

# Timeouts
LIBBOT_ARRIVAL_TIMEOUT=90
LIBBOT_RETURN_TIMEOUT=90
LIBBOT_POLL_INTERVAL=1.0

# Audio (Raspberry Pi)
LIBBOT_AUDIO_CARD=4
LIBBOT_AUDIO_CONTROL=PCM
LIBBOT_AUDIO_VOLUME=100%
```

### 4. Khởi tạo database

```bash
python manage.py migrate
python manage.py createsuperuser
```

### 5. Nạp firmware ESP32

```bash
cd esp32/
pio run --target upload
```

Cập nhật `WIFI_SSID` và `WIFI_PASS` trong `main.cpp` trước khi nạp.

### 6. Chạy hệ thống

Mở 3 terminal riêng biệt:

```bash
# Terminal 1: Django web server
python manage.py runserver 0.0.0.0:8000

# Terminal 2: Robot status worker (poll ESP32)
python robot_status_worker.py

# Terminal 3: Chatbot AI worker
python chatbot_worker.py
```

Truy cập trang gọi robot: `http://<raspberry-pi-ip>:8000/table/1/`

### 7. Chạy demo (không cần robot)

```bash
python chatbot_demo.py
```

---

## TCP Protocol (Raspberry Pi ↔ ESP32)

| Lệnh | Phản hồi | Mô tả |
|---|---|---|
| `PING` | `PONG` | Kiểm tra kết nối |
| `STATUS` | `STATUS:MODE,...` | Lấy trạng thái chi tiết |
| `GOTO:N` | `OK:GOTO:N` | Di chuyển đến bàn N (1–4) |
| `RETURN` | `OK:RETURN` | Quay về trạm |
| `STOP` | `OK:STOP` | Dừng khẩn cấp |

**Ví dụ STATUS response:**
```
STATUS:WAITING_AT_TABLE,target=2,current=2,count=2,active=0,crossStable=0,last=ARRIVED:2,ip=172.20.10.3
```

---

## Pipeline AI Chatbot

```
[Giọng nói tiếng Việt]
        ↓
Google Speech Recognition (ASR)
        ↓
GPT-4o-mini → Intent extraction
{intent: "search_book", search_query: "Vật Lý đại cương"}
        ↓
Django API → SQLite query
→ Book title, author, category, availability, shelf location
        ↓
GPT-4o-mini → Generate answer (dựa trên DB context, max 160 tokens)
        ↓
OpenAI TTS (voice: nova) → MP3
        ↓
pygame → Phát loa
```

**3 intent được hỗ trợ:**
- `search_book` — tìm sách, vị trí kệ, tình trạng còn/hết
- `check_borrowed` — sách đang được mượn bởi ai, hạn trả khi nào
- `general` — chào hỏi, câu hỏi chung

**Câu kết thúc hội thoại** (robot tự quay về trạm):
> "xong rồi", "cảm ơn", "tạm biệt", "không cần nữa", "bye", ...

---

## Trạng thái hệ thống (`bot_state.json`)

| `robot_status` | Ý nghĩa |
|---|---|
| `idle` | Đang đứng ở trạm, chờ lệnh |
| `called` | Nhận lệnh gọi, chuẩn bị di chuyển |
| `moving` | Đang di chuyển đến bàn |
| `arrived` | Đã đến bàn, sẵn sàng tương tác |
| `listening` | Đang nghe giọng nói người dùng |
| `thinking` | Đang xử lý câu hỏi (LLM) |
| `speaking` | Đang phát âm thanh trả lời |
| `returning` | Đang quay về trạm |
| `stopped` | Đã dừng khẩn cấp |
| `error` | Gặp lỗi kết nối hoặc timeout |

---

## Giao diện mặt robot

File `robot_display.html` render giao diện Canvas animation với 4 trạng thái biểu cảm:

| Trạng thái | Màu sắc | Hiệu ứng |
|---|---|---|
| **Idle** | Xám đen | Mắt chớp chậm, nhìn ngang tự nhiên |
| **Listening** | Xanh dương | Mắt to hơn, ring pulse quanh mặt |
| **Thinking** | Xanh cyan | Mắt nhìn ngang nhanh, orbit animation |
| **Speaking** | Xanh lá | Thanh sóng âm thanh bên dưới mắt |

---

## Thuật toán PID bám vạch

Bộ điều khiển PID tính toán correction dựa trên sai số vị trí từ mảng cảm biến 5 kênh:

```
u(t) = Kp·e(t) + Ki·∫e(t)dt + Kd·de(t)/dt
```

| Tham số | Giá trị |
|---|---|
| Kp | 20.0 |
| Ki | 0.0 |
| Kd | 12.5 |
| Error filter α | 0.74 |
| Derivative filter α | 0.36 |
| Max correction | ±76 |

**Cross-Score nhận diện vạch dừng:**

| Pattern cảm biến | Score |
|---|---|
| `11111` | 5 — Chắc chắn vạch ngang |
| `≥4 bit + S3` | 4 — Rất có khả năng |
| `01110` hoặc `S1+S3+S5` | 3 — Có khả năng |
| `00111` hoặc `11100` | 2 — Chưa đủ chắc |

Stable counter tăng +2 (score≥4) hoặc +1 (score=3) và giảm -1 nếu không thấy vạch. Khi đạt ngưỡng `STABLE_NEEDED=3` thì xác nhận điểm dừng.

---

## Kết quả thực nghiệm

| Chỉ tiêu | Kết quả |
|---|---|
| Tỷ lệ nhận diện giọng nói tiếng Việt | > 95% |
| Thời gian phản hồi end-to-end | 1.5 – 2.5 giây |
| Bám vạch | Mượt mà, sai số vị trí nhỏ |
| Độ trễ truyền lệnh TCP | Thấp (< 100ms trên LAN) |
| Độ ổn định hệ thống | Rất cao (kiến trúc phân tán) |

---

## Giới hạn hiện tại

- **Không có SLAM/tránh vật cản** — robot chỉ bám vạch, không xử lý được vật cản động
- **Phụ thuộc Internet** — ASR, LLM, TTS đều dùng Cloud API (Google, OpenAI)
- **Hỗ trợ tối đa 4 bàn** — giới hạn bởi số stop point trên đường line hiện tại
- **Quy mô nguyên mẫu** — chưa scale lên môi trường thư viện thực tế lớn

---

## Hướng phát triển

- Tích hợp SLAM + LiDAR để thoát khỏi giới hạn vạch quang học
- Xây dựng ASR/TTS offline để không phụ thuộc Internet
- Mở rộng số bàn và đa tuyến đường
- Tích hợp hệ thống quản lý mượn/trả sách tự động
- Thêm màn hình hiển thị thông tin sách dạng trực quan

---

## Tài liệu liên quan

- [Báo cáo đồ án tốt nghiệp (PDF)](./THIẾT_KẾ_ROBOT_TƯ_VẤN_THƯ_VIỆN_DI_ĐỘNG_TƯƠNG_TÁC_GIỌNG_NÓI.pdf)
- [PlatformIO Documentation](https://docs.platformio.org/)
- [OpenAI API Reference](https://platform.openai.com/docs/)
- [Django REST Framework](https://www.django-rest-framework.org/)

---

## Nhóm thực hiện

| Họ tên | MSSV |
|---|---|
| Nguyễn Nhất Phong | 22161166 |
| Nguyễn Hoàng Phát | 22161163 |

**Trường:** Đại học Công Nghệ Kỹ Thuật TP. Hồ Chí Minh (HCM-UTE)  
**Khoa:** Điện - Điện Tử  
**GVHD:** TS. Trương Quang Phúc  
**Năm:** 2026

---

## 📄 License

Dự án này được thực hiện phục vụ mục đích học thuật trong khuôn khổ Đồ án Tốt nghiệp tại HCM-UTE.
