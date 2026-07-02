/*
  LibBot - ESP32 Line Following + Stop Point Test
  HW-871 5 kênh + L298N

  Mục tiêu bản test:
  - Robot tự chạy sau khi cấp nguồn.
  - Không dùng Serial commands.
  - Không dùng Wi-Fi/TCP/GOTO/RETURN.
  - Dò line hình con nhộng.
  - Có 4 điểm dừng bằng vạch line ngang nằm trên đoạn line thẳng.
  - Xe dừng tại mỗi vạch ngang 5 giây rồi tự đi tiếp.

  Quy ước:
  OUT1-OUT2 L298N = Motor phải
  OUT3-OUT4 L298N = Motor trái

  Cảm biến:
  S1 = trái ngoài
  S2 = trái trong
  S3 = giữa
  S4 = phải trong
  S5 = phải ngoài

  Bit sau chuẩn hóa:
  bit0 = S1 / trái ngoài
  bit1 = S2 / trái trong
  bit2 = S3 / giữa
  bit3 = S4 / phải trong
  bit4 = S5 / phải ngoài
  bit = 1 nghĩa là cảm biến thấy line đen

  Thuật toán điểm dừng:
  - Không bắt buộc rawBits phải đúng 11111.
  - Dùng crossScore:
      11111       => score 5
      >=4 + S3    => score 4
      01110       => score 3
      S1+S3+S5    => score 3
      00111/11100 => score 2
  - stableCounterCross tăng khi score cao và giảm chậm khi mất tín hiệu.
*/

#include <Arduino.h>
#include <math.h>

// =====================================================
// LINE SENSOR - ADC1 SAFE PINS
// =====================================================
#define S1 36
#define S2 39
#define S3 34
#define S4 35
#define S5 32

#define BLACK_LINE_IS_LOW true

/*
  false:
  S1 S2 S3 S4 S5
  trái ngoài ... giữa ... phải ngoài

  true:
  đảo thứ tự cảm biến nếu bạn lắp module ngược.
*/
const bool SENSOR_ORDER_REVERSED = false;

// =====================================================
// L298N MOTOR PINS
// =====================================================
#define IN1_R 16
#define IN2_R 17
#define ENA_R 21

#define IN3_L 18
#define IN4_L 19
#define ENB_L 22

// =====================================================
// PWM CONFIG
// =====================================================
#define PWM_FREQ 5000
#define PWM_RES  8
#define PWM_CH_R 0
#define PWM_CH_L 1

// =====================================================
// SPEED CONFIG - BẢN TEST DÒ LINE + ĐIỂM DỪNG
// =====================================================
int BASE_SPEED   = 220;
int MIN_SPEED    = 100;
int MAX_SPEED    = 255;
int SEARCH_SPEED = 170;

// Có tải phía trên: không để bánh trong quá yếu, nếu không xe vào cua sẽ ì/stall.
int HARD_TURN_INNER_SPEED = 60;
int HARD_TURN_OUTER_SPEED = 250;

// Với L298N + xe có tải, hạn chế quay lùi khi cua vì dễ tụt lực.
// Biến này vẫn giữ lại để không phá cấu trúc source.
int PIVOT_REVERSE_SPEED = 100;
int PIVOT_FORWARD_SPEED = 250;

// =====================================================
// STOP POINT CONFIG
// =====================================================
const int TOTAL_STOP_POINTS = 4;

// Dừng 3 giây tại mỗi vạch ngang.
const unsigned long STOP_POINT_WAIT_MS = 3000;

// Bỏ qua vạch start trong vài giây đầu sau khi bật nguồn.
const unsigned long START_IGNORE_MS = 3000;

// Sau khi dừng xong, chạy thẳng một đoạn để vượt khỏi vạch ngang.
const unsigned long STOP_POINT_BYPASS_MS = 800;

// Sau khi phát hiện một vạch ngang, bỏ qua một thời gian để chống đếm trùng.
const unsigned long STOP_POINT_IGNORE_MS = 1000;

