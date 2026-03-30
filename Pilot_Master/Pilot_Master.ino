/*
  Pilot Master ESP-NOW (klawiatura 4x3)
  - Steruje trybami gry: losowy, Simon Says, ALL-PRESS
  - Zbiera ONLINE/HB_ACK/ACK/BTN i zarzadza lista aktywnych urzadzen
  - Obsluguje WAKE/SLEEP_NOW oraz timeouty i potwierdzenia LED_ACK
*/

#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_sleep.h>
#include <Keypad.h> // Dodana biblioteka do obsługi klawiatury

/* --- USTAWIENIA --- */
#define KANAL_WIFI_ESPNOW 6
#define MAX_BUTTONS 220 // Obsłuż do 220 (miejsca na 6 pól planszy)
#define HEARTBEAT_INTERVAL_MS 1000 // Wysyłaj heartbeat co 5 sekund
#define OFFLINE_TIMEOUT_MS 60000   // Po 60 s bez sygnału uznaj przycisk za offline (wydłużone dla cyklu sleep)
#define TURN_TIMEOUT_MS 5000       // Po 5 s bez BTN uznaj turę za zawieszoną
#define WAKE_BROADCAST_INTERVAL_MS 3000 // Co 3 s pobudka gdy aktywny tryb wybudzania
#define PILOT_SLEEP_INTERVAL_MS 30000    // Po 60 s bezczynności pilot idzie spać w trybie bezczynności
#define POST_GAME_GRACE_MS 15000   // Przez 15 s po grze nie wywalaj przycisków jako offline
#define BOARD_BASE_ID 200          // ID bazowe planszy (sloty 200-205)
#define BOARD_SLOT_COUNT 6

// Lista dostepnych przyciskow aktywnych
bool buttonsOnline[MAX_BUTTONS + 1]; // indeks 0 nieużywany, 1-100 dla przycisków
int onlineButtonCount = 0;
int onlineButtonList[MAX_BUTTONS]; // Lista ID przycisków online
unsigned long lastSeen[MAX_BUTTONS + 1]; // Znacznik czasu ostatniej wiadomości od przycisku

/* --- USTAWIENIA KLAWIATURY 4x3 --- */
// WAŻNE: Klawiatura 4x3 wymaga 7 pinów (4 rzędy + 3 kolumny)
const byte ROWS = 4; // 4 rzędy
const byte COLS = 3; // 3 kolumny

char keys[ROWS][COLS] = {
  {'1','2','3'},
  {'4','5','6'},
  {'7','8','9'},
  {'*','0','#'}
};

// !!! ZMIEŃ TE PINY na te, których używasz !!!
// Poniższe piny to tylko przykład.
byte rowPins[ROWS] = {0,1,2,4}; // 4 piny dla rzędów
byte colPins[COLS] = {5,6,7};  // 3 piny dla kolumn

// Inicjalizacja obiektu klawiatury
Keypad customKeypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

/* --- ZMIENNE GLOBALNE --- */
int aktywnyPrzycisk = -1;
bool graWToku = false;
unsigned long czasOstatniegoNacisniecia = 0;
int licznikNacisniec = 0;

// Flaga do bezpiecznej komunikacji miedzy funkcja odbioru a petla glowna
volatile bool otrzymanoNacisniecie = false;

// Mechanizm ponawiania wysyłki
#define MAX_RETRIES 3
#define ACK_TIMEOUT_MS 1000  // Zwiększone do 1000 ms, aby uwzględnić animacje po naciśnięciu
volatile bool ackReceived = false;
volatile int ackButtonNumber = -1;
int retryCount = 0;
unsigned long lastCommandTime = 0;
bool waitingForAck = false;

// Śledzenie potwierdzeń LED_ACK
bool ledAckReceived[MAX_BUTTONS + 1];
unsigned long ledAckWaitStart = 0;

// --- SIMON SAYS ---
bool simonSaysMode = false;
bool simonShortMode = false; // Tryb krótki (3 poziomy)
int simonPattern[50]; // Wzorzec do 50 poziomów
int simonLevel = 0; // Aktualny poziom
int simonPlayerIndex = 0; // Indeks gracza w sekwencji
bool simonShowingPattern = false;
volatile int simonPlayerInput = -1; // Przycisk naciśnięty przez gracza

// Tryb ALL-PRESS (wszystkie muszą zostać naciśnięte)
bool allPressMode = false;
bool allPressDone[MAX_BUTTONS + 1];
int allPressRemaining = 0;

// Plansza (6 krancówek) – włączana klawiszem '9'
bool planszaEnabled = true; // plansza domyślnie włączona

inline bool isBoardId(int id) {
  return id >= BOARD_BASE_ID && id < BOARD_BASE_ID + BOARD_SLOT_COUNT;
}

inline bool isButtonPlayable(int id) {
  return !isBoardId(id) || planszaEnabled;
}

