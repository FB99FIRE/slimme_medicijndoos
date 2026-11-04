#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>

#define LDRPIN 26
#define buzzerPin 14
#define LDR_THRESHOLD 500

// RTOS-objecten, worden in setup() aangemaakt.
SemaphoreHandle_t lcdMutex;            // beschermt het LCD, dat door meerdere tasks wordt gebruikt
SemaphoreHandle_t alarmSemaphore;      // control -> servo/buzzer: de timer is afgelopen
SemaphoreHandle_t sluitDoosSemaphore;  // control -> servo/buzzer: gebruiker drukte op '#'
SemaphoreHandle_t rondeKlaarSemaphore; // servo/buzzer -> control: de ronde is afgerond
QueueHandle_t keyQueue;                // numpad -> control: ingedrukte toetsen

LiquidCrystal_I2C lcd(0x27, 20, 4);
Servo myservo;

// boolean om mee aan te geven dat de LDR licht ziet.
volatile bool lichtGezien = true;

// ---------------------------------------------------------------
// Toestanden van het systeem. De huidige toestand is een lokale variabele in
// vControlTask, dus geen enkele andere task kan hem aanpassen.
// ---------------------------------------------------------------
enum class Toestand
{
  Invoer,  // gebruiker typt het aantal seconden
  Wachten, // timer loopt
  Alarm,   // doos is open, buzzer aan, wacht tot de doos dicht is
  Sluiten  // gebruiker drukte op '#': servo sluit de doos
};

char decodeKey(uint8_t code);
void toonScherm(const char *regel0, const char *regel1);
void toonStartscherm();
void toonInvoer(const String &invoer);
bool rondeIsKlaar();
void openDoos();
void sluitDoos();
void vNumpadTask(void *pvParameters);
void vControlTask(void *pvParameters);
void vServoBuzzerTask(void *pvParameters);
void vLDRTask(void *pvParameters);

// ---------------------------------------------------------------
// setup() wordt één keer uitgevoerd bij het opstarten van de ESP32. Hierin worden de RTOS-objecten aangemaakt en de tasks gestart.
// ---------------------------------------------------------------
void setup()
{
  Serial.begin(115200);
  Serial1.begin(9600, SERIAL_8N1, 7, 8); // Grove keypad

  lcdMutex = xSemaphoreCreateMutex();
  alarmSemaphore = xSemaphoreCreateBinary();
  sluitDoosSemaphore = xSemaphoreCreateBinary();
  rondeKlaarSemaphore = xSemaphoreCreateBinary();
  keyQueue = xQueueCreate(10, sizeof(char));

  // Controleer of alles succesvol is aangemaakt
  if (lcdMutex == NULL || alarmSemaphore == NULL || sluitDoosSemaphore == NULL ||
      rondeKlaarSemaphore == NULL || keyQueue == NULL)
  {
    Serial.println("Fout: RTOS-objecten aanmaken mislukt");
    while (true)
      vTaskDelay(pdMS_TO_TICKS(1000));
  }

  lcd.init();
  lcd.backlight();
  toonStartscherm();

  myservo.attach(32);
  pinMode(buzzerPin, OUTPUT);
  // De buzzer gebruikt de tone()-functie, die een timer gebruikt. De ESP32 heeft meerdere timers, dus we kunnen een andere timer gebruiken dan de servo. We kiezen hier voor kanaal 2.
  setToneChannel(2);
  // Prioriteiten:
  // - ServoBuzzer (3): hoogste, moet direct reageren als het alarm afgaat.
  // - Numpad en Control (2): verwerken de invoer van de gebruiker.
  // - LDR (1): laagste, leest alleen de lichtsensor.
  xTaskCreate(vServoBuzzerTask, "ServoBuzzerTask", 4096, NULL, 3, NULL);
  xTaskCreate(vNumpadTask, "NumpadTask", 4096, NULL, 2, NULL);
  xTaskCreate(vControlTask, "ControlTask", 4096, NULL, 2, NULL);
  xTaskCreate(vLDRTask, "LDRTask", 4096, NULL, 1, NULL);
}
// ---------------------------------------------------------------
// loop() doet niets: de taak blokkeert zodat de andere tasks alle CPU-tijd krijgen.
// ---------------------------------------------------------------
void loop()
{
  vTaskDelay(pdMS_TO_TICKS(1000));
}