// Chống đếm trùng theo khoảng cách thời gian tối thiểu giữa 2 lần phát hiện.
const unsigned long STOP_POINT_MIN_INTERVAL_MS = 1000;

// Tốc độ vượt qua vạch ngang sau khi dừng.
int STOP_POINT_BYPASS_SPEED = 250;

// Khi bắt đầu thấy dấu hiệu vạch ngang, giảm tốc để dễ bắt vạch hơn.
const int CROSS_APPROACH_SPEED = 120;

// Stable mềm.
// Nếu vẫn bỏ sót điểm dừng: giảm về 2.
// Nếu nhận nhầm cua là vạch ngang: tăng lên 4.
const int STOP_POINT_STABLE_NEEDED = 1;

int stableCounterCross = 0;
int stopPointCount = 0;
int currentStopPoint = 0;

unsigned long stopPointIgnoreUntil = 0;
unsigned long stopPointBypassUntil = 0;
unsigned long stopPointWaitUntil = 0;
unsigned long lastStopPointDetectedAt = 0;

enum StopPointEvent {
  STOP_POINT_NONE = 0,
  STOP_POINT_DETECTED = 1,
  STOP_POINT_WAITING = 2,
  STOP_POINT_BYPASS = 3,
  STOP_POINT_CONTINUE = 4
};

StopPointEvent lastStopPointEvent = STOP_POINT_NONE;

// =====================================================
// ROBOT MODE - CHỈ PHỤC VỤ TEST
// =====================================================
enum RobotMode {
  MODE_LINE_FOLLOWING = 0,
  MODE_STOP_WAITING = 1,
  MODE_STOP_BYPASS = 2,
  MODE_LINE_LOST = 3
};

RobotMode robotMode = MODE_LINE_FOLLOWING;

// =====================================================
// PID CONFIG
// =====================================================
float Kp = 22.0;
float Ki = 0.0;
float Kd = 18.0;

float integral = 0.0;
float lastError = 0.0;
float filteredError = 0.0;
float filteredDerivative = 0.0;

float ERROR_FILTER_ALPHA = 0.74;
float D_FILTER_ALPHA = 0.36;

const float MAX_CORRECTION = 76.0;

const float INTEGRAL_LIMIT = 12.0;
const float INTEGRAL_ACTIVE_ZONE = 0.8;

unsigned long lastTime = 0;

// =====================================================
// LOST LINE CONFIG
// =====================================================
unsigned long lineLostTime = 0;
const unsigned long LINE_LOST_STOP_MS = 1000;

int lastSeenSide = 0;
// -1: line lệch trái
//  0: line giữa
// +1: line lệch phải

// =====================================================
// CURVE MEMORY CONFIG
// =====================================================
int turnMemory = 0;
// -1: ưu tiên trái
//  0: không ưu tiên
// +1: ưu tiên phải

unsigned long turnHoldUntil = 0;
const unsigned long TURN_HOLD_MS = 220;

unsigned long pivotHoldUntil = 0;
const unsigned long PIVOT_HOLD_MS = 100;

// =====================================================
// DIGITAL SENSOR FILTER
// =====================================================
int sensorScore[5] = {0, 0, 0, 0, 0};

const int SCORE_MIN = 0;
const int SCORE_MAX = 4;
const int SCORE_THRESHOLD = 1;

bool DEBUG_MODE = false;

// =====================================================
// MOTOR FUNCTIONS
// =====================================================
void setRightMotor(int pwm, bool forward) {
  pwm = constrain(pwm, 0, 255);

  if (pwm == 0) {
    digitalWrite(IN1_R, LOW);
    digitalWrite(IN2_R, LOW);
    ledcWrite(PWM_CH_R, 0);
    return;
  }

  digitalWrite(IN1_R, forward ? HIGH : LOW);
  digitalWrite(IN2_R, forward ? LOW  : HIGH);
  ledcWrite(PWM_CH_R, pwm);
}

