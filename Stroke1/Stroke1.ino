#include <NMEAGPS.h>
#include "I2Cdev.h"
#include "MPU6050_6Axis_MotionApps20.h"
#include <SdFat.h>
#include <SPI.h>
#include "Wire.h"
#include <GPSport.h>


MPU6050 mpu;

//gps setup
#define GPSSerial Serial1
static NMEAGPS GPS;
#define GPSECHO false
static gps_fix fix;

//mpu interrupt setup
#define INTERRUPT_PIN 19  // use pin 2 on Arduino Uno & most boards
#define LED_PIN 13 // (Arduino is 13, Teensy is 11, Teensy++ is 6)
bool blinkState = false;

// MPU control/status vars
bool dmpReady = false;  // set true if DMP init was successful
uint8_t mpuIntStatus;   // holds actual interrupt status byte from MPU
uint8_t devStatus;      // return status after each device operation (0 = success, !0 = error)
uint16_t packetSize;    // expected DMP packet size (default is 42 bytes)
uint16_t fifoCount;     // count of all bytes currently in FIFO
uint8_t fifoBuffer[64]; // FIFO storage buffer

// orientation/motion vars
Quaternion q;           // [w, x, y, z]         quaternion container
VectorInt16 aa;         // [x, y, z]            accel sensor measurements
VectorInt16 aaReal;     // [x, y, z]            gravity-free accel sensor measurements
VectorInt16 aaWorld;    // [x, y, z]            world-frame accel sensor measurements
VectorFloat gravity;    // [x, y, z]            gravity vector
float euler[3];         // [psi, theta, phi]    Euler angle container
float ypr[3];           // [yaw, pitch, roll]   yaw/pitch/roll container and gravity vector


String dName;
//SD Setup
int trig = 0;
SdFat sd;

char cinBuf[40];
ArduinoInStream cin(Serial, cinBuf, sizeof(cinBuf));

const int chipSelect = 4;

// ================================================================
// ===               INTERRUPT DETECTION ROUTINE                ===
// ================================================================

volatile bool mpuInterrupt = false;     // indicates whether MPU interrupt pin has gone high
void dmpDataReady() {
  mpuInterrupt = true;
}



// ================================================================
// ===                      INITIAL SETUP                       ===
// ================================================================

void setup() {
  Wire.begin();
  Serial.begin(115200);
  delay(500);
  gpsPort.begin( 57600 );

  delay(500);

  while (!sd.begin(chipSelect)) {
    Serial.println("Card failed, or not present");
    // don't do anything more:
    return;
  }
  
  Serial.println("Card initialized");

  // initialize device
  Serial.println(F("Initializing I2C devices..."));

  mpu.initialize();
  pinMode(INTERRUPT_PIN, INPUT);

  delay(1000);


  // verify connection
  Serial.println(F("Testing device connections..."));
  Serial.println(mpu.testConnection() ? F("MPU6050 connection successful") : F("MPU6050 connection failed"));

  // load and configure the DMP
  Serial.println(F("Initializing DMP..."));
  devStatus = mpu.dmpInitialize();

  // make sure it worked (returns 0 if so)
  if (devStatus == 0) {
    mpu.PrintActiveOffsets();
    // turn on the DMP, now that it's ready
    Serial.println(F("Enabling DMP..."));
    mpu.setDMPEnabled(true);

    attachInterrupt(digitalPinToInterrupt(INTERRUPT_PIN), dmpDataReady, RISING);
    mpuIntStatus = mpu.getIntStatus();

    Serial.println(F("DMP ready! Waiting for first interrupt..."));
    dmpReady = true;

    //1000/1+setRate()
    mpu.setRate(49);

    // get expected DMP packet size for later comparison
    packetSize = mpu.dmpGetFIFOPacketSize();
  } else {
    // ERROR!
    // 1 = initial memory load failed
    // 2 = DMP configuration updates failed
    // (if it's going to break, usually the code will be 1)
    Serial.print(F("DMP Initialization failed (code "));
    Serial.print(devStatus);
    Serial.println(F(")"));
  }



  mpu.setXAccelOffset(-4196);
  mpu.setYAccelOffset(-451);
  mpu.setZAccelOffset(3511);
  mpu.setXGyroOffset(119);
  mpu.setYGyroOffset(53);
  mpu.setZGyroOffset(-10);


  // configure LED for output
  pinMode(LED_PIN, OUTPUT);

}



