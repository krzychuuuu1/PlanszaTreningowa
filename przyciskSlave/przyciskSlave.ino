/*
  Przycisk ESP-NOW (pojedynczy modul)
  - Automatycznie przypisuje ID na podstawie adresu MAC
  - Obsluguje ONLINE/HB_ACK/ACK/BTN/LED_ACK oraz START/STOP/WAKE/SLEEP_NOW
  - Tryb oszczedzania energii: nasluch WAKE i deep sleep (timer + przycisk)
*/

#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Adafruit_NeoPixel.h>

/* --- Definicje --- */
#define KANAL_WIFI_ESPNOW 6
#define LICZBA_PIKSELI 7
#define PIN_LED 6
#define PIN_PRZYCISK 2
#define PIN_BUZZER 4
#define WAKE_TIMER_US (30000000ULL) // 30 s timera na pobudkę
#define WAKE_LISTEN_MS 8000         // 8 s okno nasłuchu WAKE po wybudzeniu
#define PIN_ONBOARD_LED 10

// Automatyczne przypisanie ID - nie trzeba już ręcznie zmieniać!
int NUMER_MOJEGO_PRZYCISKU = 0;  // Zostanie automatycznie przypisany

/* --- Zmienne globalne --- */
uint8_t adresMastera[6];           // Adres MAC mastera
bool masterZarejestrowany = false; // Czy master jest zarejestrowany
bool mojaKolej = false;            // Czy aktualnie to mój przycisk ma działać
bool simonSaysActive = false;      // Czy tryb Simon Says jest aktywny
bool rejestracjaPotwierdzono = false; // Czy master potwierdził otrzymanie ONLINE
bool firstRegistration = true;     // Czy to pierwsza rejestracja (pokaz zielony)
bool shouldStartFlag = false;      // Flaga do uruchomienia start() w petli glownej
bool shouldSleepFlag = false;      // Flaga do przejscia w deep sleep
bool wakeKeepAwake = false;        // Czy zostajemy obudzeni (otrzymano WAKE)
unsigned long wakeListenDeadline = 0; // Do kiedy czekamy na WAKE po przebudzeniu
bool wokeFromTimer = false;        // Czy wybudził nas timer (pasynwe wstawanie)
bool wokeFromButton = false;       // Czy wybudził nas przycisk (aktywny start)



RTC_DATA_ATTR int bootCount = 0;

// Nadzor polaczenia
#define HEARTBEAT_TIMEOUT_MS 45000 // 45 s bez heartbeat = rozlaczenie (wydluzone, by nie zrywac gry)
unsigned long lastHeartbeatTime = 0;

Adafruit_NeoPixel piksele(LICZBA_PIKSELI, PIN_LED, NEO_GRB + NEO_KHZ800);

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

/* --- Oszczędzanie energii --- */
void konfigurujNiskieZuzycie() {
  // Obniż jasność LED (istotne dla baterii)
  piksele.setBrightness(12); // niższa jasność = dużo mniejszy pobór

  // Wlacz oszczedzanie Wi-Fi (tryb uspienia modemu)
  WiFi.setSleep(true);
  esp_wifi_set_ps(WIFI_PS_MAX_MODEM); // bardziej agresywne oszczędzanie
}