uint8_t adresRozgloszeniowy[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Tryb pobudki przycisków
bool wakeBroadcastEnabled = true;
unsigned long lastWakeBroadcast = 0;
unsigned long lastPilotActivity = 0; // do automatycznego usypiania pilota
unsigned long lastGameEndTime = 0;    // do okna ochronnego po grze

/* --- FUNKCJA ODWOLANIA: WYSLANIE --- */
void PoDanychWyslanych(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  (void)info;
  (void)status;
}
// Pewne wysyłanie LED z ACK i ponawianiem
bool wlaczLEDReliable(int numerPrzycisku, int r, int g, int b, int maxRetries = MAX_RETRIES) {
  if (numerPrzycisku <= 0 || numerPrzycisku > MAX_BUTTONS) return false;
  if (!buttonsOnline[numerPrzycisku]) return false;
  
  // Wyczyść znacznik ACK raz przed wszystkimi próbami
  ledAckReceived[numerPrzycisku] = false;
  
  for (int attempt = 0; attempt < maxRetries; attempt++) {
    // Wyślij komendę LED
    char dane[32];
    snprintf(dane, sizeof(dane), "LED_ON%d,%d,%d,%d", numerPrzycisku, r, g, b);
    esp_now_send(adresRozgloszeniowy, (uint8_t *)dane, strlen(dane));
    delay(10); // Krótkie opóźnienie, aby ramka zdążyła wyjść
    
    // Czekaj na ACK
    unsigned long startWait = millis();
    while (millis() - startWait < ACK_TIMEOUT_MS) {
      if (ledAckReceived[numerPrzycisku]) {
        lastSeen[numerPrzycisku] = millis();
        return true; // Sukces
      }
      delay(10);
    }
    
    // Przekroczenie czasu - ponów (nie czyść flagi, ACK może przyjść z opóźnieniem)
    if (attempt < maxRetries - 1) {
      Serial.printf("⚠️ Retry LED dla #%d (próba %d/%d)\n", numerPrzycisku, attempt + 2, maxRetries);
      delay(50);
      // Sprawdź jeszcze raz przed ponowieniem, na wypadek spóźnionego ACK
      if (ledAckReceived[numerPrzycisku]) {
        lastSeen[numerPrzycisku] = millis();
        return true;
      }
    }
  }
  
  Serial.printf("❌ LED dla #%d nie potwierdzone po %d próbach\n", numerPrzycisku, maxRetries);
  return false;
}

// Grupowe, pewne wysyłanie LED: najpierw wyślij do wszystkich, potem czekaj na ACK
void wlaczLEDReliableBatch(int* buttons, int count, int r, int g, int b) {
  if (count == 0) return;
  
  // Zresetuj znaczniki ACK
  for (int i = 0; i < count; i++) {
    if (buttons[i] > 0 && buttons[i] <= MAX_BUTTONS) {
      ledAckReceived[buttons[i]] = false;
    }
  }
  
  // Wyślij do wszystkich przycisków jednocześnie
  char dane[32];
  for (int i = 0; i < count; i++) {
    if (buttons[i] > 0 && buttons[i] <= MAX_BUTTONS && buttonsOnline[buttons[i]]) {
      snprintf(dane, sizeof(dane), "LED_ON%d,%d,%d,%d", buttons[i], r, g, b);
      esp_now_send(adresRozgloszeniowy, (uint8_t *)dane, strlen(dane));
      delay(5); // Mały odstęp między wysyłkami
    }
  }
  
  // Czekaj na wszystkie ACK
  unsigned long startWait = millis();
  int remainingCount = count;
  
  while (millis() - startWait < ACK_TIMEOUT_MS && remainingCount > 0) {
    remainingCount = 0;
    for (int i = 0; i < count; i++) {
      if (buttons[i] > 0 && buttons[i] <= MAX_BUTTONS && buttonsOnline[buttons[i]]) {
        if (!ledAckReceived[buttons[i]]) {
          remainingCount++;
        }
      }
    }
    if (remainingCount > 0) delay(10);
  }
  
  // Ponów dla brakujących ACK
  if (remainingCount > 0) {
    Serial.printf("⚠️ %d przyciski bez ACK, retry...\n", remainingCount);
    for (int i = 0; i < count; i++) {
      if (buttons[i] > 0 && buttons[i] <= MAX_BUTTONS && buttonsOnline[buttons[i]]) {
        if (!ledAckReceived[buttons[i]]) {
          wlaczLEDReliable(buttons[i], r, g, b, 2); // Szybkie 2 ponowienia dla opóźnionych odpowiedzi
        }
      }
    }
  }
}

/* --- FUNKCJA ODWOLANIA: ODBIOR --- */
void PoDanychOdebranych(const esp_now_recv_info_t *info, const uint8_t *danePrzychodzace, int dlugosc) {
  char buforWiadomosci[dlugosc + 1];
  memcpy(buforWiadomosci, danePrzychodzace, dlugosc);
  buforWiadomosci[dlugosc] = '\0';

  Serial.print("Odebrano od ");
  if (info && info->src_addr) {
    for (int i = 0; i < 6; i++) {
      if (i) Serial.print(":");
      Serial.printf("%02X", info->src_addr[i]);
    }
  }
  Serial.print(" → ");
  Serial.println(buforWiadomosci);

  // Sprawdz, czy otrzymano ONLINE
  if (strncmp(buforWiadomosci, "ONLINE", 6) == 0) {
    int buttonID = atoi(buforWiadomosci + 6);
    if (buttonID > 0 && buttonID <= MAX_BUTTONS) {
      if (buttonID >= BOARD_BASE_ID && buttonID < BOARD_BASE_ID + BOARD_SLOT_COUNT) {
        unsigned long now = millis();
        int slotId = buttonID;
        bool nowySlot = !buttonsOnline[slotId];
        planszaEnabled = true; // auto-włącz planszę gdy zgłosi się slot
        lastSeen[slotId] = now;
        if (nowySlot) {
          buttonsOnline[slotId] = true;
          onlineButtonList[onlineButtonCount] = slotId;
          onlineButtonCount++;
          Serial.printf("Plansza slot #%d ONLINE (razem: %d)\n", slotId, onlineButtonCount);
        }
        char potwierdzenie[32];
        snprintf(potwierdzenie, sizeof(potwierdzenie), "ONLINE_OK%d", slotId);
        esp_now_send(adresRozgloszeniowy, (uint8_t *)potwierdzenie, strlen(potwierdzenie));
        if (nowySlot) {
          Serial.printf("Wyslano potwierdzenie ONLINE_OK%d\n", slotId);
        }
      } else {
        bool nowyPrzycisk = !buttonsOnline[buttonID];
        lastSeen[buttonID] = millis();

        if (nowyPrzycisk) {
          buttonsOnline[buttonID] = true;
          onlineButtonList[onlineButtonCount] = buttonID;
          onlineButtonCount++;
          Serial.printf("Przycisk #%d jest ONLINE (razem: %d przyciskow)\n", buttonID, onlineButtonCount);
        }

        char potwierdzenie[32];
        snprintf(potwierdzenie, sizeof(potwierdzenie), "ONLINE_OK%d", buttonID);
        esp_now_send(adresRozgloszeniowy, (uint8_t *)potwierdzenie, strlen(potwierdzenie));

        if (nowyPrzycisk) {
          Serial.printf("Wyslano potwierdzenie ONLINE_OK%d\n", buttonID);
        }
      }
    }
    return;
  }

  // Potwierdzenie sygnalu zycia (ACK) z przycisku
  if (strncmp(buforWiadomosci, "HB_ACK", 6) == 0) {
    int buttonID = atoi(buforWiadomosci + 6);
    if (buttonID > 0 && buttonID <= MAX_BUTTONS) {
      unsigned long now = millis();
      lastSeen[buttonID] = now;
      if (!buttonsOnline[buttonID]) {
        buttonsOnline[buttonID] = true;
        onlineButtonList[onlineButtonCount] = buttonID;
        onlineButtonCount++;
        Serial.printf("🔄 Przywrócono ONLINE z HB_ACK #%d (razem: %d)\n", buttonID, onlineButtonCount);
      }
    }
    return;
  }

  // LED_ACK - potwierdzenie otrzymania komendy LED
  if (strncmp(buforWiadomosci, "LED_ACK", 7) == 0) {
    int btn = atoi(buforWiadomosci + 7);
    if (btn > 0 && btn <= MAX_BUTTONS) {
      lastSeen[btn] = millis();
      ledAckReceived[btn] = true;
      Serial.printf("💡 LED_ACK otrzymany od przycisku #%d\n", btn);
    }
    return;
  }

  // Sprawdź czy otrzymano ACK (z deduplikacją szybkich duplikatów)
  if (strncmp(buforWiadomosci, "ACK", 3) == 0) {
    static unsigned long lastAckTs = 0;
    static int lastAckBtn = -1;
    int btn = atoi(buforWiadomosci + 3);
    unsigned long now = millis();
    lastSeen[btn] = now;
    if (btn == lastAckBtn && (now - lastAckTs) < 300) {
      return; // ignoruj natychmiastowe duplikaty
    }
    lastAckBtn = btn;
    lastAckTs = now;

    ackButtonNumber = btn;
    ackReceived = true;
    
    // Zapisz LED_ACK dla funkcji pewnego wysyłania LED
    if (btn > 0 && btn <= MAX_BUTTONS) {
      ledAckReceived[btn] = true;
    }
    
    Serial.printf("✅ ACK otrzymany od przycisku #%d\n", ackButtonNumber);
    return;
  }

  // ALL-PRESS: przechwytywanie BTN
  if (allPressMode && strncmp(buforWiadomosci, "BTN", 3) == 0) {
    int btnId = atoi(buforWiadomosci + 3);
    lastSeen[btnId] = millis();
    if (btnId > 0 && btnId <= MAX_BUTTONS && !allPressDone[btnId]) {
      allPressDone[btnId] = true;
      if (allPressRemaining > 0) {
        allPressRemaining--;
      }
      Serial.printf("✅ ALL-PRESS: #%d wciśnięty, pozostało: %d\n", btnId, allPressRemaining);
      // Nie zmieniaj tu koloru LED - przycisk sam obsługuje potwierdzenie naciśnięcia (zielone migniecie)
      // Natychmiastowa zmiana powoduje problemy czasowe z LED_ACK
      if (allPressRemaining <= 0) {
        Serial.println("🎉 ALL-PRESS ukończony!");
        allPressMode = false;
        wylaczWszystkieLED();
        recordGameEnd();
        esp_now_send(adresRozgloszeniowy, (uint8_t *)"STOP", 4);
      }
    }
    return;
  }

  // Simon Says: przechwytywanie numeru przycisku
  if (simonSaysMode && !simonShowingPattern && strncmp(buforWiadomosci, "BTN", 3) == 0) {
    simonPlayerInput = atoi(buforWiadomosci + 3);
    lastSeen[simonPlayerInput] = millis();
    return;
  }

  // Sprawdzamy tylko, czy gra jest w toku i czy to poprawna wiadomość
  if (strcmp(buforWiadomosci, "PRZYCISK_NACISNIETY") == 0 && graWToku) {
    licznikNacisniec++;
    unsigned long teraz = millis();
    if (aktywnyPrzycisk > 0 && aktywnyPrzycisk <= MAX_BUTTONS) {
      lastSeen[aktywnyPrzycisk] = teraz;
    }
    if (czasOstatniegoNacisniecia > 0) {
      unsigned long roznica = teraz - czasOstatniegoNacisniecia;
      Serial.printf("🕒 Czas od poprzedniego naciśnięcia: %.2f s\n", roznica / 1000.0);
    } else {
      Serial.println("🕒 Pierwsze naciśnięcie!");
    }
    czasOstatniegoNacisniecia = teraz;

    Serial.printf("🔢 Liczba naciśnięć: %d\n", licznikNacisniec);

    // Ustawiamy flage dla glownej petli programu
    otrzymanoNacisniecie = true; 
  }
}

/* --- FUNKCJE --- */
void wyswietlDostepnePrzyciski() {
  Serial.printf("\n📊 Dostępne przyciski (%d):\n", onlineButtonCount);
  if (onlineButtonCount == 0) {
    Serial.println("  Brak przycisków online!");
  } else {
    Serial.print("  ID: ");
    for (int i = 0; i < onlineButtonCount; i++) {
      Serial.printf("%d", onlineButtonList[i]);
      if (i < onlineButtonCount - 1) Serial.print(", ");
    }
    Serial.println();
  }
  Serial.println();
}

int getRandomOnlineButton(int excludeButton = -1) {
  if (onlineButtonCount == 0) {
    return -1;
  }
  
  // Zbuduj listę dostępnych przycisków (bez wykluczonych)
  int available[MAX_BUTTONS];
  int availableCount = 0;
  
  for (int i = 0; i < onlineButtonCount; i++) {
    if (onlineButtonList[i] != excludeButton) {
      if (!isButtonPlayable(onlineButtonList[i])) continue; // pomiń planszę gdy wyłączona
      available[availableCount++] = onlineButtonList[i];
    }
  }
  
  if (availableCount == 0) {
    return -1;
  }
  
  // Wybierz losowy przycisk z puli
  return available[random(0, availableCount)];
}

// Paleta 15 predefiniowanych kolorów (bez szarości/czerni/bieli)
const int ALL_PRESS_COLORS[15][3] = {
  {255, 0, 0},    // czerwony
  {0, 255, 0},    // zielony
  {0, 0, 255},    // niebieski
  {255, 255, 0},  // zolty
  {255, 0, 255},  // magenta
  {0, 255, 255},  // cyjan
  {255, 128, 0},  // pomaranczowy
  {128, 0, 255},  // fioletowy
  {0, 255, 128},  // zielono-mietowy
  {255, 0, 128},  // rozowy
  {0, 128, 255},  // blekitny
  {128, 255, 0},  // limonkowy
  {255, 64, 0},   // cieply pomaranczowy
  {0, 255, 64},   // mietowy
  {64, 0, 255}    // indygo
};

// Kolor z palety 15 kolorów
void pickRandomColor(int &r, int &g, int &b) {
  int idx = random(0, 15);
  r = ALL_PRESS_COLORS[idx][0];
  g = ALL_PRESS_COLORS[idx][1];
  b = ALL_PRESS_COLORS[idx][2];
}

// LED_ON z potwierdzeniem (używane w ALL-PRESS); zwraca true po ACK
bool wyslijLedAllPressZPotwierdzeniem(int numerPrzycisku, int r, int g, int b) {
  char dane[32];
  snprintf(dane, sizeof(dane), "LED_ON%d,%d,%d,%d", numerPrzycisku, r, g, b);

  for (int proba = 0; proba < MAX_RETRIES; proba++) {
    ledAckReceived[numerPrzycisku] = false;

    Serial.printf("📤 ALL-PRESS LED do #%d (próba %d/%d)\n", numerPrzycisku, proba + 1, MAX_RETRIES);
    esp_now_send(adresRozgloszeniowy, (uint8_t *)dane, strlen(dane));

    unsigned long startTime = millis();
    while (millis() - startTime < ACK_TIMEOUT_MS) {
      if (ledAckReceived[numerPrzycisku]) {
        Serial.printf("✅ LED potwierdzony przez #%d\n", numerPrzycisku);
        return true;
      }
      delay(10);
    }

    Serial.printf("⏱️ Brak ACK LED od #%d\n", numerPrzycisku);
  }

  Serial.printf("❌ LED do #%d niepotwierdzony po %d próbach\n", numerPrzycisku, MAX_RETRIES);
  return false;
}

void startAllPressGame() {
  // Zbuduj listę dostępnych ID z uwzględnieniem przelacznika planszy
  int playableIds[MAX_BUTTONS];
  int playableCount = 0;
  for (int i = 0; i < onlineButtonCount; i++) {
    int id = onlineButtonList[i];
    if (isButtonPlayable(id)) {
      playableIds[playableCount++] = id;
    }
  }

  if (playableCount == 0) {
    Serial.println("❌ Brak dostępnych przycisków - nie można rozpocząć ALL-PRESS");
    return;
  }

  graWToku = false;
  simonSaysMode = false;
  allPressMode = true;
  allPressRemaining = playableCount;
  for (int i = 0; i <= MAX_BUTTONS; i++) {
    allPressDone[i] = false;
  }

  Serial.printf("🚦 ALL-PRESS start: %d przycisków\n", playableCount);

  // Wylosuj permutację 15 kolorów, aby pierwsze 15 bylo unikalne
  int paletteOrder[15];
  for (int i = 0; i < 15; i++) {
    paletteOrder[i] = i;
  }
  // Tasowanie Fishera-Yatesa
  for (int i = 14; i > 0; i--) {
    int j = random(0, i + 1);
    int tmp = paletteOrder[i];
    paletteOrder[i] = paletteOrder[j];
    paletteOrder[j] = tmp;
  }

  // Wyślij kolor do każdego online (LED_ON{id},r,g,b) z potwierdzeniem;
  // jeśli brak ACK po 3 próbach, traktuj jak "już wciśnięty", by nie blokować gry
  for (int i = 0; i < playableCount; i++) {
    int id = playableIds[i];
    int r, g, b;
    int idx = paletteOrder[i % 15]; // przy >15 przyciskach kolory zaczna sie powtarzac
    r = ALL_PRESS_COLORS[idx][0];
    g = ALL_PRESS_COLORS[idx][1];
    b = ALL_PRESS_COLORS[idx][2];
    if (wyslijLedAllPressZPotwierdzeniem(id, r, g, b)) {
      // OK, czekamy na BTN w ALL-PRESS
    } else {
      Serial.printf("⚠️ ALL-PRESS: #%d brak ACK LED — uznaję za wciśnięty, by gra mogła trwać\n", id);
      allPressDone[id] = true;
      if (allPressRemaining > 0) {
        allPressRemaining--;
      }
    }
  }

  // Jeśli wszystkie odpadły z powodu braku ACK, zakończ od razu
  if (allPressRemaining <= 0) {
    Serial.println("🎉 ALL-PRESS ukończony (wszystkie bez ACK)");
    allPressMode = false;
    wylaczWszystkieLED();
    recordGameEnd();
    esp_now_send(adresRozgloszeniowy, (uint8_t *)"STOP", 4);
  } else {
    Serial.printf("ALL-PRESS aktywny, oczekuję na %d naciśnięć\n", allPressRemaining);
  }
}

bool wyslijZPotwierdzeniem(int numerPrzycisku) {
  char dane[16];
  snprintf(dane, sizeof(dane), "START%d", numerPrzycisku);
  
  for (int proba = 0; proba < MAX_RETRIES; proba++) {
    ackReceived = false;
    ackButtonNumber = -1;
    
    Serial.printf("📤 Wysyłanie komendy do przycisku #%d (próba %d/%d)\n", numerPrzycisku, proba + 1, MAX_RETRIES);
    esp_now_send(adresRozgloszeniowy, (uint8_t *)dane, strlen(dane));
    
    unsigned long startTime = millis();
    while (millis() - startTime < ACK_TIMEOUT_MS) {
      if (ackReceived && ackButtonNumber == numerPrzycisku) {
        Serial.printf("✅ Przycisk #%d potwierdził odbiór\n", numerPrzycisku);
        lastCommandTime = millis(); // start liczenia tury po ACK
        return true;
      }
      delay(10);
    }
    
    Serial.printf("⏱️ Timeout - brak ACK od przycisku #%d\n", numerPrzycisku);
  }
  
  Serial.printf("❌ Przycisk #%d nie odpowiada po %d próbach\n", numerPrzycisku, MAX_RETRIES);
  return false;
}

void aktywujLosowyPrzycisk() {
  if (onlineButtonCount == 0) {
    Serial.println("❌ Brak dostępnych przycisków online - koniec gry");
    graWToku = false;
    return;
  }
  
  // Wybierz losowy przycisk z dostępnych (różny od poprzedniego)
  int nastepny = getRandomOnlineButton(aktywnyPrzycisk);
  
  if (nastepny == -1) {
    Serial.println("❌ Brak dostępnych przycisków - koniec gry");
    graWToku = false;
    recordGameEnd();
    esp_now_send(adresRozgloszeniowy, (uint8_t *)"STOP", 4);
    return;
  }
  
  aktywnyPrzycisk = nastepny;
  Serial.printf("🎯 Próba aktywacji przycisku #%d\n", aktywnyPrzycisk);
  
  if (!wyslijZPotwierdzeniem(aktywnyPrzycisk)) {
    // Przycisk nie odpowiada - oznacz jako nieaktywny
    Serial.printf("⚠️ Przycisk #%d nie odpowiada - usuwam z listy online\n", aktywnyPrzycisk);
    buttonsOnline[aktywnyPrzycisk] = false;
    
    // Usuń z listy
    for (int i = 0; i < onlineButtonCount; i++) {
      if (onlineButtonList[i] == aktywnyPrzycisk) {
        // Przesuń resztę listy
        for (int j = i; j < onlineButtonCount - 1; j++) {
          onlineButtonList[j] = onlineButtonList[j + 1];
        }
        onlineButtonCount--;
        break;
      }
    }
    
    // Sprobuj z innym przyciskiem
    Serial.println("🔄 Próba aktywacji innego przycisku...");
    
    for (int i = 0; i < onlineButtonCount; i++) {
      aktywnyPrzycisk = onlineButtonList[i];
      Serial.printf("🎯 Próba aktywacji przycisku #%d\n", aktywnyPrzycisk);
      
      if (wyslijZPotwierdzeniem(aktywnyPrzycisk)) {
        return; // Sukces
      } else {
        // Ten tez nie dziala, oznacz jako nieaktywny
        buttonsOnline[aktywnyPrzycisk] = false;
      }
    }
    
    // Żaden przycisk nie odpowiada
    Serial.println("💥 BŁĄD: Żaden przycisk nie odpowiada - koniec gry!");
    onlineButtonCount = 0; // Wyczyść listę
    graWToku = false;
    recordGameEnd();
    esp_now_send(adresRozgloszeniowy, (uint8_t *)"STOP", 4);
  }
}

void pruneOfflineButtons() {
  // W trakcie pokazywania wzorca Simona nie usuwaj przycisków
  if (simonShowingPattern) {
    return;
  }

  // Podczas aktywnej gry (losowe, Simon, ALL-PRESS) polegaj na ACK/BTN zamiast przekroczenia czasu
  if (graWToku || simonSaysMode || allPressMode) {
    return;
  }

  // Po zakonczeniu gry daj okno ochronne, aby uniknac falszywych oznaczen nieaktywnosci
  if (lastGameEndTime > 0 && millis() - lastGameEndTime < POST_GAME_GRACE_MS) {
    return;
  }

  unsigned long now = millis();
  bool removedActive = false;
  for (int i = onlineButtonCount - 1; i >= 0; i--) {
    int id = onlineButtonList[i];
    if (lastSeen[id] > 0 && now - lastSeen[id] > OFFLINE_TIMEOUT_MS) {
      Serial.printf("⚠️ Przycisk #%d uznany za offline (brak sygnału)\n", id);
      buttonsOnline[id] = false;
      if (id == aktywnyPrzycisk) {
        removedActive = true;
      }
      // Usuń z listy
      for (int j = i; j < onlineButtonCount - 1; j++) {
        onlineButtonList[j] = onlineButtonList[j + 1];
      }
      onlineButtonCount--;
    }
  }
  if (removedActive && graWToku) {
    Serial.println("⛔ Aktywny przycisk offline - zatrzymuję grę");
    graWToku = false;
    esp_now_send(adresRozgloszeniowy, (uint8_t *)"STOP", 4);
  }
}

void wlaczLED(int numerPrzycisku, int r, int g, int b) {
  char dane[32];
  snprintf(dane, sizeof(dane), "LED_ON%d,%d,%d,%d", numerPrzycisku, r, g, b);
  esp_now_send(adresRozgloszeniowy, (uint8_t *)dane, strlen(dane));
  // Odswiez lastSeen, by przycisk nie wypadl jako nieaktywny podczas dlugich pokazow
  if (numerPrzycisku > 0 && numerPrzycisku <= MAX_BUTTONS) {
    lastSeen[numerPrzycisku] = millis();
  }
  // Komendy LED nie wymagaja ACK - tylko ustawiaja swiatlo
}


void wylaczWszystkieLED() {
  esp_now_send(adresRozgloszeniowy, (uint8_t *)"LED_OFF", 7);
}

inline void recordGameEnd() {
  lastGameEndTime = millis();
}

void simonPokazWzorzec() {
  Serial.println("\n🎮 Pokazywanie wzorca...");
  simonShowingPattern = true;
  
  for (int i = 0; i <= simonLevel; i++) {
    // Odswiez wszystkie aktywne, zeby nie wypadly z listy podczas dlugiego wzorca
    unsigned long now = millis();
    for (int j = 0; j < onlineButtonCount; j++) {
      int id = onlineButtonList[j];
      if (id > 0 && id <= MAX_BUTTONS) {
        lastSeen[id] = now;
      }
    }

    Serial.printf("  [%d] -> Przycisk #%d\n", i + 1, simonPattern[i]);
    wlaczLED(simonPattern[i], 0, 150, 255); // Cyjan dla Simon Says
    delay(800);
    wylaczWszystkieLED();
    delay(400);
  }
  
  simonShowingPattern = false;
  Serial.println("👉 Twoja kolej! Powtórz sekwencję...\n");
}

void simonStartNewGame(bool shortMode = false) {
  if (onlineButtonCount == 0) {
    Serial.println("❌ Brak dostępnych przycisków - nie można rozpocząć gry!");
    return;
  }
  
  if (shortMode) {
    Serial.println("\n🎯 === SIMON SAYS - TRYB KRÓTKI (3 poziomy) ===\n");
  } else {
    Serial.println("\n🎯 === SIMON SAYS - NOWA GRA ===\n");
  }
  simonSaysMode = true;
  simonShortMode = shortMode;
  simonLevel = 0;
  simonPlayerIndex = 0;
  
  // Generuj pierwszy element wzorca z dostępnych przycisków
  simonPattern[0] = getRandomOnlineButton();
  
  delay(1000);
  simonPokazWzorzec();
}

void simonNextLevel() {
  simonLevel++;
  simonPlayerIndex = 0;
  
  Serial.printf("\n✅ Poprawnie! Poziom %d ukończony!\n", simonLevel);
  
  // Zapal wszystkie LED na zielono po ukonczeniu poziomu (tylko aktywne) - z ACK
  wlaczLEDReliableBatch(onlineButtonList, onlineButtonCount, 0, 255, 0);
  delay(500);
  wylaczWszystkieLED();
  delay(300);
  
  // Sprawdź czy tryb krótki został ukończony (3 poziomy = 0,1,2)
  if (simonShortMode && simonLevel >= 3) {
    Serial.println("\n🎉🎉🎉 GRATULACJE! Ukończyłeś tryb krótki! 🎉🎉🎉\n");
    
    // Zielone miganie wszystkich LED - sukces (tylko aktywne) - z ACK
    for (int i = 0; i < 5; i++) {
      wlaczLEDReliableBatch(onlineButtonList, onlineButtonCount, 0, 255, 0);
      delay(200);
      wylaczWszystkieLED();
      delay(200);
    }
    
    // Wyślij SIMON_END aby przyciski wyłączyły simonSaysActive
    esp_now_send(adresRozgloszeniowy, (uint8_t *)"SIMON_END", 9);
    
    simonSaysMode = false;
    simonShortMode = false;
    return;
  }
  
  Serial.printf("🎊 Przechodzisz do poziomu %d\n\n", simonLevel + 1);
  
  // Dodaj nowy element do wzorca z dostępnych przycisków
  simonPattern[simonLevel] = getRandomOnlineButton();
  
  delay(2000);
  simonPokazWzorzec();
}

void simonGameOver() {
  Serial.printf("\n❌ BŁĄD! Gra zakończona.\n");
  Serial.printf("🏆 Osiągnięty poziom: %d\n\n", simonLevel + 1);
  
  // Miganie wszystkich LED na czerwono (tylko aktywne)
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < onlineButtonCount; j++) {
      wlaczLED(onlineButtonList[j], 255, 0, 0); // Czerwony dla błędu
    }
    delay(300);
    wylaczWszystkieLED();
    delay(300);
  }
  
  // Wyślij SIMON_END aby przyciski wyłączyły simonSaysActive
  esp_now_send(adresRozgloszeniowy, (uint8_t *)"SIMON_END", 9);
  
  simonSaysMode = false;
  simonShortMode = false;
}

