#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>

#define LDRPIN 26
#define buzzerPin 14
// #define LDR_THRESHOLD 2000
#define LDR_THRESHOLD 500

LiquidCrystal_I2C lcd(0x27, 20, 4);
Servo myservo;

bool BuzzerAan = false;
bool actieGedaan = false;
bool timerActief = false;
bool rondeKlaar = false;

unsigned long startMillis = 0;
unsigned long wachtTijd = 0;

String invoer = "";
int seconden = 0;

// numpad DECODER FUNCTIE
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

// ---------------------
void tijdKlopt()
{
  Serial.println("Alarm! Servo open + buzzer aan");

  for (int pos = 0; pos <= 180; pos += 1)
  {
    delay(5);
    myservo.write(pos);
  }
  delay(300);
  myservo.write(1);
  delay(1000);

  tone(buzzerPin, 440);
  BuzzerAan = true;
  actieGedaan = true;
}

// ---------------------
void setup()
{
  Serial.begin(115200);
  Serial1.begin(9600, SERIAL_8N1, 7, 8); // Grove keypad

  lcd.init();
  lcd.backlight();

  lcd.setCursor(0, 0);
  lcd.print("MedicijnDoos");
  lcd.setCursor(0, 1);
  lcd.print("Voer seconden in");

  myservo.attach(32);
  pinMode(buzzerPin, OUTPUT);
}

// ---------------------
void loop()
{

  // numpad uitlezen
  if (!timerActief && !rondeKlaar)
  {

    if (Serial1.available())
    {
      uint8_t code = Serial1.read();
      char key = decodeKey(code);
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("Voer seconden in");
      if (key != '?')
      {
        Serial.print("Toets: ");
        Serial.println(key);
      }

      if (key >= '0' && key <= '9')
      {
        if (invoer.length() < 4) // max 4 cijfers
          invoer += key;
      }

      if (key == '*')
      {
        invoer = ""; // wissen
      }

      if (key == '#')
      {
        if (invoer.length() > 0)
        {
          seconden = invoer.toInt();
          wachtTijd = seconden * 1000UL;
          startMillis = millis();
          timerActief = true;

          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("Timer gestart");
        }
      }

      // LCD invoer live updaten
      lcd.setCursor(0, 1);
      lcd.print("Invoer: ");
      lcd.print(invoer + "   ");
    }
  }

  // ldr uitlezen en alarm

  int LDRValue = analogRead(LDRPIN);

  if (timerActief && (millis() - startMillis >= wachtTijd) && !actieGedaan)
  {
    tijdKlopt();
    timerActief = false;

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("ALARM actief");
    lcd.setCursor(0, 1);
    lcd.print("Sluit de doos");
  }

  // doos dicht = buzzer uit en daarna herhaalt de code
  if (actieGedaan && LDRValue < LDR_THRESHOLD)
  {
    noTone(buzzerPin);
    BuzzerAan = false;
    actieGedaan = false;
    rondeKlaar = true;

    actieGedaan = false;
    timerActief = false;
    rondeKlaar = false;
    invoer = "";
    seconden = 0;

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Ronde klaar");
    lcd.setCursor(0, 1);
    lcd.print("Voer opnieuw in");
  }

  delay(30);
}