/* --- Funkcje pomocnicze --- */
void przypisIDNaPodstawieMac() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  
  // Uzyj ostatniego bajtu MAC jako ID (1-255)
  // Możesz też użyć innej metody, np. sumy bajtów modulo 6 + 1
  NUMER_MOJEGO_PRZYCISKU = (mac[5] % 99) + 1; // ID od 1 do 100
  
  Serial.printf("📟 Automatycznie przypisany ID: %d (MAC: %02X:%02X:%02X:%02X:%02X:%02X)\n",
                NUMER_MOJEGO_PRZYCISKU,
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void wyslijOnlineDoMastera() {
  char wiadomosc[32];
  snprintf(wiadomosc, sizeof(wiadomosc), "ONLINE%d", NUMER_MOJEGO_PRZYCISKU);
  
  // Wyslij rozgloszeniowo (master jeszcze nie jest zarejestrowany)
  uint8_t broadcastAddr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_err_t wynik = esp_now_send(broadcastAddr, (uint8_t *)wiadomosc, strlen(wiadomosc));
  
  if (wynik == ESP_OK) {
    Serial.printf("📡 Wysłano ONLINE%d - czekam na potwierdzenie...\n", NUMER_MOJEGO_PRZYCISKU);
  } else {
    Serial.printf("⚠️ Błąd wysyłania ONLINE (%d)\n", wynik);
  }
}

void ustawKolorLED(int r, int g, int b) {
  piksele.clear();
  for (int i = 0; i < LICZBA_PIKSELI; i++) {
    piksele.setPixelColor(i, piksele.Color(r, g, b));
  }
  piksele.show();
}

void animacjaLED(int Rp, int Gp, int Bp, int Rk, int Gk, int Bk) {
  const int kroki = 100;
  const int opoznienieMs = 10;

  for (int i = 0; i <= kroki; i++) {
    float t = (float)i / kroki;
    int r = Rp + (Rk - Rp) * t;
    int g = Gp + (Gk - Gp) * t;
    int b = Bp + (Bk - Bp) * t;
    ustawKolorLED(r, g, b);
    delay(opoznienieMs);
  }

  for (int i = kroki; i >= 0; i--) {
    float t = (float)i / kroki;
    int r = Rp + (Rk - Rp) * t;
    int g = Gp + (Gk - Gp) * t;
    int b = Bp + (Bk - Bp) * t;
    ustawKolorLED(r, g, b);
    delay(opoznienieMs);
  }
}

void wejdzWDzikiSen() {
  Serial.println("😴 Przejście w deep sleep (timer + przycisk)");
  mojaKolej = false;
  simonSaysActive = false;
  shouldStartFlag = false;
  shouldSleepFlag = false;
  ustawKolorLED(0, 0, 0);
  noTone(PIN_BUZZER);
  
  // Jeśli przycisk jest wciśnięty, poczekaj na puszczenie, by nie wybudzić natychmiast
  while (digitalRead(PIN_PRZYCISK) == LOW) {
    delay(10);
  }
  delay(50); // stabilizacja

  // Źródła wybudzenia: przycisk (LOW) oraz timer
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_deep_sleep_enable_gpio_wakeup(1ULL << PIN_PRZYCISK, ESP_GPIO_WAKEUP_GPIO_LOW);
  esp_sleep_enable_timer_wakeup(WAKE_TIMER_US);
  digitalWrite(PIN_ONBOARD_LED, HIGH);
  delay(200);
  digitalWrite(PIN_ONBOARD_LED, LOW);
  delay(200);
  digitalWrite(PIN_ONBOARD_LED, HIGH);
  delay(200);
  digitalWrite(PIN_ONBOARD_LED, LOW);
  delay(200);
  esp_deep_sleep_start();
}

/* --- Funkcja wysyłania do mastera --- */
void wyslijDoMastera(const char *wiadomosc) {
  if (!masterZarejestrowany) {
    Serial.println("❌ Master niezarejestrowany — nie można wysłać wiadomości");
    return;
  }

  esp_err_t wynik = esp_now_send(adresMastera, (uint8_t *)wiadomosc, strlen(wiadomosc));

  if (wynik == ESP_OK) {
    Serial.printf("✅ Wysłano wiadomość: %s\n", wiadomosc);
  } else {
    Serial.printf("⚠️ Błąd wysyłania (%d)\n", wynik);
  }
}

/* --- Działanie przycisku --- */
void start() {
  Serial.printf("🟢 MOJA KOLEJ! (Przycisk #%d)\n", NUMER_MOJEGO_PRZYCISKU);
  mojaKolej = true;

  tone(PIN_BUZZER, 2000, 200);
  delay(250);
  tone(PIN_BUZZER, 3000, 200);
  delay(250);
  noTone(PIN_BUZZER);

  unsigned long startTime = millis();
  const unsigned long TIMEOUT_MS = 30000; // 30 sekund limitu czasu

  while (mojaKolej) {
    // Sprawdz limit czasu
    if (millis() - startTime > TIMEOUT_MS) {
      Serial.println("⏱️ TIMEOUT - przycisk nie został naciśnięty");
      mojaKolej = false;
      ustawKolorLED(0, 0, 0);
      return;
    }
    
    if (!mojaKolej) break; // Sprawdź czy gra nie została zatrzymana
    
    ustawKolorLED(0, 0, 150);
    

    if (digitalRead(PIN_PRZYCISK) == LOW) {
      unsigned long startNacisniecia = millis();

      while (digitalRead(PIN_PRZYCISK) == LOW) {
        if (millis() - startNacisniecia >= 15) {
          Serial.println("🔘 PRZYCISK NACIŚNIĘTY!");

          tone(PIN_BUZZER, 2000, 500);
          ustawKolorLED(0, 255, 0);
          delay(500);
          ustawKolorLED(0, 0, 0);
          noTone(PIN_BUZZER);

          // Wysyłamy również numer przycisku dla trybu Simon Says
          // Wyslij BTN bezposrednio rozgloszeniowo (master nie zawsze ma nas jako klienta bezposredniego)
          char btnMsg[16];
          snprintf(btnMsg, sizeof(btnMsg), "BTN%d", NUMER_MOJEGO_PRZYCISKU);
          uint8_t broadcastAddr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
          esp_err_t bres = esp_now_send(broadcastAddr, (uint8_t *)btnMsg, strlen(btnMsg));
          Serial.printf("📤 BTN broadcast (wynik=%d)\n", bres);

          // Dodatkowo wyslij bezposrednio do mastera, jesli zarejestrowany
          if (masterZarejestrowany) {
            wyslijDoMastera("PRZYCISK_NACISNIETY");
            wyslijDoMastera(btnMsg);
          }

          mojaKolej = false;
          return;
        }
      }
    }
    delay(1);
  }
  
  // Upewnij się, że LED jest wyłączony po wyjściu z pętli
  ustawKolorLED(0, 0, 0);
}

/* --- Stop gry --- */
void zatrzymajGre() {
  Serial.println("⛔ Gra zatrzymana");
  mojaKolej = false;
  ustawKolorLED(0, 0, 0);
  noTone(PIN_BUZZER);
}

/* --- Odbiór danych ESP-NOW --- */
void PoDanychOdebranych(const esp_now_recv_info *info, const uint8_t *danePrzychodzace, int dlugosc) {
  const uint8_t *mac = info->src_addr;

  if (!masterZarejestrowany) {
    // Sprawdz, czy to wiadomosc rozgloszeniowa
    bool isBroadcast = true;
    for (int i = 0; i < 6; i++) {
      if (mac[i] != 0xFF) {
        isBroadcast = false;
        break;
      }
    }
    
    // Nie rejestruj wiadomosci rozgloszeniowej jako mastera
    if (!isBroadcast) {
      memcpy(adresMastera, mac, 6);

      esp_now_peer_info_t infoPeera = {};
      memcpy(infoPeera.peer_addr, adresMastera, 6);
      infoPeera.channel = KANAL_WIFI_ESPNOW;
      infoPeera.encrypt = false;

      esp_err_t result = esp_now_add_peer(&infoPeera);
      if (result == ESP_OK || result == ESP_ERR_ESPNOW_EXIST) {
        // Jezeli klient juz istnieje, uznaj mastera za zarejestrowanego
        masterZarejestrowany = true;
        Serial.print("📡 Master zarejestrowany: ");
        for (int i = 0; i < 6; i++) {
          Serial.printf("%02X", adresMastera[i]);
          if (i < 5) Serial.print(":");
        }
        Serial.println();
      } else {
        Serial.printf("⚠️ Błąd rejestracji mastera: %d\n", result);
      }
    }
  }

  char buforWiadomosci[dlugosc + 1];
  memcpy(buforWiadomosci, danePrzychodzace, dlugosc);
  buforWiadomosci[dlugosc] = '\0';
  String wiadomosc = String(buforWiadomosci);

  Serial.print("📥 Odebrano: ");
  Serial.println(wiadomosc);

  // IGNORUJ własne wiadomości (ACK, BTN, ONLINE)
  if (wiadomosc.startsWith("ACK") || wiadomosc.startsWith("BTN") || wiadomosc.startsWith("ONLINE")) {
    int messageID = 0;
    if (wiadomosc.startsWith("ACK")) {
      messageID = atoi(buforWiadomosci + 3);
    } else if (wiadomosc.startsWith("BTN")) {
      messageID = atoi(buforWiadomosci + 3);
    } else if (wiadomosc.startsWith("ONLINE")) {
      messageID = atoi(buforWiadomosci + 6);
    }
    
    if (messageID == NUMER_MOJEGO_PRZYCISKU) {
      // To nasza wlasna wiadomosc - ignoruj
      return;
    }
  }

  // Sprawdz, czy otrzymano sygnal zycia (moze zawierac liste nieaktywnych)
  if (wiadomosc.startsWith("HEARTBEAT")) {
    lastHeartbeatTime = millis();

    // Odeslij potwierdzenie, by master odswiezyl czas ostatniego kontaktu
    char hbAck[16];
    snprintf(hbAck, sizeof(hbAck), "HB_ACK%d", NUMER_MOJEGO_PRZYCISKU);
    uint8_t broadcastAddr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_now_send(broadcastAddr, (uint8_t *)hbAck, strlen(hbAck));

    return;
  }

  if (wiadomosc == "WAKE") {
    // Miekkie wybudzenie - wazne, by nie zasypiac
    lastHeartbeatTime = millis();
    wakeKeepAwake = true;           // otrzymaliśmy WAKE, zostajemy aktywni
    wakeListenDeadline = 0;
    return;
  }

  if (wiadomosc == "SLEEP_NOW") {
    Serial.println("😴 Otrzymano SLEEP_NOW - przygotowanie do deep sleep");
    shouldSleepFlag = true;
    mojaKolej = false;
    simonSaysActive = false;
    return;
  }

  // Sprawdź czy master potwierdził rejestrację
  if (wiadomosc.startsWith("ONLINE_OK")) {
    int potwierdzonyID = wiadomosc.substring(9).toInt();
    if (potwierdzonyID == NUMER_MOJEGO_PRZYCISKU) {
      rejestracjaPotwierdzono = true;
      Serial.printf("✅ Master potwierdził rejestrację przycisku #%d\n", NUMER_MOJEGO_PRZYCISKU);
      // Pokaz zielony tylko przy pierwszej rejestracji lub wybudzeniu przyciskiem
      if (firstRegistration || wokeFromButton) {
        ustawKolorLED(0, 255, 0); // Zielony - sukces rejestracji
        delay(500);
        ustawKolorLED(0, 0, 0);
        firstRegistration = false; // Już nie pierwsza
      }
    }
    return;
  }

  if (wiadomosc == "STOP") {
    zatrzymajGre();
    simonSaysActive = false; // Wyłącz wszystkie tryby
    mojaKolej = false;
    shouldStartFlag = false; // Wyczysc flage, by nie restartowac start() po STOP
    ustawKolorLED(0, 0, 0); // Wyłącz LED
    noTone(PIN_BUZZER);
    // Po zakonczeniu gry pozwol wrocic do cyklu usypiania i nasluchu
    wakeKeepAwake = false;
    wakeListenDeadline = millis() + WAKE_LISTEN_MS;
    return;
  }
  
  if (wiadomosc == "SIMON_END") {
    simonSaysActive = false; // Koniec Simon Says
    mojaKolej = false;
    ustawKolorLED(0, 0, 0);
    noTone(PIN_BUZZER);
    Serial.println("🏁 Simon Says zakończony");
    // Po zakonczeniu gry pozwol wrocic do cyklu usypiania i nasluchu
    wakeKeepAwake = false;
    wakeListenDeadline = millis() + WAKE_LISTEN_MS;
    return;
  }

  // Simon Says: tylko pokazywanie LED bez interakcji
  if (wiadomosc.startsWith("LED_ON")) {
    // Uklad ramki: LED_ON{button},{r},{g},{b}
    int przecinekPos1 = wiadomosc.indexOf(',', 6);
    int docelowyPrzycisk = wiadomosc.substring(6, przecinekPos1).toInt();
    
    if (docelowyPrzycisk == NUMER_MOJEGO_PRZYCISKU) {
      // Odczytaj kolory RGB
      int przecinekPos2 = wiadomosc.indexOf(',', przecinekPos1 + 1);
      int przecinekPos3 = wiadomosc.indexOf(',', przecinekPos2 + 1);
      
      int r = wiadomosc.substring(przecinekPos1 + 1, przecinekPos2).toInt();
      int g = wiadomosc.substring(przecinekPos2 + 1, przecinekPos3).toInt();
      int b = wiadomosc.substring(przecinekPos3 + 1).toInt();
      
      Serial.printf("💡 LED WŁĄCZONY RGB(%d,%d,%d)\n", r, g, b);
      ustawKolorLED(r, g, b);
      tone(PIN_BUZZER, 1500, 200);

      // Wyslij LED_ACK, by master potwierdzil dotarcie komendy LED
      char ackMsg[16];
      snprintf(ackMsg, sizeof(ackMsg), "LED_ACK%d", NUMER_MOJEGO_PRZYCISKU);
      uint8_t broadcastAddr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
      esp_now_send(broadcastAddr, (uint8_t *)ackMsg, strlen(ackMsg));
    }
    simonSaysActive = true; // Aktywuj tryb Simon Says
    return;
  }

  if (wiadomosc == "LED_OFF") {
    ustawKolorLED(0, 0, 0);
    noTone(PIN_BUZZER);
    
    return;
  }

  if (wiadomosc.startsWith("START")) {
    int docelowyPrzycisk = wiadomosc.substring(5).toInt();
    if (docelowyPrzycisk == NUMER_MOJEGO_PRZYCISKU) {
      // Jeśli już czekamy na start lub trwa tura, ignoruj duplikat
      if (shouldStartFlag || mojaKolej) {
        return;
      }
      // Aktywna gra – nie usypiaj w tle
      wakeKeepAwake = true;
      wakeListenDeadline = 0;
      // ACK musi byc wyslany teraz w funkcji odbioru, zanim cokolwiek innego
      char ackMsg[16];
      snprintf(ackMsg, sizeof(ackMsg), "ACK%d", NUMER_MOJEGO_PRZYCISKU);
      
      // Master jest juz zarejestrowany, bo wyslal nam START
      // Wyslij ACK tylko rozgloszeniowo - to najszybsza droga
      uint8_t broadcastAddr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
      esp_err_t result = esp_now_send(broadcastAddr, (uint8_t *)ackMsg, strlen(ackMsg));
      
      Serial.printf("✅ ACK%d -> broadcast (wynik=%d)\n", NUMER_MOJEGO_PRZYCISKU, result);
      
      // Ustaw flage, aby start() wykonal sie w petli glownej
      shouldStartFlag = true;
    } else if (mojaKolej) {
      Serial.println("To już nie moja kolej");
      zatrzymajGre();
    }
  }
}

/* --- KONFIGURACJA --- */
void setup() {
  Serial.begin(115200);
  delay(100);

  // Sprawdź powód wybudzenia i ustaw flagi, by w trybie pasywnego wybudzenia nie migać
  esp_sleep_wakeup_cause_t wakeReason = esp_sleep_get_wakeup_cause();
  wokeFromTimer = (wakeReason == ESP_SLEEP_WAKEUP_TIMER);
  wokeFromButton = (wakeReason == ESP_SLEEP_WAKEUP_EXT0 || wakeReason == ESP_SLEEP_WAKEUP_EXT1);
  print_wakeup_reason();
  bool suppressWakeFeedback = (wokeFromTimer && !wokeFromButton);
  
  wakeKeepAwake = false;
  wakeListenDeadline = millis() + WAKE_LISTEN_MS; // 8 s na usłyszenie WAKE
  pinMode(PIN_ONBOARD_LED, OUTPUT);
  pinMode(PIN_PRZYCISK, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  piksele.begin();
  konfigurujNiskieZuzycie();

  // Jeśli obudzono z timera (okno WAKE), zachowaj ciszę i zgaś LED, by nie migały
  if (suppressWakeFeedback) {
    piksele.clear();
    piksele.show();
    noTone(PIN_BUZZER);
  }

  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(KANAL_WIFI_ESPNOW, WIFI_SECOND_CHAN_NONE);

  // Automatyczne przypisanie ID na podstawie MAC
  przypisIDNaPodstawieMac();

  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ Błąd inicjalizacji ESP-NOW");
    return;
  }

  esp_now_register_recv_cb(PoDanychOdebranych);
  
  // Dodaj klienta rozgloszeniowego do wysylania ONLINE
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, (uint8_t[]){0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, 6);
  peerInfo.channel = KANAL_WIFI_ESPNOW;
  peerInfo.encrypt = false;
  
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("❌ Błąd dodawania broadcast peer");
    return;
  }

  Serial.println("🎮 Gra Przyciskowa ESP-NOW - PRZYCISK");
  Serial.printf("NUMER MOJEGO PRZYCISKU: %d\n", NUMER_MOJEGO_PRZYCISKU);
  Serial.println("Parametry Wi-Fi:");
  Serial.println("  Tryb: STA");
  Serial.print("  Adres MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.printf("  Kanał: %d\n", KANAL_WIFI_ESPNOW);
  Serial.println("\nGotowy! Czekam na start gry...");

  // Pomijamy mrugniecie LED wbudowanym, aby oszczedzac energie
  
  Serial.println("\n🔄 Rozpoczynam rejestrację w masterze...");
}

/* --- PETLA GLOWNA --- */
void loop() {
  // Podczas trwającej gry lub pokazu Simona nie pozwól, by licznik heartbeat się zestarzał
  if (rejestracjaPotwierdzono && (mojaKolej || simonSaysActive)) {
    lastHeartbeatTime = millis();
  }

  // Wywolaj start(), jesli flaga jest ustawiona (z funkcji odbioru)
  if (shouldStartFlag) {
    shouldStartFlag = false;
    // Wymus czyszczenie LED, jesli gra nie jest aktywna
    if (!mojaKolej && !simonSaysActive) {
      ustawKolorLED(0, 0, 0);
    }
    start(); // Teraz bezpiecznie mozemy blokowac w petli glownej
  }

  // Gleboki sen na zadanie (z wiadomosci SLEEP_NOW)
  if (shouldSleepFlag) {
    wejdzWDzikiSen();
  }

  // Po wybudzeniu: jeśli w 10 s nie dostaniemy WAKE, wracamy spać
  if (!wakeKeepAwake && wakeListenDeadline > 0 && millis() > wakeListenDeadline && !mojaKolej && !simonSaysActive) {
    Serial.println("⏲️ Brak WAKE w oknie nasłuchu - wracam do snu");
    wejdzWDzikiSen();
  }
  
  // Sprawdz, czy stracilismy polaczenie z masterem (brak sygnalu zycia)
  if (rejestracjaPotwierdzono && (millis() - lastHeartbeatTime > HEARTBEAT_TIMEOUT_MS)) {
    // Jesli trwa gra lub pokaz, nie zrywaj - daj kolejna szanse na heartbeat
    if (mojaKolej || simonSaysActive) {
      lastHeartbeatTime = millis(); // odłóż reakcję na później
    } else {
      Serial.println("⚠️ Brak heartbeat od mastera - miękka ponowna rejestracja (bez restartu)");
      rejestracjaPotwierdzono = false;
      masterZarejestrowany = false;
      mojaKolej = false;
      simonSaysActive = false;
      ustawKolorLED(255, 100, 0); // Pomarańczowy - rozłączenie
      delay(300);
      ustawKolorLED(0, 0, 0);
      // Teraz petla ponownie wysyla ONLINE, ale bez resetu urzadzenia
    }
  }
  
  // Wysyłaj ONLINE co 2 sekundy aż do potwierdzenia
  static unsigned long ostatnieWyslanieOnline = 0;
  if (!rejestracjaPotwierdzono) {
    if (millis() - ostatnieWyslanieOnline >= 2000) {
      wyslijOnlineDoMastera();
      ostatnieWyslanieOnline = millis();
    }
  }
  
  // Simon Says: monitoruj przycisk gdy tryb aktywny
  if (simonSaysActive && !mojaKolej && digitalRead(PIN_PRZYCISK) == LOW) {
    char ackMsg[16];
      snprintf(ackMsg, sizeof(ackMsg), "ACK%d", NUMER_MOJEGO_PRZYCISKU);
      
      // Wyslij zarowno rozgloszeniowo, jak i do zarejestrowanego mastera
      uint8_t broadcastAddr[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
      esp_now_send(broadcastAddr, (uint8_t *)ackMsg, strlen(ackMsg));
    delay(50); // Eliminacja drgan stykow
    if (digitalRead(PIN_PRZYCISK) == LOW) {
      Serial.printf("🔘 Simon Says: Naciśnięto przycisk #%d\n", NUMER_MOJEGO_PRZYCISKU);
      
      // Wizualna i dźwiękowa informacja zwrotna
      tone(PIN_BUZZER, 2000, 200);
      ustawKolorLED(0, 255, 0); // Zielony - potwierdzenie
      delay(200);
      ustawKolorLED(0, 0, 0); // Wyłącz LED
      
      // Wyślij numer przycisku do mastera
      char buforNumeru[8];
      snprintf(buforNumeru, sizeof(buforNumeru), "BTN%d", NUMER_MOJEGO_PRZYCISKU);
      wyslijDoMastera(buforNumeru);
      
      // Czekaj aż przycisk zostanie puszczony
      while (digitalRead(PIN_PRZYCISK) == LOW) {
        delay(10);
      }
      delay(100); // Dodatkowa eliminacja drgan stykow
    }
  }
  
  delay(10);
}
