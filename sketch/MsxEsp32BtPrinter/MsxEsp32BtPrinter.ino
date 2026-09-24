// MSX -> ESP32-WROOM -> Bluetooth SPPプリンタ（BML-80BT）
//
// MSX -> ESP32: DATA0..7 and STROBE は 5V TTL
// 10k series / 20k to GND の抵抗分圧で MSX 5V -> ESP32 3.3V に変換
// ESP32 -> MSX の BUSY は 3.3V 出力のまま
// STROBE は立ち下がりエッジの割り込み（Active Low）
// slaveAddress[] に Bluetoothプリンタの MAC address を指定

#include <Arduino.h>
#include <BluetoothSerial.h>
#include "soc/gpio_struct.h"

BluetoothSerial btSerial;

// ---------- Bluetoothプリンタ ----------
uint8_t slaveAddress[6] = {0x00, 0x01, 0x90, 0xE6, 0x60, 0xB7};

// ---------- MSX プリンタポート ----------
static constexpr uint8_t PIN_D0     = 18;
static constexpr uint8_t PIN_D1     = 19;
static constexpr uint8_t PIN_D2     = 23;
static constexpr uint8_t PIN_D3     = 13;
static constexpr uint8_t PIN_D4     = 14;
static constexpr uint8_t PIN_D5     = 27;
static constexpr uint8_t PIN_D6     = 16;
static constexpr uint8_t PIN_D7     = 17;
static constexpr uint8_t PIN_STROBE = 25;
static constexpr uint8_t PIN_BUSY   = 26;
// ---------- 操作スイッチ ----------
static constexpr uint8_t PIN_KEY    = 12;

// ---------- MSX受信リングバッファ ----------
static constexpr uint16_t RX_BUFFER_SIZE  = 1024;
static constexpr uint16_t RX_BUFFER_MASK  = RX_BUFFER_SIZE  - 1;

volatile uint8_t  rxBuffer[RX_BUFFER_SIZE ];
volatile uint16_t rxWritePos = 0;
volatile uint16_t rxReadPos  = 0;

// ---------- Bluetooth送信リングバッファ ----------
static constexpr uint16_t TX_BUFFER_SIZE = 2048;
static constexpr uint16_t TX_BUFFER_MASK = TX_BUFFER_SIZE - 1;

uint8_t  txBuffer[TX_BUFFER_SIZE];
uint16_t txWritePos = 0;
uint16_t txReadPos  = 0;

// Rxバッファフル検出のしきい値
static constexpr uint16_t RX_BUSY_MARGIN = 16;
// Txバッファフル検出のしきい値
// 将来ダウンロード文字制御等を追加する余裕も持たせる。
static constexpr uint16_t TX_MARGIN = 64;

bool isReady = false;

// ---------- プリンタ送信データ定義 ----------
// Beepオン 3回
const uint8_t strBeep[1]    = {0x07};
// 印刷濃度 140%
const uint8_t strSetDark[3] = {0x1B, 0x59, 0x05};
// 文字コード SJIS
const uint8_t strSetSJIS[3] = {0x1C, 0x43, 0x01};
// LF
const uint8_t strLf[1]      = {0x0A};

// LFスイッチのチャタリング除去時間
static constexpr uint32_t LF_DEBOUNCE_TIME = 50;

// ---------- リングバッファ ----------
static inline uint16_t bufferUsed(uint16_t w, uint16_t r, uint16_t mask) {
    return (w - r) & mask;
}

static inline uint16_t bufferFree(uint16_t w, uint16_t r, uint16_t mask) {
    return mask - bufferUsed(w, r, mask);
}

// ---------- GPIO  ----------
// GPIO.in のスナップショット -> 8ビットプリンタデータ
static inline uint8_t IRAM_ATTR extractPrinterData(uint32_t gpio) {
    return ((gpio >> PIN_D0) & 1U) << 0 |
           ((gpio >> PIN_D1) & 1U) << 1 |
           ((gpio >> PIN_D2) & 1U) << 2 |
           ((gpio >> PIN_D3) & 1U) << 3 |
           ((gpio >> PIN_D4) & 1U) << 4 |
           ((gpio >> PIN_D5) & 1U) << 5 |
           ((gpio >> PIN_D6) & 1U) << 6 |
           ((gpio >> PIN_D7) & 1U) << 7;
}

static inline uint16_t IRAM_ATTR rxBufferUsed(uint16_t w, uint16_t r) {
    return (w - r) & RX_BUFFER_MASK;
}

static inline void IRAM_ATTR busyOn() {
    GPIO.out_w1ts = (1UL << PIN_BUSY);   // HIGH
}

static inline void IRAM_ATTR busyOff() {
    GPIO.out_w1tc = (1UL << PIN_BUSY);   // LOW
}