void setLeftMotor(int pwm, bool forward) {
  pwm = constrain(pwm, 0, 255);

  if (pwm == 0) {
    digitalWrite(IN3_L, LOW);
    digitalWrite(IN4_L, LOW);
    ledcWrite(PWM_CH_L, 0);
    return;
  }

  digitalWrite(IN3_L, forward ? HIGH : LOW);
  digitalWrite(IN4_L, forward ? LOW  : HIGH);
  ledcWrite(PWM_CH_L, pwm);
}

void stopMotors() {
  // Bật HIGH cả 2 chân + đẩy PWM lên max để khóa từ trường động cơ
  digitalWrite(IN1_R, HIGH);
  digitalWrite(IN2_R, HIGH);
  ledcWrite(PWM_CH_R, 255);

  digitalWrite(IN3_L, HIGH);
  digitalWrite(IN4_L, HIGH);
  ledcWrite(PWM_CH_L, 255);
}

void driveForwardPWM(int leftPWM, int rightPWM) {
  leftPWM  = constrain(leftPWM,  MIN_SPEED, MAX_SPEED);
  rightPWM = constrain(rightPWM, MIN_SPEED, MAX_SPEED);

  setLeftMotor(leftPWM, true);
  setRightMotor(rightPWM, true);
}

void driveForwardFixed(int pwm) {
  pwm = constrain(pwm, MIN_SPEED, MAX_SPEED);
  setLeftMotor(pwm, true);
  setRightMotor(pwm, true);
}

void driveForwardCurvePWM(int leftPWM, int rightPWM) {
  leftPWM  = constrain(leftPWM,  0, MAX_SPEED);
  rightPWM = constrain(rightPWM, 0, MAX_SPEED);

  setLeftMotor(leftPWM, true);
  setRightMotor(rightPWM, true);
}

void pivotTurnLeft() {
  // Lùi bánh trái, tiến bánh phải
  setLeftMotor(PIVOT_REVERSE_SPEED, false); 
  setRightMotor(PIVOT_FORWARD_SPEED, true);
}

void pivotTurnRight() {
  // Tiến bánh trái, lùi bánh phải
  setLeftMotor(PIVOT_FORWARD_SPEED, true);
  setRightMotor(PIVOT_REVERSE_SPEED, false); 
}

// =====================================================
// SENSOR FUNCTIONS
// =====================================================
bool readRawSensor(int pin) {
  int raw = digitalRead(pin);

  if (BLACK_LINE_IS_LOW) {
    return raw == LOW;
  }

  return raw == HIGH;
}

/*
  Chuẩn hóa bits:
  bit 0 = trái ngoài
  bit 1 = trái trong
  bit 2 = giữa
  bit 3 = phải trong
  bit 4 = phải ngoài
*/
uint8_t normalizeBits(uint8_t physicalBits) {
  if (!SENSOR_ORDER_REVERSED) {
    return physicalBits;
  }

  uint8_t normalized = 0;

  if (physicalBits & (1 << 0)) normalized |= (1 << 4);
  if (physicalBits & (1 << 1)) normalized |= (1 << 3);
  if (physicalBits & (1 << 2)) normalized |= (1 << 2);
  if (physicalBits & (1 << 3)) normalized |= (1 << 1);
  if (physicalBits & (1 << 4)) normalized |= (1 << 0);

  return normalized;
}

uint8_t readRawLineBits() {
  uint8_t bits = 0;

  if (readRawSensor(S1)) bits |= (1 << 0);
  if (readRawSensor(S2)) bits |= (1 << 1);
  if (readRawSensor(S3)) bits |= (1 << 2);
  if (readRawSensor(S4)) bits |= (1 << 3);
  if (readRawSensor(S5)) bits |= (1 << 4);

  return normalizeBits(bits);
}

