// Tangible AI Riddle Board — ESP32-S3 + PN532 (HSU) → เซิร์ฟเวอร์บน Render (wss)

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <PN532_HSU.h>
#include <PN532.h>
#include <NfcAdapter.h>

// ---------------- ตั้งค่า Wi-Fi ----------------
const char *ssid = "PP";
const char *password = "0954098456";

// ---------------- ตั้งค่า Server บน Render ------
const char *websocket_host = "nfc-riddlegame.onrender.com";
const uint16_t websocket_port = 443;     // wss (SSL)
const char *websocket_path = "/ws";      // WebSocket ดิบ โปรโตคอล JSON เดิมของ Python
const char *device_key = "";             // ถ้าตั้ง DEVICE_KEY บน Render ให้ใส่ค่าเดียวกันที่นี่

WebSocketsClient webSocket;
String wsPath;

HardwareSerial SerialPN532(2);
PN532_HSU pn532hsu(SerialPN532);
NfcAdapter nfc(pn532hsu);

// ---------------- สถานะการทำงาน ----------------
bool wsConnected = false;
bool pendingWrite = false;   // มีคำสั่ง write_tag รอเขียนลงแท็ก
String dataToWrite = "";
unsigned long pendingSince = 0;
bool awaitingWrite = false;  // ส่ง tag_scanned แล้ว และแท็กยังไม่ถูกยกออก (จึงเขียนกลับลงใบเดิมได้)
bool tagLocked = false;      // แตะแล้ว ต้องยกแท็กออกก่อนจึงอ่านครั้งต่อไป
bool tagWasPresent = false;
uint8_t missCount = 0;       // นับครั้งที่ตรวจไม่พบแท็กติดกัน (กันสัญญาณกระพริบ)
unsigned long lastReadTime = 0;

// เวลารอให้ PN532 ตอบเมื่อมองหาแท็ก — อย่าตั้งต่ำกว่า ~50 ms
// (ค่า 30 ms สั้นเกินไป PN532 ผ่าน HSU มักตอบไม่ทัน ทำให้เหมือน "อ่านแท็กไม่เจอ")
const unsigned long TAG_POLL_MS = 100;
const uint8_t REMOVE_AFTER_MISSES = 3;     // ไม่เจอแท็กติดกันกี่ครั้งถึงถือว่ายกออกแล้ว
const unsigned long READ_DEBOUNCE = 1000;  // กันอ่านรัว (ms)
const unsigned long WRITE_TIMEOUT = 8000;  // รอเขียนแท็กไม่เกินกี่ ms

// จัดการข้อความที่เซิร์ฟเวอร์ส่งมา
void handleServerMessage(const char *msg, size_t length)
{
  // เซิร์ฟเวอร์ broadcast โจทย์ให้ทุกอุปกรณ์ (ข้อความยาว) ESP32 ไม่ต้องใช้ จึงไม่แปลง JSON
  if (strstr(msg, "\"start_game\""))
  {
    Serial.println("[WS] Server เริ่มโจทย์ใหม่แล้ว");
    return;
  }

  if (strstr(msg, "\"write_tag\""))
  {
    JsonDocument doc;
    if (deserializeJson(doc, msg, length))
    {
      Serial.println("[WS] write_tag รูปแบบ JSON ไม่ถูกต้อง");
      return;
    }
    const char *nextId = doc["payload"];
    if (!nextId || !*nextId)
      return;

    // ถ้าแท็กถูกยกออกไปก่อนคำสั่งมาถึง ห้ามเขียน ไม่งั้นอาจไปเขียนทับแท็กใบอื่น
    if (!awaitingWrite)
    {
      Serial.println("[NFC] แท็กถูกยกออกก่อนได้รับคำสั่งเขียน — ข้าม");
      return;
    }
    dataToWrite = String(nextId);
    pendingWrite = true;
    pendingSince = millis();
    Serial.printf("[NFC] พร้อมเขียน %s ลงแท็ก กรุณาวางแท็กค้างไว้บน Reader...\n", nextId);
    return;
  }

  // ข้อความอื่น เช่น {"status":"error","message":"Tag ID 'TAG_999' not found in database"}
  Serial.printf("[WS] Received: %.120s\n", msg);
}

void webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{
  switch (type)
  {
  case WStype_DISCONNECTED:
    Serial.println("[WS] Disconnected! (กำลังเชื่อมต่อใหม่...)");
    wsConnected = false;
    break;
  case WStype_CONNECTED:
    Serial.printf("[WS] Connected to %s\n", websocket_host);
    wsConnected = true;
    break;
  case WStype_TEXT:
    handleServerMessage((const char *)payload, length);
    break;
  case WStype_ERROR:
    Serial.println("[WS] Connection Error!");
    break;
  default:
    break;
  }
}

void setup()
{
  Serial.begin(115200);
  delay(500);
  SerialPN532.begin(115200, SERIAL_8N1, 17, 18); // RX=17, TX=18

  Serial.println("--- Tangible AI Riddle Board ---");

  // เชื่อมต่อ Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // ลด latency
  WiFi.begin(ssid, password);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
    if (millis() - wifiStart > 30000)
    {
      Serial.println("\nWiFi เชื่อมต่อไม่สำเร็จ รีสตาร์ท...");
      ESP.restart();
    }
  }
  Serial.println("\nWiFi Connected!");

  // เริ่มต้น WebSocket (SSL) ไปยัง Render
  wsPath = websocket_path;
  if (strlen(device_key) > 0)
  {
    wsPath += "?key=";
    wsPath += device_key;
  }
  webSocket.beginSSL(websocket_host, websocket_port, wsPath.c_str());
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);       // Render แพ็กเกจฟรีอาจใช้ ~30-50 วิ ปลุกเซิร์ฟเวอร์
  webSocket.enableHeartbeat(25000, 8000, 2);  // ตรวจจับการเชื่อมต่อที่ตายเงียบ

  // เริ่มต้น NFC
  // ถ้าไม่พบบอร์ด PN532 ไลบรารีจะพิมพ์ "Didn't find PN53x board" แล้วหยุดทำงานตรงนี้
  // (แปลว่าปัญหาอยู่ที่สายไฟ/สวิตช์โหมด HSU/การจ่ายไฟ ไม่เกี่ยวกับแท็ก)
  nfc.begin();
  Serial.println("NFC Reader Initialized. Waiting for tags...");
}

