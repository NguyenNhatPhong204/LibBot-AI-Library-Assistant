/*
  LibBot - ESP32 Line Following V6 TCP GOTO + RETURN + Raspberry Task
  Improved Stop Point Detection with Cross Score + Soft Stable Counter
  Added: GOTO Boost Speed

  Hardware:
  - ESP32
  - HW-871 5-channel line sensor
  - L298N motor driver

  TCP commands:
    PING
    STATUS
    STOP
    GOTO:1
    GOTO:2
    GOTO:3
    GOTO:4
    RETURN
    CONTINUE

  Sensor convention:
  - S1 = left outer
  - S2 = left inner
  - S3 = center
  - S4 = right inner
  - S5 = right outer

  Bit convention after normalization:
  - bit 0 = S1 / left outer
  - bit 1 = S2 / left inner
  - bit 2 = S3 / center
  - bit 3 = S4 / right inner
  - bit 4 = S5 / right outer
  - bit = 1 means sensor sees black line

  Stop point logic:
  - Does not require rawBits == 11111 absolutely.
  - Uses crossScore:
      11111       => score 5
      >=4 + S3    => score 4
      01110       => score 3
      S1+S3+S5    => score 3
      00111/11100 => score 2
  - Stable counter increases when score is high and decreases slowly when signal drops.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <math.h>

// =====================================================
// WIFI CONFIG
// =====================================================
const char* WIFI_SSID = "Phong";
const char* WIFI_PASS = "11111111";

const uint16_t TCP_PORT = 5000;
WiFiServer server(TCP_PORT);

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
  left ... center ... right

  true:
  S5 S4 S3 S2 S1
  left ... center ... right
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
// SPEED CONFIG - FULL FRAME SAFE PROFILE
// =====================================================
int BASE_SPEED   = 195;
int MIN_SPEED    = 155;
int MAX_SPEED    = 245;
int SEARCH_SPEED = 170;

int HARD_TURN_INNER_SPEED = 95;
int HARD_TURN_OUTER_SPEED = 250;

int PIVOT_REVERSE_SPEED = 45;
int PIVOT_FORWARD_SPEED = 250;

// =====================================================
// ROBOT TASK MODE
// =====================================================
enum RobotMode {
  MODE_IDLE_AT_BASE = 0,
  MODE_GOING_TO_TABLE = 1,
  MODE_WAITING_AT_TABLE = 2,
  MODE_RETURNING_TO_BASE = 3,
  MODE_STOPPED = 4,
  MODE_ERROR = 5
};

RobotMode robotMode = MODE_IDLE_AT_BASE;

int targetStopPoint = 0;
bool hasActiveTask = false;

String lastCommand = "";
String lastEvent = "BOOT";

// =====================================================
// BOOST CONFIG
// =====================================================
unsigned long gotoStartTime = 0;
const unsigned long GOTO_BOOST_MS = 700;
const int GOTO_BOOST_SPEED = 220;

// =====================================================
// STOP POINT CONFIG - V6
// =====================================================
const int TOTAL_STOP_POINTS = 4;

/*
  Bỏ qua đoạn start sau khi nhận GOTO để tránh đếm nhầm trạm/start.
*/
const unsigned long START_IGNORE_MS = 1800;

/*
  Sau khi phát hiện vạch ngang:
  - Robot đi thẳng một đoạn để vượt vạch.
  - 250 ms cuối cho PID bắt line lại sớm.
*/
const unsigned long STOP_POINT_BYPASS_MS = 650;

/*
  Sau khi phát hiện vạch ngang, bỏ qua một thời gian để chống đếm trùng.
*/
const unsigned long STOP_POINT_IGNORE_MS = 1000;

/*
  Tốc độ vượt vạch ngang.
*/
int STOP_POINT_BYPASS_SPEED = 220;

/*
  V6: stable mềm.
  Nếu vẫn bỏ sót điểm dừng: giảm về 2.
  Nếu nhận nhầm cua: tăng lên 4.
*/
const int STOP_POINT_STABLE_NEEDED = 3;