/* --- KONFIGURACJA --- */
void setup() {
  Serial.begin(115200);
  delay(100);

  Serial.println("=== ESP-NOW MASTER v2.1 (Klawiatura + Poprawka) ===");

  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(KANAL_WIFI_ESPNOW, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ Błąd inicjalizacji ESP-NOW");
    delay(3000);
    ESP.restart();
  }

  esp_now_register_send_cb(PoDanychWyslanych);
  esp_now_register_recv_cb(PoDanychOdebranych);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, adresRozgloszeniowy, 6);
  peer.channel = KANAL_WIFI_ESPNOW;
  peer.encrypt = false;

  if (esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("❌ Nie udało się dodać peera (broadcast)");
    while (true) delay(100);
  }

  randomSeed((uint32_t)esp_random());
  
  // Zainicjalizuj tablice aktywnych przyciskow
  for (int i = 0; i <= MAX_BUTTONS; i++) {
    buttonsOnline[i] = false;
    lastSeen[i] = 0;
  }
  onlineButtonCount = 0;

  Serial.println("Gotowy! Sterowanie z klawiatury:");
  Serial.println("   1 - rozpocznij grę losowych przycisków");
  Serial.println("   2 - rozpocznij SIMON SAYS (nieskończony)");
  Serial.println("   3 - rozpocznij SIMON SAYS (3 poziomy)");
  Serial.println("   4 - wyświetl dostępne przyciski");
  Serial.println("   5 - ALL-PRESS (wszyscy naraz)");
  Serial.println("   7 - uśpij wszystkie przyciski");
  Serial.println("   8 - pobudka (WAKE co 3s)");
  Serial.println("   9 - toggle plansza (6 krancówek)");
  Serial.println("   # - zatrzymaj grę");
  Serial.println("   0 - resetuj statystyki");
  Serial.printf("Kanał: %d\n\n", KANAL_WIFI_ESPNOW);
  Serial.println("🔍 Czekam na przyciski ONLINE...\n");
}

