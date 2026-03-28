/*
WAV Files are to be in the format:Mono, 16-bit PCM encoding with 16kHz sampleRate
*/

#include <I2S.h>
#include <SD.h>
// I2S pins for Arduino Nano ESP32
#define I2S_BCLK 7  // Bit clock
#define I2S_DOUT 8  // Data out
#define I2S_LRC 9   // Left/Right clock (Word Select)

#define I2S_FAIL A2  // I2S-Initialization Fail LED
#define SD_FAIL A1   // SD-Initialization Fail LED
#define SD_CS 10

#define CH1 2   //Channel 1 Trigger Pin
#define CH2 3   //Channel 2 Trigger Pin
#define CH3 4   //Channel 3 Trigger Pin
#define PART 5  //Part Entire Pin

const int sampleRate = 16000;        // sample rate in Hz
const int bps = 16;                  // bits per sample
i2s_mode_t mode = I2S_PHILIPS_MODE;  // I2S decoder is needed

const int playersAmount = 3;
const int MAX_LENGTH = 30000;

File file;  //temporary file used to parse wav data

volatile unsigned long next;  //Time in microseconds when the next sample is set

bool anyPlaying;
int16_t mix;

hw_timer_t* timer = NULL;

byte triggerPins[playersAmount] = { CH1, CH2, CH3 };

struct Player {

  uint16_t length;
  bool playing;
  unsigned int pos;
  bool triggered;

  byte triggerPin;
  int16_t data[MAX_LENGTH];
};

Player players[playersAmount];

void setup() {
  Serial.begin(115200);
  delay(2000);

  pinMode(I2S_FAIL, OUTPUT);
  digitalWrite(I2S_FAIL, LOW);

  pinMode(CH1, INPUT_PULLUP);
  pinMode(CH2, INPUT_PULLUP);
  pinMode(CH3, INPUT_PULLUP);
  pinMode(PART, INPUT_PULLUP);

  I2S.setDataPin(I2S_DOUT);
  I2S.setFsPin(I2S_LRC);
  I2S.setSckPin(I2S_BCLK);

  Serial.println("I2S Pins set");

  if (!I2S.begin(mode, sampleRate, bps)) {
    digitalWrite(I2S_FAIL, HIGH);
    Serial.println("Failed to initialize I2S!");
    while (1)
      ;  // do nothing
  }

  pinMode(SD_FAIL, OUTPUT);
  digitalWrite(SD_FAIL, LOW);

  if (!SD.begin(SD_CS)) {
    Serial.println("Card Mount Failed");
    digitalWrite(SD_FAIL, HIGH);
    return;
  }

  Serial.println("SD card initialized");

  //Initialize Players
  for (int i = 0; i < playersAmount; i++) {
    players[i].pos = 0;
    players[i].playing = false;
    players[i].triggered = false;

    players[i].length = parseFile(i + 1, players[i].data);
    if (players[i].length > MAX_LENGTH) { players[i].length = MAX_LENGTH; }  //Limit the length of the sample

    players[i].triggerPin = triggerPins[i];
  }

  Serial.println("Wave files parsed and saved");


  Serial.println("Setup complete");
}

void loop() {

  //Check if a channel has been triggered
  checkChannelsTriggered();

  //Mix and output audio
  processAudio();

  //Check if a sample has finished playing.
  checkFinished();
}
void checkFinished() {

  for (int i = 0; i < playersAmount; i++) {
    if (players[i].pos >= players[i].length && players[i].playing) {
      Serial.print("Sample ");
      Serial.print(i + 1);
      Serial.println(" Finished");

      players[i].playing = false;
    }
  }
}

void processAudio() {

  anyPlaying = false;
  for (int i = 0; i < playersAmount; i++) {
    if (players[i].playing) {
      anyPlaying = true;  //If just one sample is playing then anyPlaying is true and the rest of the loop can be stopped
      break;
    }
  }

  if (anyPlaying) {
    //At least one player is active, mix the active samples together
    mix = 0;  //Reset mix to re-calculate the current sample

    for (int i = 0; i < playersAmount; i++) {
      if (players[i].playing) {
        mix += players[i].data[players[i].pos];  //Add the current samples together
      }
    }

    //Output the mixed sample to the Right and Left channels of the DAC
    I2S.write(mix);
    I2S.write(mix);

  } else {
    //No sample is playing, output silence
    I2S.write(0);
    I2S.write(0);
  }

  //Set the time for the next sample to be playing
  //TODO: Replace the timing with a hardware timer instead of micros()
  if (micros() > next) {
    for (int i = 0; i < playersAmount; i++) {
      if (players[i].playing) {
        players[i].pos += 1;//Set the new sample for all active players
      }
    }
    next = next + 62;
  }
}

uint16_t parseFile(int number, int16_t array[]) {
  //Write the wav-file data into the sample arrays and return the array length

  byte sizeBytes[4];
  String filename = "/";
  filename.concat(number);
  filename.concat(".wav");

  file = SD.open(filename);

  if (!file) {

    Serial.println("Could not open file");
    return 0;
  } else {
    file.seek(40);  //Skip to the size chunk

    for (int i = 0; i < 4; i++) {
      sizeBytes[i] = file.read();  //read in the 4 size bytes.
    }

    //Calculate the size of the data chunk in bytes
    uint32_t size = 0;
    size = sizeBytes[3];

    for (int i = 0; i < 3; i++) {
      size = size << 8;
      size = size | sizeBytes[2 - i];
    }
    //Calculate the number of samples (2 bytes per sample)
    int samplesAmount = size / 2;
    int toWrite = 0;  //The amount of samples to write in the array (limited by MAX_LENGTH)
    if (samplesAmount > MAX_LENGTH) {
      toWrite = MAX_LENGTH;
    } else {
      toWrite = samplesAmount;
    }

    //Write the samples to the Player arrays (Little Endian encoded! Conversion to Big endian is required)
    byte temp1 = 0;  //Temporary byte for constructing the sample value
    byte temp2 = 0;  //Temporary byte for constructing the sample value
    int16_t tempSample = 0;

    for (int i = 0; i < toWrite; i++) {
      temp1 = file.read();
      temp2 = file.read();

      tempSample = temp2;
      tempSample = tempSample << 8;
      tempSample = tempSample | temp1;

      array[i] = tempSample;
    }
    file.close();
    return samplesAmount;
  }
}

void checkChannelsTriggered() {
  for (int i = 0; i < playersAmount; i++) {

    if (digitalRead(players[i].triggerPin) == LOW && !players[i].triggered) {
      //GATE ON
      players[i].triggered = true;  // Trigger flag set, only plays the sample on the rising edge

      Serial.print("players ");
      Serial.print(i + 1);
      Serial.println(" Triggered");
      players[i].pos = 0;
      players[i].playing = true;
      next = micros() + 100;
    }

    if (digitalRead(players[i].triggerPin) == HIGH && players[i].triggered) {
      //GATE OFF
      players[i].triggered = false;  //Reset Trigger flag
      if (digitalRead(PART) == LOW) {
        players[i].playing = false;
      }
    }
  }
}

void printArray(int16_t array[], uint16_t size) {
  //Debug helper method
  Serial.print("Size: ");
  Serial.println(size);
  for (int i = 0; i < size; i++) {
    Serial.print(array[i]);
    Serial.print(" ");
    if (i % 32 == 0) {
      Serial.println(" ");
    }
  }
}