//---------------------------------------------------------------
// geeft de toets terug die bij de code hoort, of '?' als de code onbekend is.
//---------------------------------------------------------------
char decodeKey(uint8_t code)
{
  switch (code)
  {
  case 0xE1:
    return '1';
  case 0xE2:
    return '2';
  case 0xE3:
    return '3';
  case 0xE4:
    return '4';
  case 0xE5:
    return '5';
  case 0xE6:
    return '6';
  case 0xE7:
    return '7';
  case 0xE8:
    return '8';
  case 0xE9:
    return '9';
  case 0xEA:
    return '*';
  case 0xEB:
    return '0';
  case 0xEC:
    return '#';
  }
  return '?';
}
//---------------------------------------------------------------
// toonScherm() toont de gegeven regels op het LCD-schermpje.
// ---------------------------------------------------------------
// Alle LCD-toegang loopt via toonScherm() en toonInvoer(), zodat de mutex
// altijd wordt gebruikt.
void toonScherm(const char *regel0, const char *regel1)
{
  if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE)
  {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(regel0);
    lcd.setCursor(0, 1);
    lcd.print(regel1);
    xSemaphoreGive(lcdMutex);
  }
}
// ---------------------------------------------------------------
// pakt de mutex, toont de invoerregel op het lcd schermpje en geeft de mutex weer vrij.
// ---------------------------------------------------------------
void toonInvoer(const String &invoer)
{
  if (xSemaphoreTake(lcdMutex, portMAX_DELAY) == pdTRUE)
  {
    lcd.clear();
    lcd.setCursor(0, 1);
    lcd.print("Invoer: " + invoer);
    xSemaphoreGive(lcdMutex);
  }
}
// ---------------------------------------------------------------
// startscherm tonen bij opstarten of na afronden van een ronde.
// ---------------------------------------------------------------
void toonStartscherm()
{
  // In één keer, zodat de regel "Invoer:" niet even ontbreekt.
  toonScherm("MedicijnDoos", "Voer seconden in");
}

// ---------------------------------------------------------------
// Geeft true als de servo/buzzer-task meldt dat de ronde klaar is.
// Alleen de control-task roept deze functie aan.
// ---------------------------------------------------------------
bool rondeIsKlaar()
{
  return xSemaphoreTake(rondeKlaarSemaphore, 0) != pdTRUE;
}

// ---------------------------------------------------------------
// opent de doos.
// ---------------------------------------------------------------
void openDoos()
{
  Serial.println("Alarm! Servo open");
  for (int pos = 0; pos <= 180; pos++)
  {
    vTaskDelay(pdMS_TO_TICKS(10));
    myservo.write(pos);
  }
}

// ---------------------------------------------------------------
// sluit de doos.
// ---------------------------------------------------------------
void sluitDoos()
{
  Serial.println("Doos wordt gesloten");
  for (int pos = 180; pos >= 0; pos--)
  {
    vTaskDelay(pdMS_TO_TICKS(10));
    myservo.write(pos);
  }
}