// ================================================================
// ===                    MAIN PROGRAM LOOP                     ===
// ================================================================

void loop() {
  // if programming failed, don't try to do anything
  if (!dmpReady) return;

  // wait for MPU interrupt or extra packet(s) available
  while (!mpuInterrupt && fifoCount < packetSize) {
    if (mpuInterrupt && fifoCount < packetSize) {
      // try to get out of the infinite loop
      fifoCount = mpu.getFIFOCount();
    }
  }

  // reset interrupt flag and get INT_STATUS byte
  mpuInterrupt = false;
  mpuIntStatus = mpu.getIntStatus();

  // get current FIFO count
  fifoCount = mpu.getFIFOCount();
  if (fifoCount < packetSize) {
    //Lets go back and wait for another interrupt. We shouldn't be here, we got an interrupt from another event
    // This is blocking so don't do it   while (fifoCount < packetSize) fifoCount = mpu.getFIFOCount();
  }

  // check for overflow (this should never happen unless our code is too inefficient)
  else if ((mpuIntStatus & 0x01 << (MPU6050_INTERRUPT_FIFO_OFLOW_BIT)) || fifoCount >= 1024) {
    // reset so we can continue cleanly
    mpu.resetFIFO();
    //  fifoCount = mpu.getFIFOCount();  // will be zero after reset no need to ask
    Serial.println(F("FIFO overflow!"));

    // otherwise, check for DMP data ready interrupt (this should happen frequently)
  } else if (mpuIntStatus & 0x01 << (MPU6050_INTERRUPT_DMP_INT_BIT)) {
    // read a packet from FIFO


    while (fifoCount >= packetSize) { // Lets catch up to NOW, someone is using the dreaded delay()!
      mpu.getFIFOBytes(fifoBuffer, packetSize);
      // track FIFO count here in case there is > 1 packet available
      // (this lets us immediately read more without waiting for an interrupt)
      fifoCount -= packetSize;
    }

    // display initial world-frame acceleration, adjusted to remove gravity
    // and rotated based on known orientation from quaternion
    mpu.dmpGetQuaternion(&q, fifoBuffer);
    mpu.dmpGetAccel(&aa, fifoBuffer);
    mpu.dmpGetGravity(&gravity, &q);
    mpu.dmpGetLinearAccel(&aaReal, &aa, &gravity);
    mpu.dmpGetLinearAccelInWorld(&aaWorld, &aaReal, &q);


    mpu.setIntEnabled(false);

    //prevent any further executution until there is a GPS fix.

    while (GPS.available( gpsPort )) {
      fix = GPS.read();
    }

    if (trig == 0) {
      dName = "";
      dName.concat(fix.dateTime.date);
      dName.concat("-");
      dName.concat(fix.dateTime.hours);
      dName.concat(fix.dateTime.minutes);
      dName.concat(".csv");
      trig = 1;
    }

    Serial.print("Name: ");
    Serial.println(dName);

    Serial.print("Fix status:");
    Serial.print(" ");
    Serial.println(fix.valid.status);

    Serial.print("Location status:");
    Serial.print(" ");
    Serial.println(fix.valid.location);

    Serial.print("trig status:");
    Serial.print(" ");
    Serial.println(trig);


    Serial.print(aaReal.x);
    Serial.print(",");
    Serial.print(aaReal.y);
    Serial.print(",");
    Serial.print(aaReal.z);
    Serial.print(",");
    Serial.print(fix.dateTime.hours);
    Serial.print(",");
    Serial.print(fix.dateTime.minutes);
    Serial.print(",");
    Serial.print(fix.dateTime.seconds);
    Serial.print(",");
    Serial.print(fix.latitude());
    Serial.print(",");
    Serial.println(fix.longitude());

    File dataFile = sd.open(dName, FILE_WRITE);
    dataFile.print(millis());
    dataFile.print(",");
    dataFile.print(aaReal.x);
    dataFile.print(",");
    dataFile.print(aaReal.y);
    dataFile.print(",");
    dataFile.print(aaReal.z);
    dataFile.print(",");
    dataFile.print(fix.dateTime);
    dataFile.print(",");
    dataFile.print(fix.latitudeL());
    dataFile.print(",");
    dataFile.println(fix.longitudeL());
    mpu.resetFIFO();
    if (trig = 1)
    {
      blinkState = !blinkState;
      digitalWrite(LED_PIN, blinkState);
    }
    dataFile.close();



    mpu.setIntEnabled(true);
  }
}
