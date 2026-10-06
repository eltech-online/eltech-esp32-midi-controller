// ElTech-Online ESP32 Knob Controller — four knobs and a rotary encoder that
// show up on your phone, tablet or computer as a wireless (Bluetooth) MIDI
// controller for music apps.
//
// BETA: this sketch compiles but has not been fully tested on hardware yet.
//
// Two techniques in one kit:
//   - CD74HC4067 multiplexer -> MULTIPLEXING: reading many analog parts
//     through ONE analog pin, by switching between them very quickly
//   - Bluetooth Low Energy   -> the board announces itself as a standard
//     BLE-MIDI device, so music apps use it without any driver
//
// MIDI is the language music gear has used since 1983. A knob sends "Control
// Change" (CC) messages: three numbers saying which channel, which control
// (0-127) and its new value (0-127). The app decides what each control does.
//
// Libraries needed: none to install. BLE is built into the ESP32 board package.
// Board package: esp32 by Espressif Systems
// This kit does not use WiFi.
//
// How to use:
//   1. Flash this sketch. Serial Monitor prints every message it sends, so you
//      can test the knobs before connecting anything.
//   2. Connect from your device. It appears as "ElTech Knobs":
//        iPhone/iPad: in GarageBand (Settings > Advanced > Bluetooth MIDI
//                     Devices) or any app with a Bluetooth MIDI screen
//        Mac:         Audio MIDI Setup > Window > Show MIDI Studio > Bluetooth
//      It does NOT appear in the normal Bluetooth settings list.
//   3. In your music app, use "MIDI learn" to tie each knob to a control.
// Full source, wiring diagram and setup guide: github.com/eltech-online/eltech-esp32-midi-controller
//
// ---------------------------------------------------------------------------
// New to Arduino code? How to read this file
// ---------------------------------------------------------------------------
// Lines starting with // are comments: notes for people, ignored by the board.
// The file is in this order, and you can read it top to bottom:
//   1. Settings       - pin numbers and MIDI numbers you can safely change
//   2. Multiplexer    - choosing a channel and reading its knob
//   3. Bluetooth MIDI - announcing the device and sending a message
//   4. Encoder        - the fifth, endless knob
//   5. setup()        - runs ONCE when the board is powered on
//   6. loop()         - then runs over and over, forever
// A good first experiment: change the numbers in KNOB_CC below and upload.

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

// ---- Pins ----
#define MUX_S0  4    // multiplexer S0
#define MUX_S1  5    // multiplexer S1
#define MUX_S2  6    // multiplexer S2
#define MUX_S3  7    // multiplexer S3
#define MUX_SIG 3    // multiplexer SIG. Must be an analog pin (GPIO 0-4 on the ESP32-C3)
#define ENC_CLK 10   // encoder CLK
#define ENC_DT  20   // encoder DT
#define ENC_SW  21   // encoder SW (the push button)
#define LED_PIN 8    // the blue LED already on the board: lit while a device is connected

// ---- MIDI numbers ----
const char* DEVICE_NAME = "ElTech Knobs";   // the name your phone or computer shows
const int MIDI_CHANNEL = 1;                 // 1-16

// How many knobs are on the multiplexer, and which MIDI control each one is.
// These four are standard ones most apps already understand:
//   1 = modulation, 7 = volume, 10 = pan, 74 = filter cutoff ("brightness")
// The multiplexer has 16 channels, so you can add up to 12 more knobs later:
// raise KNOB_COUNT and add their numbers here.
const int KNOB_COUNT = 4;
const int KNOB_CC[KNOB_COUNT] = { 1, 7, 10, 74 };
const int ENCODER_CC = 20;   // the encoder's own value, 0-127
const int BUTTON_CC  = 21;   // the encoder's push button: 127 = on, 0 = off (it toggles)

// ---- Knob range ----
// Each knob is wired between 3.3 V and GND, so its middle pin gives 0-3300 mV.
// The ESP32-C3's converter only measures accurately up to about 2500 mV, so
// the last part of each knob's travel would do nothing. Setting "full" a
// little below that makes the knob reach 127 before it gets there.
const int KNOB_MIN_MV = 60;
const int KNOB_MAX_MV = 2450;

// Turning the encoder the "wrong" way round? Change false to true.
const bool ENC_REVERSE = false;

// ---------------------------------------------------------------------------
// Multiplexer
// ---------------------------------------------------------------------------
// The CD74HC4067 is a 16-way rotary switch worked by electricity instead of by
// hand. Its SIG pin is connected to ONE of its 16 channel pins (C0-C15), and
// the four S pins choose which. Together S3 S2 S1 S0 spell the channel number
// in binary:
//     channel 0 = 0000     channel 1 = 0001     channel 2 = 0010
//     channel 3 = 0011     ...                  channel 15 = 1111
// So with 4 output pins and 1 analog pin, the board can read 16 knobs.