// ---------- STROBE割り込み ----------
// 1. BUSY を即 Hi にする
// 2. GPIO.in のスナップショットを取得
// 3. 1バイトをバッファへ保存
// 4. バッファに余裕があれば BUSY を Low にする
void IRAM_ATTR onPrinterStrobe() {
    busyOn();

    const uint32_t gpio = GPIO.in;
    const uint8_t data = extractPrinterData(gpio);

    const uint16_t w = rxWritePos;
    const uint16_t next = (w + 1) & RX_BUFFER_MASK;

    if (next != rxReadPos) {
        rxBuffer[w] = data;
        rxWritePos = next;

        const uint16_t used = rxBufferUsed(next, rxReadPos);
        if (used < (RX_BUFFER_SIZE - RX_BUSY_MARGIN)) {
            busyOff();
        }
    }
    // バッファ不足時は BUSY は High のまま、メインループ内で落とす
}

// ---------- Bluetooth送信バッファ ----------
static bool txPut(uint8_t data) {
    const uint16_t next = (txWritePos + 1) & TX_BUFFER_MASK;
    if (next == txReadPos) {
        return false;
    }
    txBuffer[txWritePos] = data;
    txWritePos = next;
    return true;
}

static uint16_t txFree() {
    return bufferFree(txWritePos, txReadPos, TX_BUFFER_MASK);
}

// ---------- JIS -> SJIS変換 ----------
static bool jisToSjis(uint8_t jis1, uint8_t jis2, uint8_t &sjis1, uint8_t &sjis2) {
    // JIS X 0208の有効範囲
    if (jis1 < 0x21 || jis1 > 0x7E ||
        jis2 < 0x21 || jis2 > 0x7E) {
        return false;
    }

    uint8_t row = jis1;
    uint8_t cell = jis2;

    // Shift-JIS 1バイト目
    sjis1 = ((row - 0x21) >> 1) + 0x81;

    if (sjis1 > 0x9F) {
        sjis1 += 0x40;
    }

    // Shift-JIS 2バイト目
    if (row & 1) {
        // JISの奇数区
        sjis2 = cell + 0x1F;
        if (sjis2 >= 0x7F) {
            sjis2++;
        }
    } else {
        // JISの偶数区
        sjis2 = cell + 0x7E;
    }
    return true;
}

// ---------- MSX漢字プリンタコード変換 ----------
// MSXプリンタの漢字モード
static bool msxKanjiMode = false;
// ESC受信待ち
static bool msxEscPending = false;
// 漢字1バイト目受信待ち
static bool msxKanjiFirstPending = false;
static uint8_t msxKanjiFirst = 0;

// MSXから受信した1バイトを解釈してBluetooth送信バッファへ積む
static void processMsxPrinterByte(uint8_t data) {
    // ESCの次のバイト
    if (msxEscPending) {
        msxEscPending = false;
        switch (data) {
            case 0x4B:  // ESC K: MSX 漢字モード開始
                msxKanjiMode = true;
                msxKanjiFirstPending = false;
                return;
            case 0x48:  // ESC H: MSX 漢字モード終了
                msxKanjiMode = false;
                msxKanjiFirstPending = false;
                return;
            default:
                // 今回解釈しないESCコマンドは ESC + data をそのままプリンタへ送る
                btSerial.write((uint8_t)0x1B);
                btSerial.write(data);
                return;
        }
    }

    // ESC受信
    if (data == 0x1B) {
        msxEscPending = true;
        return;
    }

    // 漢字モード
    if (msxKanjiMode) {
        // JIS漢字コード1バイト目
        if (!msxKanjiFirstPending) {
            msxKanjiFirst = data;
            msxKanjiFirstPending = true;
            return;
        }

        // JIS漢字コード2バイト目
        if (msxKanjiFirst == 0x00) {
            // 0x00 0xXX はそのまま送信
            txPut(data);
        } else {
            uint8_t sjis1;
            uint8_t sjis2;
            if (jisToSjis(msxKanjiFirst, data, sjis1, sjis2)) {
                txPut(sjis1);
                txPut(sjis2);
            }
        }
        msxKanjiFirstPending = false;
        return;
    }

    // 通常文字はそのまま送信
    txPut(data);
}

// ---------- MSX受信バッファ -> コード変換 ----------
static void processRxBuffer() {
    // 送信バッファに十分余裕がある間だけ処理
    while ((rxReadPos != rxWritePos) && (txFree() >= TX_MARGIN)) {
        const uint16_t r = rxReadPos;
        const uint8_t data = rxBuffer[r];
        // デバッグ用出力
        Serial.printf("%02X ", data);
        // MSXコードを解析
        processMsxPrinterByte(data);
        // MSX受信バッファを進める
        rxReadPos = (r + 1) & RX_BUFFER_MASK;
    }
}