/* --- PETLA GLOWNA --- */
void loop() {
  
  // *** Sprawdzanie klawiatury ***
  char key = customKeypad.getKey();

  if (key) { // Jeśli naciśnięto klawisz
    Serial.print("Naciśnięto klawisz: ");
    Serial.println(key);
  
    // Mapowanie klawiszy na komendy
    switch (key) {
      case '#': // Zatrzymaj
        if (graWToku) {
          graWToku = false;
          Serial.printf("⏹ Gra zatrzymana. Łącznie naciśnięć: %d\n", licznikNacisniec);
          recordGameEnd();
          esp_now_send(adresRozgloszeniowy, (uint8_t *)"STOP", 4);
        } else if (allPressMode) {
          allPressMode = false;
          wylaczWszystkieLED();
          recordGameEnd();
          Serial.println("⏹ ALL-PRESS zatrzymany.");
        } else if (simonSaysMode) {
          Serial.println("⏹ Simon Says zatrzymany.");
          simonSaysMode = false;
          simonShortMode = false;
          wylaczWszystkieLED();
          recordGameEnd();
          esp_now_send(adresRozgloszeniowy, (uint8_t *)"SIMON_END", 9);
        } else {
          Serial.println("Gra nie trwa!");
        }
        break;
  
      case '0': // Reset statystyk
        licznikNacisniec = 0;
        czasOstatniegoNacisniecia = 0;
        Serial.println("🔄 Statystyki zresetowane.");
        break;
  
      case '1': // Start gry losowej
        if (!graWToku && !simonSaysMode) {
          graWToku = true;
          licznikNacisniec = 0;
          czasOstatniegoNacisniecia = 0;
          Serial.println("▶️  Gra losowych przycisków rozpoczęta!");
          aktywujLosowyPrzycisk();
        } else {
          Serial.println("Gra już trwa!");
        }
        break;

      case '5': // ALL-PRESS: zapal wszystkie, czekaj na naciśnięcie wszystkich
        if (!graWToku && !simonSaysMode && !allPressMode) {
          startAllPressGame();
        } else {
          Serial.println("Zatrzymaj najpierw aktualną grę!");
        }
        break;
        
      case '2': // Simon Says (nieskończony)
        if (!graWToku && !simonSaysMode) {
          simonStartNewGame(false);
        } else {
          Serial.println("Zatrzymaj najpierw aktualną grę!");
        }
        break;
        
      case '3': // Simon Says (krótki - 3 poziomy)
        if (!graWToku && !simonSaysMode) {
          simonStartNewGame(true);
        } else {
          Serial.println("Zatrzymaj najpierw aktulną grę!");
        }
        break;
        
      case '4': // Wyświetl dostępne przyciski
        wyswietlDostepnePrzyciski();
        break;

      case '7': // Uśpij wszystkie przyciski (deep sleep)
        wakeBroadcastEnabled = false;
        graWToku = false;
        simonSaysMode = false;
        Serial.println("😴 Wysyłam SLEEP_NOW do wszystkich przycisków");
        esp_now_send(adresRozgloszeniowy, (uint8_t *)"SLEEP_NOW", 9);
        break;

      case '8': // Włącz ciągłe wysyłanie WAKE (bez przełączania)
        wakeBroadcastEnabled = true;
        Serial.println("🚨 Wysyłanie WAKE co 3s (włączone)");
        
        esp_now_send(adresRozgloszeniowy, (uint8_t *)"WAKE", 4); // natychmiastowa pobudka
        lastWakeBroadcast = 0; // wymuś kolejne WAKE w pętli
        lastPilotActivity = millis();
        break;

      case '9': // Przelacz udzial planszy w grach
        planszaEnabled = !planszaEnabled;
        Serial.printf("Plansza %s w puli gier (ID %d-%d)\n", planszaEnabled ? "WLACZONA" : "WYLACZONA", BOARD_BASE_ID, BOARD_BASE_ID + BOARD_SLOT_COUNT - 1);
        break;
    }

    // każda akcja klawisza to aktywność pilota
    lastPilotActivity = millis();
  }

  // *** SIMON SAYS - Weryfikacja wejscia gracza ***
  if (simonSaysMode && !simonShowingPattern && simonPlayerInput != -1) {
    int oczekiwany = simonPattern[simonPlayerIndex];
    int otrzymany = simonPlayerInput;
    simonPlayerInput = -1; // Wyczyść flagę
    
    Serial.printf("Otrzymano: %d, Oczekiwano: %d\n", otrzymany, oczekiwany);
    
    if (otrzymany == oczekiwany) {
      simonPlayerIndex++;
      
      // Czy gracz ukończył cały poziom?
      if (simonPlayerIndex > simonLevel) {
        simonNextLevel();
      } else {
        Serial.printf("✓ Poprawnie! (%d/%d)\n", simonPlayerIndex, simonLevel + 1);
      }
    } else {
      simonGameOver();
    }
  }

  // *** Wysylanie sygnalu zycia do przyciskow (z lista nieaktywnych) ***
  static unsigned long lastHeartbeat = 0;
  if (millis() - lastHeartbeat >= HEARTBEAT_INTERVAL_MS) {
    char hbBuf[200];
    strcpy(hbBuf, "HEARTBEAT");

    // Dolacz liste nieaktywnych (tylko te, ktore byly kiedykolwiek widziane)
    bool first = true;
    strcat(hbBuf, "|OFF:");
    for (int i = 1; i <= MAX_BUTTONS; i++) {
      if (!buttonsOnline[i] && lastSeen[i] > 0) {
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "%s%d", first ? "" : ",", i);
        strcat(hbBuf, tmp);
        first = false;
      }
    }

    esp_now_send(adresRozgloszeniowy, (uint8_t *)hbBuf, strlen(hbBuf));
    lastHeartbeat = millis();
  }

  // *** Wysylanie pobudki, gdy tryb WAKE jest wlaczony ***
  if (wakeBroadcastEnabled && (millis() - lastWakeBroadcast >= WAKE_BROADCAST_INTERVAL_MS)) {
    esp_now_send(adresRozgloszeniowy, (uint8_t *)"WAKE", 4);
    lastWakeBroadcast = millis();
  }

  // *** Automatyczny sen pilota w bezczynnosci (brak WAKE/gier) ***
  static bool pilotSleeping = false;
  if (!wakeBroadcastEnabled && !graWToku && !simonSaysMode) {
    if (!pilotSleeping && (millis() - lastPilotActivity > PILOT_SLEEP_INTERVAL_MS)) {
      Serial.println("😴 Pilot wchodzi w light sleep (wybudzenie klawiaturą)");
      pilotSleeping = true;
      // Wybudzenie: prosta wersja przyjazna Arduino - pin wiersza klawisza '7' (R2) na HIGH
      esp_deep_sleep_enable_gpio_wakeup(1 << (gpio_num_t)rowPins[2], ESP_GPIO_WAKEUP_GPIO_HIGH);
      esp_deep_sleep_start();
      // po wybudzeniu
      lastPilotActivity = millis();
      pilotSleeping = false;
      Serial.println("🌅 Pilot wybudzony z light sleep");
    }
  } else {
    // Aktywny tryb - resetuj licznik bezczynnosci
    lastPilotActivity = millis();
  }

  // *** Usuwanie nieaktywnych przyciskow ***
  pruneOfflineButtons();

  // *** Przekroczenie czasu tury w grze losowej ***
  if (graWToku && !simonSaysMode && aktywnyPrzycisk > 0 && (millis() - lastCommandTime > TURN_TIMEOUT_MS)) {
    Serial.printf("⏱️ Tura przycisku #%d przekroczyła %d ms - koniec gry\n", aktywnyPrzycisk, TURN_TIMEOUT_MS);
    // Najpierw wyślij STOP, żeby przyciski wyszły z aktywnego stanu i mogły odbierać LED komendy
    graWToku = false;
    esp_now_send(adresRozgloszeniowy, (uint8_t *)"STOP", 4);
    delay(100); // Daj czas na przetworzenie STOP
    
    // Migaj wszystkimi aktywnymi (w tym przegranym) na czerwono (3x)
    for (int i = 0; i < 3; i++) {
      if (aktywnyPrzycisk > 0) {
        wlaczLED(aktywnyPrzycisk, 255, 0, 0);
      }
      for (int j = 0; j < onlineButtonCount; j++) {
        wlaczLED(onlineButtonList[j], 255, 0, 0);
      }
      delay(250);
      wylaczWszystkieLED();
      delay(250);
    }
    aktywnyPrzycisk = -1;
    recordGameEnd();
  }

  // *** Logika przeniesiona z funkcji odbioru ***
  // Sprawdza, czy flaga została ustawiona przez funkcję PoDanychOdebranych
  if (otrzymanoNacisniecie) {
    otrzymanoNacisniecie = false; // Zresetuj flagę
    
    // Opóźnienie jest teraz bezpieczne, bo jest w głównej pętli
    delay(300); // mała przerwa by uniknąć powielonych kliknięć
    
    // Aktywuj następny przycisk
    aktywujLosowyPrzycisk();
  }
  
  delay(10); // Krótka pauza, aby ESP32 "złapał oddech"
}