// Connects the multiplexer's SIG pin to this channel (0-15).
void selectMuxChannel(int channel) {
  // "channel & 1" is the lowest binary digit of the number; ">> 1" slides the
  // digits one place right so the next one can be picked off the same way.
  digitalWrite(MUX_S0, (channel >> 0) & 1);
  digitalWrite(MUX_S1, (channel >> 1) & 1);
  digitalWrite(MUX_S2, (channel >> 2) & 1);
  digitalWrite(MUX_S3, (channel >> 3) & 1);
  delayMicroseconds(200);   // give the switch and the pin a moment to settle
}

// Reads one knob and returns its voltage in millivolts. Four readings are
// averaged, because a single one jumps around a little.
int readKnobMillivolts(int channel) {
  selectMuxChannel(channel);
  long total = 0;
  for (int i = 0; i < 4; i++) total += analogReadMilliVolts(MUX_SIG);
  return total / 4;
}

// What each knob last sent, and a smoothed copy of its voltage.
int knobValue[KNOB_COUNT];        // 0-127, or -1 before the first reading
float knobSmoothMv[KNOB_COUNT];

void sendControlChange(int control, int value);   // written further down

// Reads every knob and sends a MIDI message for each one that moved.
void readKnobs() {
  for (int i = 0; i < KNOB_COUNT; i++) {
    int mv = readKnobMillivolts(i);

    // Smoothing: move the stored value a quarter of the way towards the new
    // reading each time. Sudden jitter is flattened; a real turn still gets
    // through within a few readings.
    knobSmoothMv[i] += (mv - knobSmoothMv[i]) * 0.25;

    // Scale the voltage to MIDI's 0-127 range.
    int value = map((long)knobSmoothMv[i], KNOB_MIN_MV, KNOB_MAX_MV, 0, 127);
    value = constrain(value, 0, 127);

    // A knob resting exactly on the border between two values would flicker
    // between them forever. So a change of 1 is only accepted when the voltage
    // is clearly inside the new value's band (this is called "hysteresis").
    float mvPerStep = (KNOB_MAX_MV - KNOB_MIN_MV) / 127.0;
    float centreOfLast = KNOB_MIN_MV + (knobValue[i] + 0.5) * mvPerStep;
    bool movedEnough = fabs(knobSmoothMv[i] - centreOfLast) > mvPerStep * 0.8;

    if (knobValue[i] < 0 || (value != knobValue[i] && movedEnough)) {
      knobValue[i] = value;
      sendControlChange(KNOB_CC[i], value);
    }
  }
}

// ---------------------------------------------------------------------------
// Bluetooth MIDI
// ---------------------------------------------------------------------------
// A Bluetooth Low Energy device offers "services", and each service has
// "characteristics": named slots that data goes in and out of. Every one has a
// long ID number. BLE-MIDI is simply an agreement that a MIDI device offers a
// service with THIS ID, containing one characteristic with THAT ID. Any app
// that knows the agreement can then use any such device.
#define MIDI_SERVICE_UUID        "03b80e5a-ede8-4b33-a751-6ce34ec4c700"
#define MIDI_CHARACTERISTIC_UUID "7772e5db-3868-4112-a1a9-f2669d106bf3"

BLECharacteristic* midiCharacteristic = NULL;
bool deviceConnected = false;

// The Bluetooth code calls these two functions by itself when a phone or
// computer connects or disconnects.
class ConnectionEvents : public BLEServerCallbacks {
  void onConnect(BLEServer* server) {
    deviceConnected = true;
    Serial.println("Bluetooth: device connected");
  }
  void onDisconnect(BLEServer* server) {
    deviceConnected = false;
    Serial.println("Bluetooth: device disconnected");
    server->startAdvertising();   // become visible again for the next connection
  }
};

// Sets up the MIDI service and starts "advertising": broadcasting the device's
// name a few times a second so that apps can find it.
void startBluetoothMidi() {
  BLEDevice::init(DEVICE_NAME);
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ConnectionEvents());

  BLEService* service = server->createService(MIDI_SERVICE_UUID);
  midiCharacteristic = service->createCharacteristic(
    MIDI_CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE_NR | BLECharacteristic::PROPERTY_NOTIFY);
  // PROPERTY_NOTIFY is what lets the board push messages to the other side
  // the moment a knob moves, instead of waiting to be asked.
  service->start();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(MIDI_SERVICE_UUID);   // "I am a MIDI device"
  advertising->setScanResponse(true);
  BLEDevice::startAdvertising();
}