uint8_t readFilteredLineBits() {
  bool raw[5];

  raw[0] = readRawSensor(S1);
  raw[1] = readRawSensor(S2);
  raw[2] = readRawSensor(S3);
  raw[3] = readRawSensor(S4);
  raw[4] = readRawSensor(S5);

  uint8_t physicalBits = 0;

  for (int i = 0; i < 5; i++) {
    if (raw[i]) {
      sensorScore[i]++;
    } else {
      sensorScore[i]--;
    }

    sensorScore[i] = constrain(sensorScore[i], SCORE_MIN, SCORE_MAX);

    if (sensorScore[i] >= SCORE_THRESHOLD) {
      physicalBits |= (1 << i);
    }
  }

  return normalizeBits(physicalBits);
}

void printBits(uint8_t bits) {
  Serial.print((bits & (1 << 0)) ? "1" : "0");
  Serial.print((bits & (1 << 1)) ? "1" : "0");
  Serial.print((bits & (1 << 2)) ? "1" : "0");
  Serial.print((bits & (1 << 3)) ? "1" : "0");
  Serial.print((bits & (1 << 4)) ? "1" : "0");
}

int countActiveLineSensors(uint8_t bits) {
  int count = 0;

  for (int i = 0; i < 5; i++) {
    if (bits & (1 << i)) {
      count++;
    }
  }

  return count;
}

// =====================================================
// STOP POINT FUNCTIONS - CROSS SCORE
// =====================================================
int getCrossLineScore(uint8_t rawBits) {
  bool s1 = rawBits & (1 << 0);  // trái ngoài
  bool s5 = rawBits & (1 << 4);  // phải ngoài

  // Tín hiệu mạnh nhất: Quét qua cả 2 biên cùng lúc -> Chắc chắn là vạch ngang
  if (s1 && s5) {
    return 5;
  }

  // Tín hiệu mạnh: Cả 5 mắt đều đen
  if (rawBits == 0b11111) {
    return 5;
  }

  // Tín hiệu trung bình: 3 mắt giữa đen. 
  // Rất hiếm khi vào cua mà bị tình trạng này, thường là xe đi ngang qua vạch hẹp
  if (rawBits == 0b01110) {
    return 3;
  }

  // Các trường hợp khác (01111, 11110, 00111, 11100...) được tính là CUA, trả về 0 để bỏ qua.
  return 0; 
}

bool detectCrossLine(uint8_t rawBits) {
  return getCrossLineScore(rawBits) >= 3;
}

StopPointEvent checkStopPoint(uint8_t rawBits) {
  unsigned long now = millis();

  if (now < stopPointIgnoreUntil) {
    stableCounterCross = 0;
    return STOP_POINT_NONE;
  }

  if (now - lastStopPointDetectedAt < STOP_POINT_MIN_INTERVAL_MS) {
    stableCounterCross = 0;
    return STOP_POINT_NONE;
  }

  int crossScore = getCrossLineScore(rawBits);

  if (crossScore >= 4) {
    // Vạch ngang rất rõ: tăng nhanh.
    stableCounterCross += 2;
  } else if (crossScore >= 3) {
    // Vạch ngang tương đối rõ.
    stableCounterCross += 1;
  } else {
    // Không reset tức thì, giảm chậm để chống mất một mẫu do nhiễu.
    stableCounterCross--;
  }

  stableCounterCross = constrain(stableCounterCross, 0, STOP_POINT_STABLE_NEEDED + 2);

  if (stableCounterCross >= STOP_POINT_STABLE_NEEDED) {
    stableCounterCross = 0;

    stopPointCount++;

    currentStopPoint++;
    if (currentStopPoint > TOTAL_STOP_POINTS) {
      currentStopPoint = 1;
    }

    lastStopPointDetectedAt = now;
    lastStopPointEvent = STOP_POINT_DETECTED;

    return STOP_POINT_DETECTED;
  }

  return STOP_POINT_NONE;
}

void resetPIDAfterStopPoint() {
  integral = 0.0;
  lastError = 0.0;
  filteredError = 0.0;
  filteredDerivative = 0.0;
  lastTime = millis();

  lineLostTime = 0;
  turnMemory = 0;
  turnHoldUntil = 0;
  pivotHoldUntil = 0;

  for (int i = 0; i < 5; i++) {
    sensorScore[i] = 0;
  }
}