void loop()
{
  webSocket.loop(); // ให้ WebSocket ทำงานต่อเนื่อง

  // ไม่มีแท็กบน Reader → (เมื่อไม่เจอติดกันหลายครั้ง) ปลดล็อก และยกเลิกคำสั่งเขียนที่ค้างอยู่
  if (!nfc.tagPresent(TAG_POLL_MS))
  {
    if (++missCount >= REMOVE_AFTER_MISSES)
    {
      if (tagWasPresent)
        Serial.println("[NFC] แท็กถูกยกออกแล้ว");
      tagWasPresent = false;
      tagLocked = false;
      awaitingWrite = false;
      pendingWrite = false;
      missCount = REMOVE_AFTER_MISSES;
    }
    return;
  }
  missCount = 0;

  if (!tagWasPresent)
  {
    Serial.println("[NFC] ตรวจพบแท็กบน Reader");
    tagWasPresent = true;
  }

  // โหมดเขียนข้อมูล (เมื่อได้รับคำสั่ง write_tag จากเซิร์ฟเวอร์)
  if (pendingWrite)
  {
    if (millis() - pendingSince > WRITE_TIMEOUT)
    {
      Serial.println("[NFC] หมดเวลารอเขียนแท็ก — ยกเลิก");
      pendingWrite = false;
      awaitingWrite = false;
      return;
    }

    NdefMessage message = NdefMessage();
    message.addTextRecord(dataToWrite); // เขียน ID ถัดไปลงแท็ก

    if (nfc.write(message))
    {
      Serial.println("[NFC] Successfully wrote new ID to tag!");
      pendingWrite = false;
      awaitingWrite = false;
      tagLocked = true; // ต้องยกแท็กออกก่อน ไม่งั้นจะอ่าน ID ใหม่แล้วข้ามโจทย์เอง
      delay(500);
    }
    else
    {
      Serial.println("[NFC] Failed to write tag. Retrying...");
      delay(300);
    }
    return;
  }

  // โหมดอ่านข้อมูล (โหมดปกติ)
  if (tagLocked || millis() - lastReadTime < READ_DEBOUNCE)
    return;

  NfcTag tag = nfc.read();
  String uidStr = tag.getUidString();
  String tagContent = "";

  if (tag.hasNdefMessage())
  {
    NdefMessage message = tag.getNdefMessage();
    if (message.getRecordCount() > 0)
    {
      NdefRecord record = message.getRecord(0);
      int payloadLength = record.getPayloadLength();
      if (payloadLength > 0)
      {
        byte payload[payloadLength];
        record.getPayload(payload);

        // NDEF Text Record: ไบต์แรกคือ status byte บิต 0..5 = ความยาวรหัสภาษา (เช่น "en" = 2)
        // ดังนั้นข้อความเริ่มที่ไบต์ที่ 1 + ความยาวรหัสภาษา (กรณีปกติ = 3)
        int langLength = payload[0] & 0x3F;
        int textStart = 1 + langLength;
        if (textStart >= payloadLength)
          textStart = 0; // ไม่ใช่ Text Record ตามมาตรฐาน อ่านทั้งก้อน
        for (int c = textStart; c < payloadLength; c++)
        {
          tagContent += (char)payload[c];
        }
        tagContent.trim();
      }
    }
  }

  if (tagContent.length() == 0)
  {
    // แท็กยังว่าง/ไม่ได้เขียน TAG_xxx → ใช้ UID แทน เพื่อให้เห็นว่าอ่านแท็กได้
    // (เซิร์ฟเวอร์จะตอบ "not found" เพราะ UID ไม่ใช่ ID โจทย์ — ต้องเขียน TAG_xxx ลงแท็กก่อนเล่นจริง)
    tagContent = uidStr;
    tagContent.replace(" ", "");
    tagContent.toUpperCase();
    Serial.printf("[NFC] แท็กไม่มีข้อความ NDEF ใช้ UID แทน: %s\n", tagContent.c_str());
  }
  else
  {
    Serial.printf("[NFC] Read TAG Content: %s\n", tagContent.c_str());
  }

  if (!wsConnected)
  {
    Serial.println("[WS] ยังไม่เชื่อมต่อเซิร์ฟเวอร์ — ข้าม (ยกแท็กออกแล้วแตะใหม่อีกครั้ง)");
    tagLocked = true;
    return;
  }

  // สร้าง JSON ส่งไปให้ Backend (โปรโตคอลเดิม + uid สำหรับดีบักบนหน้าเครื่องแม่)
  JsonDocument doc;
  doc["action"] = "tag_scanned";
  doc["tag_id"] = tagContent; // เช่น "TAG_013"
  doc["uid"] = uidStr;

  String wsMessage;
  serializeJson(doc, wsMessage);
  bool sent = webSocket.sendTXT(wsMessage);
  Serial.printf("[WS] ส่งไปเซิร์ฟเวอร์: %s (%s)\n", wsMessage.c_str(), sent ? "สำเร็จ" : "ล้มเหลว");

  lastReadTime = millis();
  awaitingWrite = sent; // รอคำสั่ง write_tag ตอบกลับ (ต้องไม่ยกแท็กออกก่อน)
  tagLocked = true;
}