// ---------------------------------------------------------------
// Numpad-task: leest ALLE beschikbare bytes en zet geldige toetsen
// in de queue. Doet verder niets.
// ---------------------------------------------------------------
void vNumpadTask(void *pvParameters)
{
  while (true)
  {
    while (Serial1.available())
    {
      char key = decodeKey(Serial1.read());
      if (key != '?')
      {
        xQueueSend(keyQueue, &key, 0);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

// ---------------------------------------------------------------
// Control-task: bezit de toestand, de invoer en de timer.
//
//  Invoer  --'#' (invoer >= 1)-->       Wachten
//  Wachten --timer afgelopen-->         Alarm
//  Alarm   --'#'-->                     Sluiten  (servo sluit de doos)
//  Alarm   --doos met de hand dicht-->  Invoer   (ronde klaar)
//  Sluiten --ronde klaar-->             Invoer
// ---------------------------------------------------------------
void vControlTask(void *pvParameters)
{
  Toestand toestand = Toestand::Invoer;
  String invoer = "";
  unsigned long startMillis = 0;
  unsigned long wachtTijd = 0;

  // Ronde is afgerond: scherm en invoer terugzetten voor de volgende ronde.
  auto nieuweRonde = [&]()
  {
    invoer = "";
    toonStartscherm();
    toestand = Toestand::Invoer;
  };

  while (true)
  {
    // Wacht maximaal 30 ms op een toets. Zo reageren we direct op toetsen en
    // wordt de timer toch regelmatig gecontroleerd.
    char key = 0;
    bool toetsOntvangen = (xQueueReceive(keyQueue, &key, pdMS_TO_TICKS(30)) == pdTRUE);

    switch (toestand)
    {
    case Toestand::Invoer:
      if (toetsOntvangen)
      {
        if (key >= '0' && key <= '9')
        {
          if (invoer.length() < 4)
          {
            invoer += key;
          }
        }
        else if (key == '*')
        {
          invoer = ""; // wissen
        }
        else if (key == '#')
        {
          long seconden = invoer.toInt();
          if (seconden >= 1) // minimaal 1 seconde
          {
            wachtTijd = seconden * 1000UL;
            startMillis = millis();
            toonScherm("Timer gestart", "");
            toestand = Toestand::Wachten;
            break; // de invoerregel niet meer bijwerken
          }
        }
        toonInvoer(invoer);
      }
      break;

    case Toestand::Wachten:
      // toetsen worden genegeerd tijdens het wachten
      if (millis() - startMillis >= wachtTijd)
      {
        toestand = Toestand::Alarm;     // eerst de toestand wijzigen...
        xSemaphoreGive(alarmSemaphore); // ...dan pas de servo/buzzer-task wakker maken
      }
      break;

    case Toestand::Alarm:
      if (toetsOntvangen && key == '#')
      {
        toestand = Toestand::Sluiten;
        xSemaphoreGive(sluitDoosSemaphore);
      }
      break;

    case Toestand::Sluiten:
      if (rondeIsKlaar())
      {
        nieuweRonde();
      }
      break;
    }
  }
}

// ---------------------------------------------------------------
// Servo/buzzer-task: voert het hele alarmproces uit, van open tot
// en met buzzer uit, en meldt daarna aan de control-task dat de
// ronde klaar is.
// ---------------------------------------------------------------
void vServoBuzzerTask(void *pvParameters)
{
  while (true)
  {
    // 1. wachten tot de timer is afgelopen
    xSemaphoreTake(alarmSemaphore, portMAX_DELAY);

    // toonScherm("ALARM actief", "Sluit de doos", "Druk op #");
    toonScherm("ALARM actief", "Druk op #");
    openDoos();

    // 2. wachten op '#'. Zodra de LDR licht ziet (de klep is open) gaat de buzzer
    //    aan. Hij blijft aan tot '#' wordt geselecteerd om weer sluiten.
    bool buzzerAan = false;
    while (xSemaphoreTake(sluitDoosSemaphore, pdMS_TO_TICKS(100)) != pdTRUE)
    {
      if (lichtGezien && !buzzerAan)
      {
        tone(buzzerPin, 440);
        buzzerAan = true;
      }
    }

    // 3. '#' ingedrukt: buzzer uit en de servo sluit de doos    noTone(buzzerPin);
    noTone(buzzerPin);
    sluitDoos();

    toonScherm("Ronde klaar", "Voer opnieuw in.");
    vTaskDelay(pdMS_TO_TICKS(2000));

    // 4. melden aan de control-task dat de ronde klaar is
    xSemaphoreGive(rondeKlaarSemaphore);
  }
}

// ---------------------------------------------------------------
// LDR-task: leest alleen de sensor uit en bewaart of de doos dicht is.
// ---------------------------------------------------------------
void vLDRTask(void *pvParameters)
{
  while (true)
  {
    int LDRValue = analogRead(LDRPIN);
    lichtGezien = (LDRValue < LDR_THRESHOLD);
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
