/*
  Test.h - Test library for Wiring - description
  Copyright (c) 2006 John Doe.  All right reserved.
*/

// ensure this library description is only included once
#ifndef AirGradient_h
#define AirGradient_h

#include <Print.h>
#include "Stream.h"

// ENUMS STRUCTS FOR CO2 START
struct CO2_READ_RESULT
{
  int co2 = -1;
  bool success = false;
};
// ENUMS STRUCTS FOR CO2 END

class PMS
{
public:
  struct Data
  {
    // Standard Particles, CF=1
    uint16_t PM_SP_UG_1_0;
    uint16_t PM_SP_UG_2_5;
    uint16_t PM_SP_UG_10_0;

    // Atmospheric environment
    uint16_t PM_AE_UG_1_0;
    uint16_t PM_AE_UG_2_5;
    uint16_t PM_AE_UG_10_0;

    // Raw particles count (number of particles in 0.1l of air
    uint16_t PM_RAW_0_3;
    uint16_t PM_RAW_0_5;
    uint16_t PM_RAW_1_0;
    uint16_t PM_RAW_2_5;
    uint16_t PM_RAW_5_0;
    uint16_t PM_RAW_10_0;

    // Formaldehyde (HCHO) concentration in mg/m^3 - PMSxxxxST units only
    uint16_t AMB_HCHO;

    // Temperature & humidity - PMSxxxxST units only
    int16_t PM_TMP;
    uint16_t PM_HUM;
  };

private:
  static const uint16_t SINGLE_RESPONSE_TIME = 1000;
  static const uint16_t TOTAL_RESPONSE_TIME = 1000 * 10;
  static const uint16_t STEADY_RESPONSE_TIME = 1000 * 30;

  static const uint16_t BAUD_RATE = 9600;

  enum STATUS
  {
    STATUS_WAITING,
    STATUS_OK
  };
  enum MODE
  {
    MODE_ACTIVE,
    MODE_PASSIVE
  };

  uint8_t _payload[32];
  Stream *_stream;
  Data _data;
  STATUS _PMSstatus;
  MODE _mode = MODE_ACTIVE;

  uint8_t _index = 0;
  uint16_t _frameLen;
  uint16_t _checksum;
  uint16_t _calculatedChecksum;
  void loop();
  char Char_PM1[10];
  char Char_PM2[10];
  char Char_PM10[10];

public:
  PMS();
  void init(Stream &);
  void sleep();
  void wakeUp();
  void activeMode();
  void passiveMode();

  void requestRead();
  bool readPMS();
  bool readUntil(uint16_t timeout = SINGLE_RESPONSE_TIME);
  const Data& getData() const;
};

class CO2Sensor
{
  Stream *_stream;
  char Char_CO2[10];

public:
  CO2Sensor();
  void init(Stream &);
  int getCO2(int numberOfSamplesToTake = 5);
  int getCO2_Raw();
};

#endif