void startStopPointWaiting() {
  stopMotors();

  robotMode = MODE_STOP_WAITING;
  stopPointWaitUntil = millis() + STOP_POINT_WAIT_MS;

  lastStopPointEvent = STOP_POINT_WAITING;

  resetPIDAfterStopPoint();

  if (DEBUG_MODE) {
    Serial.print("[STOP] count=");
    Serial.print(stopPointCount);
    Serial.print(" current=");
    Serial.println(currentStopPoint);
  }
}

void updateStopPointWaiting() {
  unsigned long now = millis();

  stopMotors();

  if (now >= stopPointWaitUntil) {
    robotMode = MODE_STOP_BYPASS;

    stopPointBypassUntil = now + STOP_POINT_BYPASS_MS;
    stopPointIgnoreUntil = now + STOP_POINT_IGNORE_MS;

    lastStopPointEvent = STOP_POINT_CONTINUE;

    resetPIDAfterStopPoint();

    if (DEBUG_MODE) {
      Serial.print("[CONTINUE] current=");
      Serial.println(currentStopPoint);
    }
  }
}

void updateStopPointBypass() {
  unsigned long now = millis();

  if (now < stopPointBypassUntil) {
    driveForwardFixed(STOP_POINT_BYPASS_SPEED);

    if (DEBUG_MODE) {
      uint8_t rawBits = readRawLineBits();
      Serial.print("[BYPASS] raw=");
      printBits(rawBits);
      Serial.print(" current=");
      Serial.println(currentStopPoint);
    }

    return;
  }

  robotMode = MODE_LINE_FOLLOWING;
  resetPIDAfterStopPoint();
}

// =====================================================
// LINE SIDE / TURN DETECTION
// =====================================================
void updateLastSeenSide(uint8_t bits) {
  bool leftSeen  = bits & ((1 << 0) | (1 << 1));
  bool midSeen   = bits & (1 << 2);
  bool rightSeen = bits & ((1 << 3) | (1 << 4));

  if (leftSeen && !rightSeen) {
    lastSeenSide = -1;
  } else if (rightSeen && !leftSeen) {
    lastSeenSide = 1;
  } else if (midSeen && !leftSeen && !rightSeen) {
    lastSeenSide = 0;
  }
}

/*
  Giữ đúng logic từ file bạn gửi:
  - Với hệ đang normalize như file gốc, các hàm trái/phải đang được định nghĩa như dưới.
  - Nếu xe cua ngược thực tế, đổi SENSOR_ORDER_REVERSED hoặc đảo lại nhóm hàm này.
*/
bool isHardLeft(uint8_t bits) {
  return (bits == 0b00001 || bits == 0b00011 || bits == 0b00111);
}

bool isHardRight(uint8_t bits) {
  return (bits == 0b10000 || bits == 0b11000 || bits == 0b11100);
}

bool isExtremeLeft(uint8_t bits) {
  return (bits == 0b00001 || bits == 0b00011);
}

bool isExtremeRight(uint8_t bits) {
  return (bits == 0b10000 || bits == 0b11000);
}

bool isPrepareLeft(uint8_t bits) {
  return (bits == 0b00110 || bits == 0b00111 || bits == 0b01111 || bits == 0b00010);
}

bool isPrepareRight(uint8_t bits) {
  return (bits == 0b01100 || bits == 0b11100 || bits == 0b11110 || bits == 0b01000);
}

bool isCenterStable(uint8_t bits) {
  return (bits == 0b00100 || bits == 0b01110);
}

