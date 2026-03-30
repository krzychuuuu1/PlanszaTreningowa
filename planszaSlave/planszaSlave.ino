/*
   Plansza ESP-NOW (6 krańcówek + 6 NeoPixel)
   - Sloty mają ID 200-205 (BOARD_BASE_ID + index)
   - Każdy slot wysyła ONLINE/HB_ACK/BTN/ACK (dla LED_ON/START)
   - Wybudzanie: timer + WAKE z mastera (brak buzzera)
*/

#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Adafruit_NeoPixel.h>

/* --- Definicje sprzętowe --- */
#define KANAL_WIFI_ESPNOW 6
#define SLOT_COUNT 6
#define LICZBA_PIKSELI SLOT_COUNT
#define PIN_LED 15     // Ustaw na faktyczny pin danych NeoPixel (unikaj 6-11 z flash)
#define PIN_BUZZER -1  // brak buzzera na planszy
#define WAKE_TIMER_US (30000000ULL) // 30 s timera na pobudkę
#define WAKE_LISTEN_MS 8000         // 8 s okno nasłuchu WAKE po wybudzeniu
#define PIN_ONBOARD_LED -1          // ustaw na wbudowaną diodę, jeśli jest
#define BOARD_BASE_ID 200           // ID slotów: 200-205
const int slotPins[SLOT_COUNT] = {13, 12, 14, 27, 26, 33};
const bool SWITCH_ACTIVE_LOW = true; // krańcówki zwierają do GND (własny pull-up 3.3V)

/* --- Zmienne globalne --- */
uint8_t adresMastera[6];
bool masterZarejestrowany = false;
bool simonSaysActive = false;
bool shouldSleepFlag = false;
bool wakeKeepAwake = false;
unsigned long wakeListenDeadline = 0;
bool wokeFromTimer = false;
bool wokeFromButton = false;

bool slotActive[SLOT_COUNT] = {false};
unsigned long slotDeadline[SLOT_COUNT] = {0};
bool slotPressedState[SLOT_COUNT] = {false};
unsigned long slotLedOffDeadline[SLOT_COUNT] = {0};
bool onlineConfirmed[SLOT_COUNT] = {false};
unsigned long slotLastTransition[SLOT_COUNT] = {0}; // Czas do eliminacji drgan stykow
#define DEBOUNCE_MS 50 // Opoznienie eliminacji drgan stykow (ms)
unsigned long lastHeartbeatTime = 0;
int lastShownSlot = -1;

Adafruit_NeoPixel piksele(LICZBA_PIKSELI, PIN_LED, NEO_GRB + NEO_KHZ800);

/* --- Pomocnicze --- */
inline int slotId(int slot) { return BOARD_BASE_ID + slot; }

inline void safeTone(int freq, int durMs) { if (PIN_BUZZER >= 0) tone(PIN_BUZZER, freq, durMs); }
inline void safeNoTone() { if (PIN_BUZZER >= 0) noTone(PIN_BUZZER); }

void print_wakeup_reason() {
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  switch (wakeup_reason) {
    case ESP_SLEEP_WAKEUP_EXT0:     Serial.println("Wakeup caused by external signal using RTC_IO"); break;
    case ESP_SLEEP_WAKEUP_EXT1:     Serial.println("Wakeup caused by external signal using RTC_CNTL"); break;
    case ESP_SLEEP_WAKEUP_TIMER:    Serial.println("Wakeup caused by timer"); break;
    case ESP_SLEEP_WAKEUP_TOUCHPAD: Serial.println("Wakeup caused by touchpad"); break;
    case ESP_SLEEP_WAKEUP_ULP:      Serial.println("Wakeup caused by ULP program"); break;
    default:                        Serial.printf("Wakeup was not caused by deep sleep: %d\n", wakeup_reason); break;
  }
}

void konfigurujNiskieZuzycie() {
  piksele.setBrightness(255); // Jasnosc 100%
  WiFi.setSleep(true);
  esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
}