/*
  Chống đếm trùng theo thời gian.
*/
const unsigned long STOP_POINT_MIN_INTERVAL_MS = 1000;

/*
  Giảm tốc nhẹ khi bắt đầu thấy dấu hiệu vạch ngang.
*/
const int CROSS_APPROACH_SPEED = 120;

int stableCounterCross = 0;
int stopPointCount = 0;
int currentStopPoint = 0;

unsigned long stopPointIgnoreUntil = 0;
unsigned long stopPointBypassUntil = 0;
unsigned long lastStopPointDetectedAt = 0;

enum StopPointEvent {
  STOP_POINT_NONE = 0,
  STOP_POINT_DETECTED = 1,
  STOP_POINT_BYPASS = 2,
  STOP_POINT_TARGET_REACHED = 3
};

StopPointEvent lastStopPointEvent = STOP_POINT_NONE;

// =====================================================
// RETURN TO BASE CONFIG
// =====================================================
const unsigned long RETURN_BASE_MIN_RUN_MS = 800;
const unsigned long BASE_GAP_DETECT_MS = 200;

unsigned long returnStartedAt = 0;
unsigned long baseGapLostStartedAt = 0;

// =====================================================
// PID CONFIG
// =====================================================
float Kp = 20.0;
float Ki = 0.0;
float Kd = 12.5;

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

// Khi chạy thật nên false.
// Khi debug nhận vạch ngang, đổi true tạm thời.
bool DEBUG_MODE = false;

// =====================================================
// FORWARD DECLARATIONS
// =====================================================
void arriveAtBase();
void startReturnToBase();

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
  setRightMotor(0, true);
  setLeftMotor(0, true);
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

void pivotTurnLeft() {
  setLeftMotor(PIVOT_REVERSE_SPEED, false);
  setRightMotor(PIVOT_FORWARD_SPEED, true);
}