// =====================================================
// COMPUTE LINE ERROR
// =====================================================
bool computeLineError(float &errorOut, uint8_t &bitsOut, uint8_t rawBits) {
  uint8_t bits = readFilteredLineBits();
  bitsOut = bits;

  if (bits == 0b00000 && rawBits != 0b00000) {
    bits = rawBits;
    bitsOut = rawBits;
  }

  if (bits == 0b00000) {
    return false;
  }

  updateLastSeenSide(bits);

  if (isHardLeft(rawBits)) {
    turnMemory = -1;
    turnHoldUntil = millis() + TURN_HOLD_MS;
    lastSeenSide = -1;
  } else if (isHardRight(rawBits)) {
    turnMemory = 1;
    turnHoldUntil = millis() + TURN_HOLD_MS;
    lastSeenSide = 1;
  } else if (isPrepareLeft(rawBits)) {
    turnMemory = -1;
    turnHoldUntil = millis() + (TURN_HOLD_MS / 2);
    lastSeenSide = -1;
  } else if (isPrepareRight(rawBits)) {
    turnMemory = 1;
    turnHoldUntil = millis() + (TURN_HOLD_MS / 2);
    lastSeenSide = 1;
  }

  if (isExtremeLeft(rawBits)) {
    pivotHoldUntil = millis() + PIVOT_HOLD_MS;
  } else if (isExtremeRight(rawBits)) {
    pivotHoldUntil = millis() + PIVOT_HOLD_MS;
  }

  switch (bits) {
    case 0b00100:
      errorOut = 0.0;
      return true;

    case 0b01110:
      errorOut = 0.0;
      return true;

    case 0b00110:
      errorOut = -3.50; // Tăng từ -2.00 lên -3.50
      return true;

    case 0b01100:
      errorOut = 3.50;  // Tăng từ 2.00 lên 3.50
      return true;

    case 0b00010:
      errorOut = -3.20;
      return true;

    case 0b01000:
      errorOut = 3.20;
      return true;

    case 0b00011:
      errorOut = -4.40;
      return true;

    case 0b11000:
      errorOut = 4.40;
      return true;

    case 0b00001:
      errorOut = -6.00;
      return true;

    case 0b10000:
      errorOut = 6.00;
      return true;

    case 0b00111:
      errorOut = -4.80;
      return true;

    case 0b11100:
      errorOut = 4.80;
      return true;

    case 0b01111:
      errorOut = -3.80;
      return true;

    case 0b11110:
      errorOut = 3.80;
      return true;

    case 0b11111:
      if (turnMemory < 0 && millis() < turnHoldUntil) {
        errorOut = -2.5;
      } else if (turnMemory > 0 && millis() < turnHoldUntil) {
        errorOut = 2.5;
      } else {
        errorOut = 0.0;
      }
      return true;

    default:
      break;
  }

  int weights[5] = {-6, -2, 0, 2, 6};
  int sumWeight = 0;
  int count = 0;

  for (int i = 0; i < 5; i++) {
    if (bits & (1 << i)) {
      sumWeight += weights[i];
      count++;
    }
  }

  if (count == 0) {
    return false;
  }

  errorOut = (float)sumWeight / (float)count;
  return true;
}

// =====================================================
// SEARCH LINE FUNCTION
// =====================================================
void searchLine() {
  if (turnMemory < 0 || lastSeenSide < 0) {
    pivotTurnLeft();
  } else if (turnMemory > 0 || lastSeenSide > 0) {
    pivotTurnRight();
  } else {
    pivotTurnRight();
  }
}