//---------- Bluetoothへ送信 ----------
static void sendTxBuffer() {
    while (txReadPos != txWritePos) {
        const uint16_t r = txReadPos;
        const uint16_t w = txWritePos;
        size_t sendSize;
        if (r < w) {
            // 連続領域
            sendSize = w - r;
        } else {
            // バッファ末尾まで
            sendSize = TX_BUFFER_SIZE - r;
        }

        const size_t sent = btSerial.write(&txBuffer[r], sendSize);
        if (sent == 0) {
            break;
        }
        txReadPos = (r + sent) & TX_BUFFER_MASK;
    }
}

// ---------- プリンタの初期設定 ----------
void initPrinter() {
    // 印刷濃度を 140% にセット
    btSerial.write(strSetDark, sizeof(strSetDark));
    // 文字コードをシフトJISにセット
    btSerial.write(strSetSJIS, sizeof(strSetSJIS));
    // ビープ音を3回出力
    if (btSerial.connected()) {
        for (int i = 0; i < 3; i++ ) {
            btSerial.write(strBeep, sizeof(strBeep));
        }
    }
    return;
}

// ---------- プリンタの状態制御 ----------
// プリンタへ送信OKを通知
void setReady() {
    if (!isReady) {
        //プリンタの初期設定とビープ音の出力
        initPrinter();
        // STROBE の立ち下がりで割り込みを許可
        attachInterrupt(digitalPinToInterrupt(PIN_STROBE), onPrinterStrobe, FALLING);
        isReady = true;
    }
    // BUSY Low で受信開始
    digitalWrite(PIN_BUSY, LOW);
}

// プリンタへ送信待機を通知
void setNotReady() {
    // BUSY High で送信待機
    digitalWrite(PIN_BUSY, HIGH);
    if (isReady) {
        // STROBE の割り込みを禁止
        detachInterrupt(digitalPinToInterrupt(PIN_STROBE));
        isReady = false;
    }
}

// ---------- setup ----------
void setup() {
    Serial.begin(115200);
    delay(500);

    // GPIOの初期化
    pinMode(PIN_D0, INPUT);
    pinMode(PIN_D1, INPUT);
    pinMode(PIN_D2, INPUT);
    pinMode(PIN_D3, INPUT);
    pinMode(PIN_D4, INPUT);
    pinMode(PIN_D5, INPUT);
    pinMode(PIN_D6, INPUT);
    pinMode(PIN_D7, INPUT);
    pinMode(PIN_STROBE, INPUT);
    pinMode(PIN_BUSY, OUTPUT);
    digitalWrite(PIN_BUSY, HIGH);   // 初期化注 BUSY は High
    pinMode(PIN_KEY, INPUT_PULLUP); // キーは内蔵プルアップ

    //Bluetoothシリアル通信ポートをマスターモードで開く
    btSerial.begin("MSX-Printer", true);

    //ESP32とBluetooth接続する
    Serial.println("BT PrinterとBluetooth接続中...");

    //PIN 0000で接続
    btSerial.setPin("0000", 4);
    if (btSerial.connect(slaveAddress)) {
        Serial.println("接続成功.");
        setReady();
    } else {
        Serial.println("接続失敗...");
        setNotReady();
        // メインループで再接続するため初期化は完了とする
    }
}

// ---------- loop ----------
void loop() {
    static bool prevLfButton = HIGH;
    static uint32_t lastLfTime = 0;

    // プリンタとの接続が切れているなら再接続
    if (!btSerial.connected()) {
        setNotReady();
        delay(100);
        if (btSerial.connect(slaveAddress)) {
            Serial.println("再接続成功..");
            setReady();
        }
        return;
    }

    // MSX受信データを変換
    processRxBuffer();
    // 変換済みデータをBluetoothへ送信
    sendTxBuffer();

    const uint16_t used = rxBufferUsed(rxWritePos, rxReadPos);

    // Rx/Txバッファともに空きが十分なら送信を許可する
    if ((used < (RX_BUFFER_SIZE - RX_BUSY_MARGIN)) && (txFree() >= TX_MARGIN)) {
        busyOff();
    } else {
        busyOn();
    }
    // バッファがフルのときはそのままにする

    // LFキーのチェック
    const bool lfButton = digitalRead(PIN_KEY);
    // HIGH -> LOW の押下エッジを検出
    if ((prevLfButton == HIGH) && (lfButton == LOW)) {
        const uint32_t now = millis();
        // チャタリング除去
        if ((now - lastLfTime) >= LF_DEBOUNCE_TIME) {
            // MSXから受信したデータを優先、Rx/Txバッファがともに空のときだけLFを直接送信
            if ((rxReadPos == rxWritePos) && (txReadPos == txWritePos)) {
                btSerial.write(strLf, sizeof(strLf));
            }
            lastLfTime = now;
        }
    }
    prevLfButton = lfButton;
}
