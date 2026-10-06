// ElTech-Online ESP32 Knob Controller — multiplexer test
//
// BETA: this sketch compiles but has not been fully tested on hardware yet.
//
// The smallest useful sketch for the CD74HC4067 multiplexer: twice a second it
// steps through the first four channels and prints the voltage of the knob on
// each one. No Bluetooth, no encoder. Use it to check the multiplexer and the
// four knobs are wired correctly.
//
// Wiring: as in the full project (see the wiring diagram in the README).
//
// What you should see: four numbers, each going from near 0 to about 3000 as
// you turn that knob. If two numbers move together, two wires are swapped.

#define MUX_S0  4
#define MUX_S1  5
#define MUX_S2  6
#define MUX_S3  7
#define MUX_SIG 3   // must be an analog pin (GPIO 0-4 on the ESP32-C3)

// Connects the multiplexer's SIG pin to one of its 16 channels (0-15).
// The four S pins spell the channel number in binary: channel 5 is 0101, so
// S3 = 0, S2 = 1, S1 = 0, S0 = 1.
void selectMuxChannel(int channel) {
  digitalWrite(MUX_S0, (channel >> 0) & 1);   // the lowest binary digit
  digitalWrite(MUX_S1, (channel >> 1) & 1);   // the next one up...
  digitalWrite(MUX_S2, (channel >> 2) & 1);
  digitalWrite(MUX_S3, (channel >> 3) & 1);
  delayMicroseconds(200);                     // a moment to settle
}

// setup() runs once, when the board is powered on or reset.
void setup() {
  Serial.begin(115200);
  pinMode(MUX_S0, OUTPUT);
  pinMode(MUX_S1, OUTPUT);
  pinMode(MUX_S2, OUTPUT);
  pinMode(MUX_S3, OUTPUT);
  analogSetPinAttenuation(MUX_SIG, ADC_11db);   // the widest measuring range
  Serial.println("Multiplexer test - knob voltages in millivolts (mV)");
}

// loop() runs over and over, forever.
void loop() {
  // "for" repeats the lines inside it, with channel = 0, then 1, 2 and 3.
  for (int channel = 0; channel < 4; channel++) {
    selectMuxChannel(channel);
    int millivolts = analogReadMilliVolts(MUX_SIG);
    Serial.print("C");
    Serial.print(channel);
    Serial.print(": ");
    Serial.print(millivolts);
    Serial.print(" mV   ");
  }
  Serial.println();   // end the line
  delay(500);         // wait half a second, then loop() runs again
}