// Sends one Control Change message: "control number `control` is now `value`".
//
// A BLE-MIDI packet is 5 bytes:
//   byte 0, 1 = a timestamp in milliseconds, split over two bytes, each with
//               its top bit set (that is what "| 0x80" does)
//   byte 2    = 0xB0 + channel: "this is a Control Change on that channel"
//   byte 3    = which control, 0-127
//   byte 4    = its new value, 0-127
void sendControlChange(int control, int value) {
  Serial.printf("CC %3d = %3d%s\n", control, value, deviceConnected ? "" : "   (nothing connected)");
  if (!deviceConnected) return;

  unsigned long t = millis();
  uint8_t packet[5];
  packet[0] = 0x80 | ((t >> 7) & 0x3F);
  packet[1] = 0x80 | (t & 0x7F);
  packet[2] = 0xB0 | ((MIDI_CHANNEL - 1) & 0x0F);
  packet[3] = control & 0x7F;
  packet[4] = value & 0x7F;
  midiCharacteristic->setValue(packet, 5);
  midiCharacteristic->notify();   // push it to the connected device
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------
// Unlike the four knobs, the encoder has no end stops and no "position": it
// only reports clicks, clockwise or anticlockwise. Inside are two switches, CLK
// and DT; when CLK falls, DT is still high for one direction and already low
// for the other. An INTERRUPT counts the clicks so none are missed.
// "volatile" tells the compiler the value can change at any moment.
volatile int encoderSteps = 0;
volatile unsigned long lastEncoderUs = 0;

void IRAM_ATTR encoderTurned() {
  unsigned long now = micros();
  if (now - lastEncoderUs < 1500) return;   // ignore contact bounce
  lastEncoderUs = now;
  bool clockwise = digitalRead(ENC_DT) == HIGH;
  if (ENC_REVERSE) clockwise = !clockwise;
  encoderSteps += clockwise ? 1 : -1;
}

int encoderValue = 64;      // starts in the middle of 0-127
bool buttonOn = false;

void readEncoder() {
  // Take the count and reset it, with interrupts paused for those two lines so
  // a click can't land in between and get lost.
  noInterrupts();
  int steps = encoderSteps;
  encoderSteps = 0;
  interrupts();

  if (steps != 0) {
    int newValue = constrain(encoderValue + steps * 2, 0, 127);   // 2 per click
    if (newValue != encoderValue) {
      encoderValue = newValue;
      sendControlChange(ENCODER_CC, encoderValue);
    }
  }

  // The button toggles: press once for on (127), again for off (0).
  // ("static" makes these keep their values between runs of loop().)
  static bool wasDown = false;
  static unsigned long lastChangeMs = 0;
  bool down = digitalRead(ENC_SW) == LOW;   // the button connects SW to GND
  if (down != wasDown && millis() - lastChangeMs > 30) {   // 30 ms: ignore contact bounce
    lastChangeMs = millis();
    wasDown = down;
    if (down) {
      buttonOn = !buttonOn;
      sendControlChange(BUTTON_CC, buttonOn ? 127 : 0);
    }
  }
}

// ---------------------------------------------------------------------------
// setup() runs once at power-on, loop() then runs forever
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  pinMode(MUX_S0, OUTPUT);
  pinMode(MUX_S1, OUTPUT);
  pinMode(MUX_S2, OUTPUT);
  pinMode(MUX_S3, OUTPUT);
  analogSetPinAttenuation(MUX_SIG, ADC_11db);   // the ESP32-C3's widest measuring range
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);                  // the LED is "active low": HIGH = off

  // Is the encoder connected? Its module pulls CLK and DT up to 3.3 V. With
  // the ESP32's own weak pull-DOWN switched on, the pins read HIGH only if the
  // module is really there.
  pinMode(ENC_CLK, INPUT_PULLDOWN);
  pinMode(ENC_DT, INPUT_PULLDOWN);
  delay(10);
  bool encoderOK = digitalRead(ENC_CLK) == HIGH && digitalRead(ENC_DT) == HIGH;

  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);   // the button has no pull-up of its own
  attachInterrupt(digitalPinToInterrupt(ENC_CLK), encoderTurned, FALLING);

  Serial.println("========================================");
  Serial.println("           ElTech-Online");
  Serial.println("   ESP32 Knob Controller (BETA)");
  Serial.println("========================================");
  Serial.println("--- Self-test ---");
  // The knobs can't be "found", only read. Each should print a voltage that
  // changes when you turn it; an unconnected channel gives a random drifting one.
  for (int i = 0; i < KNOB_COUNT; i++) {
    int mv = readKnobMillivolts(i);
    knobSmoothMv[i] = mv;
    knobValue[i] = -1;             // -1 = "nothing sent yet", so the first reading is sent
    Serial.printf("Knob %d (C%d):  %4d mV (turn it and check the CC messages below)\n", i + 1, i, mv);
  }
  Serial.print("Encoder:       "); Serial.println(encoderOK ? "OK" : "NOT FOUND");

  startBluetoothMidi();
  Serial.println("Bluetooth:     advertising as \"" + String(DEVICE_NAME) + "\"");
  Serial.print("RESULT:        "); Serial.println(encoderOK ? "PASS" : "FAIL");
}

void loop() {
  // Read the knobs 100 times a second. millis() is the number of milliseconds
  // since the board started; we note when we last read and only read again
  // once 10 ms have gone by. ("static" makes lastRead keep its value between
  // runs of loop().)
  static unsigned long lastRead = 0;
  if (millis() - lastRead >= 10) {
    lastRead = millis();
    readKnobs();
    readEncoder();
    digitalWrite(LED_PIN, deviceConnected ? LOW : HIGH);   // LOW = LED on
  }
}