void clearAllLeds() {
  piksele.clear();
  piksele.show();
}

void setSlotColor(int slot, int r, int g, int b) {
  if (slot < 0 || slot >= SLOT_COUNT) return;
  piksele.setPixelColor(slot, piksele.Color(r, g, b));
  piksele.show();
}

void sendAck(int slot) {
  char ackMsg[16];
  snprintf(ackMsg, sizeof(ackMsg), "ACK%d", slotId(slot));
  uint8_t broadcastAddr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_send(broadcastAddr, (uint8_t *)ackMsg, strlen(ackMsg));
}

void sendLedAck(int slot) {
  char ackMsg[16];
  snprintf(ackMsg, sizeof(ackMsg), "LED_ACK%d", slotId(slot));
  uint8_t broadcastAddr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_send(broadcastAddr, (uint8_t *)ackMsg, strlen(ackMsg));
}

void sendBtn(int slot) {
  char btnMsg[16];
  snprintf(btnMsg, sizeof(btnMsg), "BTN%d", slotId(slot));
  uint8_t broadcastAddr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_send(broadcastAddr, (uint8_t *)btnMsg, strlen(btnMsg));
  if (masterZarejestrowany) {
    esp_now_send(adresMastera, (uint8_t *)btnMsg, strlen(btnMsg));
  }
}

void sendOnline(int slot) {
  char msg[16];
  snprintf(msg, sizeof(msg), "ONLINE%d", slotId(slot));
  uint8_t broadcastAddr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_send(broadcastAddr, (uint8_t *)msg, strlen(msg));
  Serial.printf("ONLINE%d wyslane\n", slotId(slot));
}

void wejdzWDzikiSen() {
  Serial.println("Plansza: deep sleep (timer)");
  clearAllLeds();
  safeNoTone();
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_timer_wakeup(WAKE_TIMER_US);
  esp_deep_sleep_start();
}

