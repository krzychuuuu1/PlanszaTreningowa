# Tablica Treningowa (ESP-NOW)

Prosty projekt tablicy treningowej oparty o ESP32 i komunikację ESP-NOW.

## Co zawiera projekt

- `Pilot_Master/` - sterowanie grami z klawiatury (master)
- `przyciskSlave/` - kod pojedynczego przycisku bezprzewodowego
- `planszaSlave/` - kod planszy z 6 krańcówkami i 6 LED NeoPixel

## Jak to działa (w skrócie)

- Master zbiera urządzenia online i uruchamia tryby gry.
- Przyciski i plansza zgłaszają się przez ESP-NOW.
- W zależności od trybu zapalane są LED i oczekiwane są naciśnięcia.

## Tryby gry (master)

- losowy przycisk
- Simon Says
- ALL-PRESS (wszyscy naraz)

## Uwagi

- Wszystkie moduły powinny pracować na tym samym kanale Wi-Fi ESP-NOW.
- Przed testami sprawdź przypisanie pinów do swojego hardware.
