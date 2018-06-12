/*
   AltitudeEstimation.ino : Arduino sketch to perform altitude estimation using
   the provided library
 */

#include <Arduino.h>
#include <ArduinoTransfer.h>
#include <Wire.h>
// Assuming the IMU is an MPU9250 and thr baro a MS5637
#include <MPU9250.h>
#include <MS5637.h>

#include "altitude.hpp"

// helper variables and functions for obtaining baro data
const uint8_t HISTORY_SIZE = 48;

float   groundAltitude = 0;
float   groundPressure = 0;
float   pressureSum = 0;
float   history[HISTORY_SIZE];
uint8_t historyIdx = 0;
int     endCalibration = 150;

MS5637 barometer = MS5637();

// Pressure in millibars to altitude in meters. We assume
// millibars are the units of the pressure readings from the sensor
float millibarsToMeters(float pa)
{
    // see: https://www.weather.gov/media/epz/wxcalc/pressureAltitude.pdf
    return (1.0f - powf(pa / 1013.25f, 0.190295f)) * 44330.0f;
}

// Calibrate the baro by setting the average measured pressure as the
// ground pressure and the corresponding altitude as the ground altitude.
void calibrate(float pressure)
{
    // Update pressure history
    history[historyIdx] = pressure;
    pressureSum += pressure;
    // cycle the index throught the history array
    uint8_t nextIndex = (historyIdx + 1) % HISTORY_SIZE;
    // Remove next reading from sum so that pressureSum is kept in sync
    pressureSum -= history[nextIndex];
    historyIdx = nextIndex;
    // groundPressure will stabilize at 8 times the average measured
    // pressure (when groundPressure/8 equals pressureSum/(HISTORY_SIZE-1))
    // This acts as a low pass filter and helps to reduce noise
    groundPressure -= groundPressure / 8;
    groundPressure += pressureSum / (HISTORY_SIZE - 1);
    groundAltitude = millibarsToMeters(groundPressure/8);
}

float getAltitude(float pressure)
{
    return  (millibarsToMeters(pressure) - groundAltitude);
}

// helper variables and functions for obtaining IMU data
// Sensor full-scale settings
const Ascale_t ASCALE = AFS_8G;
const Gscale_t GSCALE = GFS_2000DPS;
const Mscale_t MSCALE = MFS_16BITS;
const Mmode_t MMODE = M_100Hz;
// SAMPLE_RATE_DIVISOR: (1 + SAMPLE_RATE_DIVISOR) is a simple divisor of the fundamental 1000 kHz rate of the gyro and accel, so
// SAMPLE_RATE_DIVISOR = 0 means 1 kHz sample rate for both accel and gyro, 4 means 200 Hz, etc.
const uint8_t SAMPLE_RATE_DIVISOR = 0;
// MPU9250 add-on board has interrupt on Butterfly pin 8
const uint8_t INTERRUPT_PIN = 8;
// Create byte-transfer objects for Arduino I^2C
ArduinoI2C mpu = ArduinoI2C(MPU9250::MPU9250_ADDRESS);
ArduinoI2C mag = ArduinoI2C(MPU9250::AK8963_ADDRESS);

// Use the MPU9250 in pass-through mode
MPU9250Passthru imu = MPU9250Passthru(&mpu, &mag);;
// Store imu data
int16_t imuData[7] = {0,0,0,0,0,0,0};
// For scaling to normal units (accelerometer G's, gyrometer rad/sec, magnetometer mGauss)
float aRes;
float gRes;
float mRes;
// We compute these at startup
float gyroBias[3]  = {0,0,0};
float accelBias[3] = {0,0,0};
// flag for when new data is received
bool gotNewData = false;

static void interruptHandler()
{
    gotNewData = true;
}

// Raw analog-to-digital values converted to radians per second
float adc2rad(int16_t adc, float bias)
{
    return (adc * gRes - bias) * M_PI / 180;
}

void getGyrometerAndAccelerometer(float gyro[3], float accel[3])
{
    if (gotNewData) {

        gotNewData = false;

        if (imu.checkNewAccelGyroData()) {

            imu.readMPU9250Data(imuData);

            // Convert the accleration value into g's
            float ax = imuData[0]*aRes - accelBias[0];  // get actual g value, this depends on scale being set
            float ay = imuData[1]*aRes - accelBias[1];
            float az = imuData[2]*aRes - accelBias[2];

            // Convert the gyro value into degrees per second
            float gx = adc2rad(imuData[4], gyroBias[0]);
            float gy = adc2rad(imuData[5], gyroBias[1]);
            float gz = adc2rad(imuData[6], gyroBias[2]);

            // Copy gyro values back out
            gyro[0] = gx;
            gyro[1] = gy;
            gyro[2] = gz;
            // and acceleration values
            accel[0] = ax;
            accel[1] = ay;
            accel[2] = az;

        } // if (imu.checkNewAccelGyroData())

    } // if gotNewData

}

// Altitude estimator
AltitudeEstimator altitude = AltitudeEstimator(0.2, // sigma Accel
                                               0.2, // sigma Gyro
                                               5,   // sigma Baro
                                               0.5, // ca
                                               0.3);// accelThreshold

void setup(void)
{
  // Set all pressure history entries to 0
  for (uint8_t k = 0; k < HISTORY_SIZE; ++k) {
      history[k] = 0;
  }
  // begin Barometer
  barometer.begin();
  // calibrate barometer
  for (uint8_t k=0; k <= endCalibration; k++) {
      float readPressure;
      barometer.getPressure(& readPressure);
      calibrate(readPressure);
  }
  // Begin serial comms
  Serial.begin(115200);
  // Set up the interrupt pin, it's set as active high, push-pull
  pinMode(INTERRUPT_PIN, INPUT);
  attachInterrupt(INTERRUPT_PIN, interruptHandler, RISING);

  // Start I^2C
  Wire.begin();
  Wire.setClock(400000); // I2C frequency at 400 kHz
  delay(1000);

  // Reset the MPU9250
  imu.resetMPU9250();

  // get sensor resolutions, only need to do this once
  aRes = imu.getAres(ASCALE);
  gRes = imu.getGres(GSCALE);
  mRes = imu.getMres(MSCALE);

  // Calibrate gyro and accelerometers, load biases in bias registers
  imu.calibrateMPU9250(gyroBias, accelBias);

  // Initialize the MPU9250
  imu.initMPU9250(ASCALE, GSCALE, SAMPLE_RATE_DIVISOR);
}

void loop(void)
{
  // get all necessary data
  float pressure;
  barometer.getPressure(& pressure);
  float baroHeight = getAltitude(pressure);
  float timestamp = millis();
  float accelData[3];
  float gyroData[3];
  getGyrometerAndAccelerometer(gyroData, accelData);
  altitude.estimate(accelData, gyroData, baroHeight, timestamp);
  Serial.print(baroHeight);
  Serial.print(",");
  Serial.print(altitude.getAltitude());
  Serial.print(",");
  Serial.print(altitude.getVerticalVelocity());
  Serial.print(",");
  Serial.println(altitude.getVerticalAcceleration());
}