/* --- Odbiór danych ESP-NOW --- */
void PoDanychOdebranych(const esp_now_recv_info *info, const uint8_t *danePrzychodzace, int dlugosc) {
  const uint8_t *mac = info->src_addr;

  if (!masterZarejestrowany) {
    bool isBroadcast = true;
    for (int i = 0; i < 6; i++) {
      if (mac[i] != 0xFF) { isBroadcast = false; break; }
    }
    if (!isBroadcast) {
      memcpy(adresMastera, mac, 6);
      esp_now_peer_info_t peerInfo = {};
      memcpy(peerInfo.peer_addr, adresMastera, 6);
      peerInfo.channel = KANAL_WIFI_ESPNOW;
      peerInfo.encrypt = false;
      esp_err_t res = esp_now_add_peer(&peerInfo);
      if (res == ESP_OK || res == ESP_ERR_ESPNOW_EXIST) {
        masterZarejestrowany = true;
        Serial.print("Master zarejestrowany: ");
        for (int i = 0; i < 6; i++) Serial.printf("%02X%s", adresMastera[i], i < 5 ? ":" : "");
        Serial.println();
      } else {
        Serial.printf("Blad rejestracji mastera: %d\n", res);
      }
    }
  }

  char buforWiadomosci[dlugosc + 1];
  memcpy(buforWiadomosci, danePrzychodzace, dlugosc);
  buforWiadomosci[dlugosc] = '\0';
  String wiadomosc = String(buforWiadomosci);

  Serial.print("Odebrano: ");
  Serial.println(wiadomosc);

  if (wiadomosc.startsWith("HEARTBEAT")) {
    lastHeartbeatTime = millis();
    for (int s = 0; s < SLOT_COUNT; s++) {
      char hbAck[16];
      snprintf(hbAck, sizeof(hbAck), "HB_ACK%d", slotId(s));
      uint8_t bc[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
      esp_now_send(bc, (uint8_t *)hbAck, strlen(hbAck));
    }
    return;
  }

  if (wiadomosc == "WAKE") {
    lastHeartbeatTime = millis();
    wakeKeepAwake = true;
    wakeListenDeadline = 0;
    return;
  }

  if (wiadomosc == "SLEEP_NOW") {
    shouldSleepFlag = true;
    simonSaysActive = false;
    return;
  }

  if (wiadomosc.startsWith("ONLINE_OK")) {
    int id = wiadomosc.substring(9).toInt();
    int slot = id - BOARD_BASE_ID;
    if (slot >= 0 && slot < SLOT_COUNT) {
      onlineConfirmed[slot] = true;
      Serial.printf("Master potwierdzil rejestracje slotu %d (ID %d)\n", slot, id);
      setSlotColor(slot, 0, 255, 0);
      delay(120);
      setSlotColor(slot, 0, 0, 0);
    }
    return;
  }

  if (wiadomosc == "LED_OFF") {
    clearAllLeds();
    safeNoTone();
    return;
  }

  if (wiadomosc.startsWith("LED_ON")) {
    int p1 = wiadomosc.indexOf(',', 6);
    int targetId = wiadomosc.substring(6, p1).toInt();
    int slot = targetId - BOARD_BASE_ID;
    if (slot >= 0 && slot < SLOT_COUNT) {
      int p2 = wiadomosc.indexOf(',', p1 + 1);
      int p3 = wiadomosc.indexOf(',', p2 + 1);
      int r = wiadomosc.substring(p1 + 1, p2).toInt();
      int g = wiadomosc.substring(p2 + 1, p3).toInt();
      int b = wiadomosc.substring(p3 + 1).toInt();
      Serial.printf("LED slot %d RGB(%d,%d,%d)\n", slot, r, g, b);
      setSlotColor(slot, r, g, b);
      sendLedAck(slot);
      slotActive[slot] = true; // w trybie Simon traktuj jako aktywny
      slotDeadline[slot] = millis() + 30000UL;
      lastShownSlot = slot;
    }
    simonSaysActive = true;
    return;
  }

  if (wiadomosc.startsWith("START")) {
    int targetId = wiadomosc.substring(5).toInt();
    int slot = targetId - BOARD_BASE_ID;
    if (slot >= 0 && slot < SLOT_COUNT) {
      sendAck(slot);
      slotActive[slot] = true;
      slotDeadline[slot] = millis() + 30000UL;
      wakeKeepAwake = true;
      wakeListenDeadline = 0;
      setSlotColor(slot, 0, 0, 150); // podświetl aktywny slot jak zwykły przycisk
      lastShownSlot = slot;
      Serial.printf("START slot %d (ID %d)\n", slot, targetId);
    }
    return;
  }

  if (wiadomosc == "STOP") {
    simonSaysActive = false;
    for (int s = 0; s < SLOT_COUNT; s++) slotActive[s] = false;
    if (lastShownSlot >= 0) {
      for (int i = 0; i < 3; i++) {
        setSlotColor(lastShownSlot, 255, 0, 0);
        delay(180);
        setSlotColor(lastShownSlot, 0, 0, 0);
        delay(180);
      }
    }
    clearAllLeds();
    wakeKeepAwake = false;
    wakeListenDeadline = millis() + WAKE_LISTEN_MS;
    lastShownSlot = -1;
    return;
  }

  if (wiadomosc == "SIMON_END") {
    simonSaysActive = false;
    clearAllLeds();
    wakeKeepAwake = false;
    wakeListenDeadline = millis() + WAKE_LISTEN_MS;
    lastShownSlot = -1;
    return;
  }
}

/* --- Konfiguracja --- */
void setup() {
  Serial.begin(115200);
  delay(100);

  esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();
  wokeFromTimer = (wakeReason == ESP_SLEEP_WAKEUP_TIMER);
  wokeFromButton = (wakeReason == ESP_SLEEP_WAKEUP_EXT0 || wakeReason == ESP_SLEEP_WAKEUP_EXT1);
  print_wakeup_reason();
  bool suppressWakeFeedback = (wokeFromTimer && !wokeFromButton);

  wakeKeepAwake = false;
  wakeListenDeadline = millis() + WAKE_LISTEN_MS;
  if (PIN_ONBOARD_LED >= 0) pinMode(PIN_ONBOARD_LED, OUTPUT);
  for (int i = 0; i < SLOT_COUNT; i++) {
    pinMode(slotPins[i], INPUT); // zewnętrzne pull-upy na groove
    slotPressedState[i] = false;
  }
  if (PIN_BUZZER >= 0) pinMode(PIN_BUZZER, OUTPUT);
  piksele.begin();
  konfigurujNiskieZuzycie();
  if (suppressWakeFeedback) {
    clearAllLeds();
    safeNoTone();
  }

  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(KANAL_WIFI_ESPNOW, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("Blad inicjalizacji ESP-NOW");
    return;
  }
  esp_now_register_recv_cb(PoDanychOdebranych);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, (uint8_t[]){0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, 6);
  peerInfo.channel = KANAL_WIFI_ESPNOW;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  Serial.println("Plansza ESP-NOW (sloty 200-205)");
  Serial.printf("Parametry Wi-Fi: kanał %d, MAC %s\n", KANAL_WIFI_ESPNOW, WiFi.macAddress().c_str());
  Serial.println("Rejestracja ONLINE dla wszystkich slotów...");
}

/* --- Petla glowna --- */
void loop() {
  static unsigned long lastOnline = 0;
  if (millis() - lastOnline >= 2000) {
    for (int s = 0; s < SLOT_COUNT; s++) {
      if (!onlineConfirmed[s]) sendOnline(s);
    }
    lastOnline = millis();
  }

  if (masterZarejestrowany && lastHeartbeatTime > 0 && (millis() - lastHeartbeatTime > 45000UL)) {
    Serial.println("Brak heartbeat od mastera - miekka ponowna rejestracja");
    masterZarejestrowany = false;
    memset(onlineConfirmed, 0, sizeof(onlineConfirmed));
    lastHeartbeatTime = millis();
  }

  for (int s = 0; s < SLOT_COUNT; s++) {
    bool raw = digitalRead(slotPins[s]);
    // Logika odwrocona: aktywne jest HIGH, gdy krancowka zamknie obwod do GND przy pull-upie
    bool pressed = SWITCH_ACTIVE_LOW ? (raw == HIGH) : (raw == LOW);

    // Zbocze narastajace z eliminacja drgan stykow
    if (pressed && !slotPressedState[s]) {
      if (millis() - slotLastTransition[s] > DEBOUNCE_MS) {
        slotPressedState[s] = true;
        slotLastTransition[s] = millis();
        bool gameplayActive = (slotActive[s] || simonSaysActive);
        if (gameplayActive) {
          Serial.printf("BTN slot %d (ID %d)\n", s, slotId(s));
          sendBtn(s);
          slotActive[s] = false;
          setSlotColor(s, 0, 255, 0);
          slotLedOffDeadline[s] = millis() + 200; // krótki blink
        }
      }
    }
    // Zbocze opadajace z eliminacja drgan stykow
    if (!pressed && slotPressedState[s]) {
      if (millis() - slotLastTransition[s] > DEBOUNCE_MS) {
        slotPressedState[s] = false;
        slotLastTransition[s] = millis();
      }
    }

    if (slotLedOffDeadline[s] > 0 && !slotActive[s] && !simonSaysActive && millis() > slotLedOffDeadline[s]) {
      setSlotColor(s, 0, 0, 0);
      slotLedOffDeadline[s] = 0;
    }

    if (slotActive[s] && millis() > slotDeadline[s]) {
      Serial.printf("Timeout slot %d\n", s);
      slotActive[s] = false;
    }
  }

  if (!wakeKeepAwake && wakeListenDeadline > 0 && millis() > wakeListenDeadline && !simonSaysActive) {
    Serial.println("Brak WAKE w oknie - sleep");
    wejdzWDzikiSen();
  }

  if (shouldSleepFlag) {
    wejdzWDzikiSen();
  }

  delay(5);
}