// =====================================================
// LINE FOLLOWING PID
// =====================================================
void lineFollowStep() {
  float rawError = 0.0;
  uint8_t bits = 0;

  uint8_t rawBits = readRawLineBits();
  unsigned long now = millis();

  // Đang dừng 5 giây tại điểm dừng.
  if (robotMode == MODE_STOP_WAITING) {
    updateStopPointWaiting();
    return;
  }

  // Đang vượt qua vạch ngang sau khi dừng.
  if (robotMode == MODE_STOP_BYPASS) {
    updateStopPointBypass();
    return;
  }

  /*
    Nếu đang trong giai đoạn bypass cuối:
    - Giai đoạn đầu của bypass đã return trong MODE_STOP_BYPASS.
    - Khi hết bypass, PID chạy lại bình thường.
  */

  /*
    Giảm tốc nhẹ khi bắt đầu thấy dấu hiệu vạch ngang.
    Không return, vẫn tiếp tục kiểm tra stable counter.
  */
  int crossScoreNow = getCrossLineScore(rawBits);

  if (
    crossScoreNow >= 3 &&
    now >= stopPointIgnoreUntil &&
    (now - lastStopPointDetectedAt >= STOP_POINT_MIN_INTERVAL_MS)
  ) {
    driveForwardFixed(CROSS_APPROACH_SPEED);
  }

  StopPointEvent stopEvent = checkStopPoint(rawBits);

  if (stopEvent == STOP_POINT_DETECTED) {
    startStopPointWaiting();
    return;
  }

  bool hasLine = computeLineError(rawError, bits, rawBits);

  if (!hasLine) {
    robotMode = MODE_LINE_LOST;

    if (lineLostTime == 0) {
      lineLostTime = now;
    }

    if (now - lineLostTime < LINE_LOST_STOP_MS) {
      searchLine();
    } else {
      stopMotors();
    }

    if (DEBUG_MODE) {
      Serial.print("[LOST] raw=");
      printBits(rawBits);
      Serial.print(" filt=");
      printBits(bits);
      Serial.println();
    }

    return;
  }

  robotMode = MODE_LINE_FOLLOWING;
  lineLostTime = 0;

  float dt = (now - lastTime) / 1000.0;

  if (dt <= 0.0 || dt > 0.2) {
    dt = 0.005;
  }

  lastTime = now;

  filteredError = ERROR_FILTER_ALPHA * rawError + (1.0 - ERROR_FILTER_ALPHA) * filteredError;

  if (fabs(filteredError) < INTEGRAL_ACTIVE_ZONE) {
    integral += filteredError * dt;
    integral = constrain(integral, -INTEGRAL_LIMIT, INTEGRAL_LIMIT);
  } else {
    integral = 0.0;
  }

  float rawDerivative = (filteredError - lastError) / dt;

  filteredDerivative = D_FILTER_ALPHA * rawDerivative + (1.0 - D_FILTER_ALPHA) * filteredDerivative;
  lastError = filteredError;

  float correction = Kp * filteredError + Ki * integral + Kd * filteredDerivative;
  correction = constrain(correction, -MAX_CORRECTION, MAX_CORRECTION);

  int dynamicBaseSpeed = BASE_SPEED;
  float absError = fabs(filteredError);

  if (absError >= 4.0) {
    dynamicBaseSpeed = BASE_SPEED - 40;
  } else if (absError >= 2.5) {
    dynamicBaseSpeed = BASE_SPEED - 30;
  }

  dynamicBaseSpeed = constrain(dynamicBaseSpeed, MIN_SPEED, BASE_SPEED);

  int leftSpeed  = dynamicBaseSpeed + correction;
  int rightSpeed = dynamicBaseSpeed - correction;

  bool pivotActive = now < pivotHoldUntil;

  if ((isExtremeLeft(rawBits) || isExtremeLeft(bits)) && pivotActive) {
    pivotTurnLeft();
  } else if ((isExtremeRight(rawBits) || isExtremeRight(bits)) && pivotActive) {
    pivotTurnRight();
  }

  else if (isHardLeft(rawBits) || isHardLeft(bits) || (now < turnHoldUntil && turnMemory < 0)) {
    leftSpeed  = HARD_TURN_INNER_SPEED;
    rightSpeed = HARD_TURN_OUTER_SPEED;
    driveForwardCurvePWM(leftSpeed, rightSpeed);
  } else if (isHardRight(rawBits) || isHardRight(bits) || (now < turnHoldUntil && turnMemory > 0)) {
    leftSpeed  = HARD_TURN_OUTER_SPEED;
    rightSpeed = HARD_TURN_INNER_SPEED;
    driveForwardCurvePWM(leftSpeed, rightSpeed);
  }

  else if (isPrepareLeft(rawBits) || isPrepareLeft(bits)) {
    // Ép mạnh: hãm nhiều bánh trái, tăng nhiều bánh phải
    leftSpeed  = BASE_SPEED - 60; // Cũ là 35
    rightSpeed = BASE_SPEED + 60; // Cũ là 45

    leftSpeed  = constrain(leftSpeed,  MIN_SPEED, MAX_SPEED);
    rightSpeed = constrain(rightSpeed, MIN_SPEED, MAX_SPEED);

    driveForwardCurvePWM(leftSpeed, rightSpeed);
  } else if (isPrepareRight(rawBits) || isPrepareRight(bits)) {
    leftSpeed  = BASE_SPEED + 60; // Cũ là 45
    rightSpeed = BASE_SPEED - 60; // Cũ là 35

    leftSpeed  = constrain(leftSpeed,  MIN_SPEED, MAX_SPEED);
    rightSpeed = constrain(rightSpeed, MIN_SPEED, MAX_SPEED);

    driveForwardCurvePWM(leftSpeed, rightSpeed);
  }

  else {
    leftSpeed  = constrain(leftSpeed,  MIN_SPEED, MAX_SPEED);
    rightSpeed = constrain(rightSpeed, MIN_SPEED, MAX_SPEED);

    driveForwardCurvePWM(leftSpeed, rightSpeed);
  }

  if (isCenterStable(bits) && now > turnHoldUntil && now > pivotHoldUntil) {
    turnMemory = 0;
  }

  if (DEBUG_MODE) {
    Serial.print("mode="); Serial.print((int)robotMode);
    Serial.print(" raw="); printBits(rawBits);
    Serial.print(" filt="); printBits(bits);
    Serial.print(" cross="); Serial.print(crossScoreNow);
    Serial.print(" stable="); Serial.print(stableCounterCross);
    Serial.print(" stopCount="); Serial.print(stopPointCount);
    Serial.print(" current="); Serial.print(currentStopPoint);
    Serial.print(" event="); Serial.print((int)lastStopPointEvent);
    Serial.print(" err="); Serial.print(rawError, 2);
    Serial.print(" filtErr="); Serial.print(filteredError, 2);
    Serial.print(" corr="); Serial.print(correction, 2);
    Serial.print(" L="); Serial.print(leftSpeed);
    Serial.print(" R="); Serial.print(rightSpeed);
    Serial.print(" side="); Serial.print(lastSeenSide);
    Serial.print(" turnMem="); Serial.print(turnMemory);
    Serial.print(" pivot="); Serial.println(pivotActive ? 1 : 0);
  }
}

