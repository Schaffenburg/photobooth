#include <Adafruit_NeoPixel.h>
#define NEO_STRIP_PORT 12
#define NEO_RING_PORT 13
#define STRIP_LEDS 10
#define RING_LEDS 32
#define COUNTDOWN 6
Adafruit_NeoPixel strip = Adafruit_NeoPixel(STRIP_LEDS, NEO_STRIP_PORT, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel ring = Adafruit_NeoPixel(RING_LEDS, NEO_RING_PORT, NEO_GRB + NEO_KHZ800);
int e = 0;
int incomingByte = 0;
int black = 0;

boolean cancel = false;

uint32_t rainbow_lut[6];

uint32_t red = strip.Color(10, 0, 0);
uint32_t yellow = strip.Color(10, 10, 0);
uint32_t green = strip.Color(0, 10, 0);
uint32_t blue = strip.Color(0, 0, 10);

uint32_t purple = strip.Color(32, 0, 32);

uint32_t cyan = strip.Color(0, 32, 32);
uint32_t white = strip.Color(32, 32, 32);

uint32_t bright = strip.Color(255, 255, 255);

void setup() {
  strip.begin();
  strip.show(); // Initialize all pixels to 'off'

  ring.begin();
  ring.show(); // Initialize all pixels to 'off'

  Serial.begin(115200);

  rainbow_lut [0] = strip.Color(128, 0, 0); // red
  rainbow_lut [1] = strip.Color(128,40, 0); // orange
  rainbow_lut [2] = strip.Color(64, 64, 0); // yellow
  rainbow_lut [3] = strip.Color(0, 128, 0); // green
  rainbow_lut [4] = strip.Color(0, 0, 128); // blue
  rainbow_lut [5] = strip.Color(32, 0, 56); // purple

/*
  rainbow_lut [0] = strip.Color(255, 0, 0); // red
  rainbow_lut [1] = strip.Color(255, 80, 0); // orange
  rainbow_lut [2] = strip.Color(128, 128, 0); // yellow
  rainbow_lut [3] = strip.Color(0, 255, 0); // green
  rainbow_lut [4] = strip.Color(0, 0, 255); // blue
  rainbow_lut [5] = strip.Color(64, 0, 110); // purple
*/
  delay(5);
  Serial.write("Photobooth-LED ready\r\n");
}

void stripclear() {
  for (e = 0; e < STRIP_LEDS; e++) {
    strip.setPixelColor(e, black);
  }
  strip.show();
}

void ringclear() {
  for (e = 0; e < RING_LEDS; e++) {
    ring.setPixelColor(e, black);
  }
  ring.show();
}

void stripincrement() {
  for (e = 0; e < STRIP_LEDS; e++) {
    for (int pos = 0; pos < STRIP_LEDS; pos++) {
      if (pos == e) {
        strip.setPixelColor(pos, black);
      }
    }
    strip.show();
    delay(5);
  }
}

void stripon() {
  for (e = 0; e < STRIP_LEDS; e++) {
    if (e % 5 == 0) {
      strip.setPixelColor(e, purple);
    }
    if (e % 5 == 1) {
      strip.setPixelColor(e, red);
    }
    if (e % 5 == 2) {
      strip.setPixelColor(e, yellow);
    }
    if (e % 5 == 3) {
      strip.setPixelColor(e, green);
    }
    if (e % 5 == 4) {
      strip.setPixelColor(e, cyan);
    }
    if (e % 5 == 4) {
      strip.setPixelColor(e, blue);
    }
    //strip.setPixelColor(e,white);
  }
  strip.show();
}

void focususer() {
  ringclear();
  uint8_t x;
  uint8_t delay_per_led = (1000 / RING_LEDS) - 2;
  float leds_per_second = (float) RING_LEDS / (float) COUNTDOWN;
  uint32_t prev_color;

/*
  Serial.print("delay_per_led:");
  Serial.print(delay_per_led);
  Serial.print(" leds_per_second");
  Serial.print(leds_per_second, 4);
*/

  for (x = 0; x < COUNTDOWN; x++)
  {
    uint8_t start_sector_pos = (uint8_t) ((float)x*leds_per_second);
    uint8_t end_sector_pos = (uint8_t) (((float)x+1.0)*leds_per_second);
    
    for (e = start_sector_pos; e < end_sector_pos; e++)
    {
      ring.setPixelColor(e, rainbow_lut [x]);
    }
    ring.show();
    
    for (e = 0; e < RING_LEDS; e++)
    {
      prev_color = ring.getPixelColor(e);
      ring.setPixelColor(e, bright);
      ring.show();
      cancelableDelay (delay_per_led);
      ring.setPixelColor(e, prev_color);
      ring.show();
      if (cancel)
        goto do_cancel;
    }
  }
  Serial.write("done\r\n");
  return;
do_cancel:
  cancelled();
}

void cancelled() {
  Serial.write("cancelled\r\n");
  cancel = false; 
}

void strip_print(uint8_t copies) {
  stripclear();
  uint8_t print_lut[6][3] = {
    {  0, 200, 255}, // cyan
    {255,   0, 220}, // magenta
    {255, 255,   0}, // yellow
    {255, 255, 255} // white
  };
  
  for (uint8_t i = 1; i <= copies; i++)
  {
    Serial.print("\r\ncopy:");
    Serial.print(i);

    uint8_t max_passes = 8;

    for (uint8_t pass = 1; pass <= max_passes; pass++)
    {
      Serial.print("\r\npass:");
      Serial.print(pass);

      uint8_t start_intensity, end_intensity;
      int8_t increment;
  
      if (pass % 2)
      {
        start_intensity = 0;
        end_intensity = 255;
        increment = 1;
      }
      else
      {
        start_intensity = 255;
        end_intensity = 0;
        increment = -1;
      }
      for (uint8_t intensity = start_intensity; intensity != end_intensity; intensity += increment)
      {
        uint8_t ca[3];
        for (uint8_t i=0; i<3; i++)
        {
          ca[i] = print_lut[(pass-1)/2][i];
/*          Serial.print("\r\nprint_lut[");
          Serial.print(pass/2);
          Serial.print("][");
          Serial.print(i);
          Serial.print("]=");
          Serial.print(ca[i]);*/
          ca[i] = (uint8_t)((float)ca[i]*intensity/255);
//          Serial.print("\tca=");
//          Serial.print(ca[i]);
        }
        uint32_t color = strip.Color(ca[0], ca[1], ca[2]);
        for (e = 0; e < STRIP_LEDS; e++)
        {
          strip.setPixelColor(e, color);
        }
        strip.show();
        cancelableDelay(9);
        if (cancel)
          goto do_cancel;
      }
    }
  }
  for (e = 0; e < RING_LEDS; e++) {
    strip.setPixelColor(e, black);
  }
  strip.show();
  Serial.write("done\r\n");
  return;

do_cancel:
  cancelled();
}

void flash_color() {
/*  for (e = 0; e < RING_LEDS; e++) {
    ring.setPixelColor(e, bright);
  }
  ring.show();
  delay(30);
*/
  for (uint8_t i = 0; i < 26; i++)
  {
    for (e = 0; e < RING_LEDS; e++) {
      ring.setPixelColor(e, rainbow_lut [(e+i) % 6]);
    }
    ring.show();
    cancelableDelay(50);
    if (cancel)
      goto do_cancel;
  }
  delay(30);

  for (e = 0; e < RING_LEDS; e++) {
    ring.setPixelColor(e, black);
  }

  ring.show();
  Serial.write("done\r\n");
  return;

do_cancel:
  cancelled();
}

void flash_white() {
  for (e = 0; e < RING_LEDS; e++) {
    ring.setPixelColor(e, white);
  }
  ring.show();
  delay(30);
  for (e = 0; e < RING_LEDS; e++) {
    ring.setPixelColor(e, black);
  }
  ring.show();
}

void cmdparse() {
  incomingByte = Serial.read();
  Serial.write("R");
  if (incomingByte == 'f') {
    flash_color();
  }
  if (incomingByte == 'c') {
    focususer();
  }
  if (incomingByte == 'o') {
    stripon();
  }
  if (incomingByte == 'b') {
    ringclear();
  }
  if (incomingByte == 't') {
    ring.setPixelColor(0, ring.Color(10,10,10));
    ring.show();
  }

  if (incomingByte == 'p') {
    int copies = Serial.parseInt();
    strip_print(copies);
  }
  if (incomingByte == 'B') {
    stripclear();
  }
}

void loop() {
  if (Serial.available() > 0) {
    cmdparse();
  }
}

void cancelableDelay(long duration)
{
  long time = millis();
  while (!cancel)
  {
    if (millis()-time > duration)
    {
      return;
    }
    if (Serial.available() > 0)
    {
      cancel = true;
    }
  }
}