void pivotTurnRight() {
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
// STOP POINT FUNCTIONS - V6 CROSS SCORE
// =====================================================
int getCrossLineScore(uint8_t rawBits) {
  bool s1 = rawBits & (1 << 0);  // trái ngoài
  bool s2 = rawBits & (1 << 1);  // trái trong
  bool s3 = rawBits & (1 << 2);  // giữa
  bool s4 = rawBits & (1 << 3);  // phải trong
  bool s5 = rawBits & (1 << 4);  // phải ngoài

  int activeCount = countActiveLineSensors(rawBits);

  // 5 cảm biến cùng thấy line: chắc chắn là vạch ngang.
  if (rawBits == 0b11111) {
    return 5;
  }

  // 4/5 cảm biến và có cảm biến giữa: rất có khả năng là vạch ngang.
  if (activeCount >= 4 && s3) {
    return 4;
  }

  // 3 cảm biến giữa cùng thấy line: có thể là vạch ngang hẹp hoặc xe lệch nhẹ.
  if (rawBits == 0b01110) {
    return 3;
  }

  // Hai biên ngoài + giữa. Có thể xảy ra khi xe đi chéo qua vạch ngang.
  // Chỉ cho score 3, cần stable counter xác nhận.
  if (s1 && s3 && s5) {
    return 3;
  }

  // Một bên rộng + giữa. Có thể là xe đi chéo qua vạch ngang, nhưng chưa đủ chắc.
  if (rawBits == 0b00111 || rawBits == 0b11100) {
    return 2;
  }

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

void bypassCurrentStopPoint() {
  resetPIDAfterStopPoint();

  unsigned long now = millis();

  stopPointBypassUntil = now + STOP_POINT_BYPASS_MS;
  stopPointIgnoreUntil = now + STOP_POINT_IGNORE_MS;

  lastStopPointEvent = STOP_POINT_BYPASS;
  lastEvent = "BYPASS_STOP:" + String(currentStopPoint);
}

void arriveAtTargetStopPoint() {
  stopMotors();

  robotMode = MODE_WAITING_AT_TABLE;
  hasActiveTask = false;

  lastStopPointEvent = STOP_POINT_TARGET_REACHED;
  lastEvent = "ARRIVED:" + String(currentStopPoint);

  stopPointIgnoreUntil = millis() + STOP_POINT_IGNORE_MS;

  if (DEBUG_MODE) {
    Serial.print("[ARRIVED] target=");
    Serial.print(targetStopPoint);
    Serial.print(" current=");
    Serial.println(currentStopPoint);
  }
}

void handleStopPointAction() {
  if (DEBUG_MODE) {
    Serial.print("[CROSS DETECTED] count=");
    Serial.print(stopPointCount);
    Serial.print(" current=");
    Serial.print(currentStopPoint);
    Serial.print(" target=");
    Serial.print(targetStopPoint);
    Serial.print(" mode=");
    Serial.println((int)robotMode);
  }

  if (robotMode == MODE_GOING_TO_TABLE) {
    if (currentStopPoint == targetStopPoint) {
      arriveAtTargetStopPoint();
      return;
    }

    bypassCurrentStopPoint();
    return;
  }

  if (robotMode == MODE_RETURNING_TO_BASE) {
    bypassCurrentStopPoint();
    return;
  }

  bypassCurrentStopPoint();
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
      errorOut = -2.00;
      return true;

    case 0b01100:
      errorOut = 2.00;
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
// LINE FOLLOWING PID STEP
// =====================================================
void lineFollowStep() {
  float rawError = 0.0;
  uint8_t bits = 0;

  uint8_t rawBits = readRawLineBits();
  unsigned long now = millis();

  /*
    Bypass sau vạch ngang:
    - Giai đoạn đầu: chạy thẳng qua vạch.
    - 250 ms cuối: cho PID bắt line lại sớm.
  */
  if (now < stopPointBypassUntil) {
    unsigned long remain = stopPointBypassUntil - now;

    if (remain > 250) {
      driveForwardFixed(STOP_POINT_BYPASS_SPEED);

      if (DEBUG_MODE) {
        Serial.print("[BYPASS] raw=");
        printBits(rawBits);
        Serial.print(" current=");
        Serial.println(currentStopPoint);
      }

      return;
    }
  }

  /*
    Giảm tốc nhẹ khi bắt đầu thấy dấu hiệu vạch ngang.
    Không return, vẫn tiếp tục kiểm tra stable counter.
  */
  int crossScoreNow = getCrossLineScore(rawBits);

  if (
    robotMode == MODE_GOING_TO_TABLE &&
    crossScoreNow >= 3 &&
    now >= stopPointIgnoreUntil &&
    (now - lastStopPointDetectedAt >= STOP_POINT_MIN_INTERVAL_MS)
  ) {
    driveForwardFixed(CROSS_APPROACH_SPEED);
  }

  StopPointEvent stopEvent = checkStopPoint(rawBits);

  if (stopEvent == STOP_POINT_DETECTED) {
    handleStopPointAction();
    return;
  }

  bool hasLine = computeLineError(rawError, bits, rawBits);

  if (!hasLine) {
    if (lineLostTime == 0) {
      lineLostTime = now;
    }

    /*
      Khi RETURN:
      - Đã chạy đủ thời gian tối thiểu.
      - Đã đi qua điểm dừng 4.
      - Gặp đoạn ngắt line đủ BASE_GAP_DETECT_MS.
      => xem như về trạm.
    */
    if (robotMode == MODE_RETURNING_TO_BASE) {
      bool minRunOk = (now - returnStartedAt) >= RETURN_BASE_MIN_RUN_MS;
      bool afterStop4 = (currentStopPoint == TOTAL_STOP_POINTS);

      if (minRunOk && afterStop4) {
        if (baseGapLostStartedAt == 0) {
          baseGapLostStartedAt = now;
        }

        if (now - baseGapLostStartedAt >= BASE_GAP_DETECT_MS) {
          arriveAtBase();
          return;
        }
      }
    }

    if (now - lineLostTime < LINE_LOST_STOP_MS) {
      searchLine();
    } else {
      stopMotors();
      lastEvent = "LINE_LOST";

      if (robotMode == MODE_RETURNING_TO_BASE) {
        robotMode = MODE_ERROR;
        hasActiveTask = false;
        lastEvent = "RETURN_LINE_LOST";
      }
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

  lineLostTime = 0;
  baseGapLostStartedAt = 0;

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

  /*
    Adaptive speed + GOTO Boost:
    Tăng tốc độ lên 220 trong 700ms đầu khi vừa nhận lệnh GOTO.
    Sau đó trở về logic tự động giảm tốc khi lệch line.
  */
  float absErr = fabs(filteredError);

  int adaptiveBase = BASE_SPEED;

  if (robotMode == MODE_GOING_TO_TABLE && (now - gotoStartTime <= GOTO_BOOST_MS)) {
    adaptiveBase = GOTO_BOOST_SPEED;
  } else {
    if (absErr >= 4.5) {
      adaptiveBase = BASE_SPEED - 30;
    } else if (absErr >= 3.0) {
      adaptiveBase = BASE_SPEED - 22;
    } else if (absErr >= 1.5) {
      adaptiveBase = BASE_SPEED - 10;
    }
  }

  // Nới lỏng chặn trên để cho phép robot xuất ra tốc độ 220 trong giai đoạn tăng tốc
  adaptiveBase = constrain(adaptiveBase, MIN_SPEED, max(BASE_SPEED, GOTO_BOOST_SPEED));

  int leftSpeed  = adaptiveBase + correction;
  int rightSpeed = adaptiveBase - correction;

  bool pivotActive = now < pivotHoldUntil;

  if ((isExtremeLeft(rawBits) || isExtremeLeft(bits)) && pivotActive) {
    pivotTurnLeft();
  } else if ((isExtremeRight(rawBits) || isExtremeRight(bits)) && pivotActive) {
    pivotTurnRight();
  } else if (isHardLeft(rawBits) || isHardLeft(bits) || (now < turnHoldUntil && turnMemory < 0)) {
    leftSpeed  = HARD_TURN_INNER_SPEED;
    rightSpeed = HARD_TURN_OUTER_SPEED;
    driveForwardPWM(leftSpeed, rightSpeed);
  } else if (isHardRight(rawBits) || isHardRight(bits) || (now < turnHoldUntil && turnMemory > 0)) {
    leftSpeed  = HARD_TURN_OUTER_SPEED;
    rightSpeed = HARD_TURN_INNER_SPEED;
    driveForwardPWM(leftSpeed, rightSpeed);
  } else if (isPrepareLeft(rawBits) || isPrepareLeft(bits)) {
    leftSpeed  = adaptiveBase - 35;
    rightSpeed = adaptiveBase + 45;
    leftSpeed  = constrain(leftSpeed,  MIN_SPEED, MAX_SPEED);
    rightSpeed = constrain(rightSpeed, MIN_SPEED, MAX_SPEED);
    driveForwardPWM(leftSpeed, rightSpeed);
  } else if (isPrepareRight(rawBits) || isPrepareRight(bits)) {
    leftSpeed  = adaptiveBase + 45;
    rightSpeed = adaptiveBase - 35;
    leftSpeed  = constrain(leftSpeed,  MIN_SPEED, MAX_SPEED);
    rightSpeed = constrain(rightSpeed, MIN_SPEED, MAX_SPEED);
    driveForwardPWM(leftSpeed, rightSpeed);
  } else {
    leftSpeed  = constrain(leftSpeed,  MIN_SPEED, MAX_SPEED);
    rightSpeed = constrain(rightSpeed, MIN_SPEED, MAX_SPEED);
    driveForwardPWM(leftSpeed, rightSpeed);
  }

  if (isCenterStable(bits) && now > turnHoldUntil && now > pivotHoldUntil) {
    turnMemory = 0;
  }

  if (DEBUG_MODE) {
    static unsigned long lastDebugPrint = 0;

    if (now - lastDebugPrint > 150) {
      lastDebugPrint = now;

      Serial.print("[RUN] raw=");
      printBits(rawBits);
      Serial.print(" filt=");
      printBits(bits);
      Serial.print(" crossScore=");
      Serial.print(getCrossLineScore(rawBits));
      Serial.print(" crossStable=");
      Serial.print(stableCounterCross);
      Serial.print(" current=");
      Serial.print(currentStopPoint);
      Serial.print(" target=");
      Serial.print(targetStopPoint);
      Serial.print(" err=");
      Serial.print(filteredError, 2);
      Serial.print(" corr=");
      Serial.print(correction, 2);
      Serial.print(" base=");
      Serial.print(adaptiveBase);
      Serial.print(" L=");
      Serial.print(leftSpeed);
      Serial.print(" R=");
      Serial.println(rightSpeed);
    }
  }
}

// =====================================================
// RETURN / BASE FUNCTIONS
// =====================================================
void startReturnToBase() {
  if (robotMode != MODE_WAITING_AT_TABLE && robotMode != MODE_STOPPED) {
    lastEvent = "RETURN_REJECTED_NOT_AT_TABLE";
    return;
  }

  robotMode = MODE_RETURNING_TO_BASE;
  hasActiveTask = true;
  lastEvent = "RETURN_TO_BASE";

  returnStartedAt = millis();
  baseGapLostStartedAt = 0;

  resetPIDAfterStopPoint();

  stopPointBypassUntil = millis() + STOP_POINT_BYPASS_MS;
  stopPointIgnoreUntil = millis() + STOP_POINT_IGNORE_MS;

  if (DEBUG_MODE) {
    Serial.println("[TASK] RETURN_TO_BASE");
  }
}

void arriveAtBase() {
  stopMotors();

  robotMode = MODE_IDLE_AT_BASE;
  hasActiveTask = false;

  targetStopPoint = 0;
  stopPointCount = 0;
  currentStopPoint = 0;

  stableCounterCross = 0;
  baseGapLostStartedAt = 0;
  lastStopPointDetectedAt = 0;

  resetPIDAfterStopPoint();

  lastEvent = "ARRIVED_BASE";

  if (DEBUG_MODE) {
    Serial.println("[BASE] ARRIVED_BASE");
  }
}

// =====================================================
// ROBOT MODE HELPERS
// =====================================================
String modeToString(RobotMode mode) {
  switch (mode) {
    case MODE_IDLE_AT_BASE:
      return "IDLE_AT_BASE";

    case MODE_GOING_TO_TABLE:
      return "GOING_TO_TABLE";

    case MODE_WAITING_AT_TABLE:
      return "WAITING_AT_TABLE";

    case MODE_RETURNING_TO_BASE:
      return "RETURNING_TO_BASE";

    case MODE_STOPPED:
      return "STOPPED";

    case MODE_ERROR:
      return "ERROR";

    default:
      return "UNKNOWN";
  }
}

void resetNavigationForNewTask() {
  stableCounterCross = 0;
  stopPointCount = 0;
  currentStopPoint = 0;

  stopPointBypassUntil = 0;
  stopPointIgnoreUntil = millis() + START_IGNORE_MS;

  lastStopPointDetectedAt = 0;
  lastStopPointEvent = STOP_POINT_NONE;

  baseGapLostStartedAt = 0;

  resetPIDAfterStopPoint();
}

void startGotoTable(int tableId) {
  targetStopPoint = tableId;
  hasActiveTask = true;
  robotMode = MODE_GOING_TO_TABLE;
  lastEvent = "GOTO:" + String(tableId);

  resetNavigationForNewTask();
  
  // Lưu thời điểm nhận lệnh GOTO để đếm 700ms tăng tốc
  gotoStartTime = millis();

  if (DEBUG_MODE) {
    Serial.print("[TASK] Start GOTO:");
    Serial.println(tableId);
  }
}

void emergencyStopRobot() {
  stopMotors();

  hasActiveTask = false;
  robotMode = MODE_STOPPED;
  lastEvent = "STOP";
}

// =====================================================
// TCP COMMAND HANDLER
// =====================================================
String buildStatusString() {
  String s = "STATUS:";
  s += modeToString(robotMode);
  s += ",target=" + String(targetStopPoint);
  s += ",current=" + String(currentStopPoint);
  s += ",count=" + String(stopPointCount);
  s += ",active=" + String(hasActiveTask ? 1 : 0);
  s += ",crossStable=" + String(stableCounterCross);
  s += ",last=" + lastEvent;
  s += ",ip=" + WiFi.localIP().toString();

  return s;
}

String processCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  lastCommand = cmd;

  if (cmd.length() == 0) {
    return "ERR:EMPTY_COMMAND";
  }

  if (cmd == "PING") {
    return "PONG";
  }

  if (cmd == "STATUS") {
    return buildStatusString();
  }

  if (cmd == "STOP") {
    emergencyStopRobot();
    return "OK:STOP";
  }

  if (cmd == "RETURN" || cmd == "CONTINUE") {
    if (robotMode != MODE_WAITING_AT_TABLE && robotMode != MODE_STOPPED) {
      return "ERR:RETURN_REQUIRES_WAITING_AT_TABLE";
    }

    startReturnToBase();
    return "OK:RETURN";
  }

  if (cmd.startsWith("GOTO:")) {
    String arg = cmd.substring(5);
    int tableId = arg.toInt();

    if (tableId < 1 || tableId > 4) {
      return "ERR:INVALID_TABLE";
    }

    if (robotMode == MODE_GOING_TO_TABLE || robotMode == MODE_RETURNING_TO_BASE) {
      return "ERR:ROBOT_BUSY";
    }

    startGotoTable(tableId);
    return "OK:GOTO:" + String(tableId);
  }

  return "ERR:UNKNOWN_COMMAND";
}

void handleTcpClient() {
  WiFiClient client = server.available();

  if (!client) {
    return;
  }

  client.setTimeout(300);
  client.setNoDelay(true);

  String cmd = "";
  unsigned long startMs = millis();

  while (client.connected() && (millis() - startMs < 300)) {
    while (client.available()) {
      char c = (char)client.read();

      if (c == '\r') {
        continue;
      }

      if (c == '\n') {
        startMs = 0;
        break;
      }

      cmd += c;

      if (cmd.length() >= 64) {
        startMs = 0;
        break;
      }
    }

    if (startMs == 0) {
      break;
    }

    if (cmd.length() > 0 && !client.available()) {
      delay(2);

      if (!client.available()) {
        break;
      }
    }

    delay(1);
  }

  String response = processCommand(cmd);

  client.print(response);
  client.print("\n");
  client.flush();

  delay(5);

  if (DEBUG_MODE) {
    Serial.print("[TCP] cmd=");
    Serial.print(cmd);
    Serial.print(" response=");
    Serial.println(response);
  }

  client.stop();
}

// =====================================================
// WIFI SETUP
// =====================================================
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("[WIFI] Connecting to ");
  Serial.println(WIFI_SSID);

  unsigned long startAttempt = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WIFI] Connected. IP=");
    Serial.println(WiFi.localIP());
    lastEvent = "WIFI_CONNECTED";
  } else {
    Serial.println("[WIFI] Failed to connect.");
    lastEvent = "WIFI_FAILED";
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
  resetNavigationForNewTask();

  robotMode = MODE_IDLE_AT_BASE;
  hasActiveTask = false;
  targetStopPoint = 0;
  lastEvent = "SYSTEM_READY";

  connectWiFi();
  server.begin();

  Serial.print("[TCP] Server started on port ");
  Serial.println(TCP_PORT);

  Serial.println("[LIBBOT] ESP32 V6 improved cross-score stop-point logic ready.");
}

// =====================================================
// LOOP
// =====================================================
void loop() {
  handleTcpClient();

  if (WiFi.status() != WL_CONNECTED) {
    stopMotors();

    robotMode = MODE_ERROR;
    hasActiveTask = false;
    lastEvent = "WIFI_DISCONNECTED";

    delay(200);
    return;
  }

  if (robotMode == MODE_GOING_TO_TABLE || robotMode == MODE_RETURNING_TO_BASE) {
    lineFollowStep();
  } else {
    stopMotors();
  }

  delay(5);
}