// =====================================================
// SETUP
// =====================================================
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(S1, INPUT);
  pinMode(S2, INPUT);
  pinMode(S3, INPUT);
  pinMode(S4, INPUT);
  pinMode(S5, INPUT);

  pinMode(IN1_R, OUTPUT);
  pinMode(IN2_R, OUTPUT);
  pinMode(IN3_L, OUTPUT);
  pinMode(IN4_L, OUTPUT);

  ledcSetup(PWM_CH_R, PWM_FREQ, PWM_RES);
  ledcSetup(PWM_CH_L, PWM_FREQ, PWM_RES);

  ledcAttachPin(ENA_R, PWM_CH_R);
  ledcAttachPin(ENB_L, PWM_CH_L);

  stopMotors();

  resetPIDAfterStopPoint();

  stableCounterCross = 0;
  stopPointCount = 0;
  currentStopPoint = 0;

  stopPointBypassUntil = 0;
  stopPointWaitUntil = 0;
  lastStopPointDetectedAt = 0;

  lastStopPointEvent = STOP_POINT_NONE;
  robotMode = MODE_LINE_FOLLOWING;

  // Bỏ qua vạch start trong vài giây đầu.
  stopPointIgnoreUntil = millis() + START_IGNORE_MS;

  if (DEBUG_MODE) {
    Serial.println("[LIBBOT] Line Following + 4 Stop Points Test Ready");
  }

  delay(1000);
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  lineFollowStep();
  delay(1);
}