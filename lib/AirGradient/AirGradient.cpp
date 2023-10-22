#include "AirGradient.h"
#include <Arduino.h>
#include <Wire.h>
#include <math.h>

PMS::PMS()
{
}

void PMS::init(Stream &stream)
{
  _stream = &stream;
}

// Standby mode. For low power consumption and prolong the life of the sensor.
void PMS::sleep()
{
  uint8_t command[] = {0x42, 0x4D, 0xE4, 0x00, 0x00, 0x01, 0x73};
  _stream->write(command, sizeof(command));
}

// Operating mode. Stable data should be got at least 30 seconds after the sensor wakeup from the sleep mode because of the fan's performance.
void PMS::wakeUp()
{
  uint8_t command[] = {0x42, 0x4D, 0xE4, 0x00, 0x01, 0x01, 0x74};
  _stream->write(command, sizeof(command));
}

// Active mode. Default mode after power up. In this mode sensor would send serial data to the host automatically.
void PMS::activeMode()
{

  uint8_t command[] = {0x42, 0x4D, 0xE1, 0x00, 0x01, 0x01, 0x71};
  _stream->write(command, sizeof(command));
  _mode = MODE_ACTIVE;
}

// Passive mode. In this mode sensor would send serial data to the host only for request.
void PMS::passiveMode()
{
  uint8_t command[] = {0x42, 0x4D, 0xE1, 0x00, 0x00, 0x01, 0x70};
  _stream->write(command, sizeof(command));
  _mode = MODE_PASSIVE;
}

// Request read in Passive Mode.
void PMS::requestRead()
{
  if (_mode == MODE_PASSIVE)
  {
    uint8_t command[] = {0x42, 0x4D, 0xE2, 0x00, 0x00, 0x01, 0x71};
    _stream->write(command, sizeof(command));
  }
}

// Non-blocking function for parse response.
bool PMS::readPMS()
{
  loop();
  return _PMSstatus == STATUS_OK;
}

// Blocking function for parse response. Default timeout is 1s.
bool PMS::readUntil(uint16_t timeout)
{
  uint32_t start = millis();
  do
  {
    loop();
    if (_PMSstatus == STATUS_OK)
      break;
  } while (millis() - start < timeout);

  return _PMSstatus == STATUS_OK;
}

const PMS::Data& PMS::getData() const
{
  return _data;
}

void PMS::loop()
{
  _PMSstatus = STATUS_WAITING;
  if (_stream->available())
  {
    uint8_t ch = _stream->read();

    switch (_index)
    {
    case 0:
      if (ch != 0x42)
      {
        return;
      }
      _calculatedChecksum = ch;
      break;

    case 1:
      if (ch != 0x4D)
      {
        _index = 0;
        return;
      }
      _calculatedChecksum += ch;
      break;

    case 2:
      _calculatedChecksum += ch;
      _frameLen = ch << 8;
      break;

    case 3:
      _frameLen |= ch;
      // Unsupported sensor, different frame length, transmission error e.t.c.
      if (_frameLen != 2 * 9 + 2 && _frameLen != 2 * 13 + 2)
      {
        _index = 0;
        return;
      }
      _calculatedChecksum += ch;
      break;

    default:
      if (_index == _frameLen + 2)
      {
        _checksum = ch << 8;
      }
      else if (_index == _frameLen + 2 + 1)
      {
        _checksum |= ch;

        if (_calculatedChecksum == _checksum)
        {
          _PMSstatus = STATUS_OK;

          // Standard Particles, CF=1.
          _data.PM_SP_UG_1_0 = makeWord(_payload[0], _payload[1]);
          _data.PM_SP_UG_2_5 = makeWord(_payload[2], _payload[3]);
          _data.PM_SP_UG_10_0 = makeWord(_payload[4], _payload[5]);

          // Atmospheric Environment.
          _data.PM_AE_UG_1_0 = makeWord(_payload[6], _payload[7]);
          _data.PM_AE_UG_2_5 = makeWord(_payload[8], _payload[9]);
          _data.PM_AE_UG_10_0 = makeWord(_payload[10], _payload[11]);

          // Total particles count per 100ml air
          _data.PM_RAW_0_3 = makeWord(_payload[12], _payload[13]);
          _data.PM_RAW_0_5 = makeWord(_payload[14], _payload[15]);
          _data.PM_RAW_1_0 = makeWord(_payload[16], _payload[17]);
          _data.PM_RAW_2_5 = makeWord(_payload[18], _payload[19]);
          _data.PM_RAW_5_0 = makeWord(_payload[20], _payload[21]);
          _data.PM_RAW_10_0 = makeWord(_payload[22], _payload[23]);

          // Formaldehyde concentration (PMSxxxxST units only)
          _data.AMB_HCHO = makeWord(_payload[24], _payload[25]) / 1000;

          // Temperature & humidity (PMSxxxxST units only)
          _data.PM_TMP = makeWord(_payload[20], _payload[21]);
          _data.PM_HUM = makeWord(_payload[22], _payload[23]);
        }

        _index = 0;
        return;
      }
      else
      {
        _calculatedChecksum += ch;
        uint8_t payloadIndex = _index - 4;

        // Payload is common to all sensors (first 2x6 bytes).
        if (payloadIndex < sizeof(_payload))
        {
          _payload[payloadIndex] = ch;
        }
      }

      break;
    }

    _index++;
  }
}

CO2Sensor::CO2Sensor() {}

void CO2Sensor::init(Stream &stream)
{
  _stream = &stream;

  if (getCO2_Raw() == -1)
  {
    Serial.println("CO2 Sensor Failed to Initialize ");
  }
  else
  {
    Serial.println("CO2 Successfully Initialized.");
  }
}

int CO2Sensor::getCO2(int numberOfSamplesToTake)
{
  int successfulSamplesCounter = 0;
  int co2AsPpmSum = 0;
  for (int sample = 0; sample < numberOfSamplesToTake; sample++)
  {
    int co2AsPpm = getCO2_Raw();
    if (co2AsPpm > 300 && co2AsPpm < 10000)
    {
      Serial.println("CO2 read success " + String(co2AsPpm));
      successfulSamplesCounter++;
      co2AsPpmSum += co2AsPpm;
    }
    else
    {
      Serial.println("CO2Sensor read failed with " + String(co2AsPpm));
    }

    // without delay we get a few 10ms spacing, add some more
    delay(250);
  }

  if (successfulSamplesCounter <= 0)
  {
    // total failure
    return -5;
  }
  Serial.println("# of CO2Sensor reads that worked: " + String(successfulSamplesCounter));
  Serial.println("CO2Sensor reads sum " + String(co2AsPpmSum));
  return co2AsPpmSum / successfulSamplesCounter;
}

// <<>>
int CO2Sensor::getCO2_Raw()
{
  while (_stream->available()) // flush whatever we might have
    _stream->read();

  const byte CO2Command[] = {0XFE, 0X04, 0X00, 0X03, 0X00, 0X01, 0XD5, 0XC5};
  byte CO2Response[] = {0, 0, 0, 0, 0, 0, 0};
  int datapos = -1;

  const int commandSize = 8;
  const int responseSize = 7;

  int numberOfBytesWritten = _stream->write(CO2Command, commandSize);

  if (numberOfBytesWritten != commandSize)
  {
    // failed to write request
    return -2;
  }

  // attempt to read response
  int timeoutCounter = 0;
  while (_stream->available() < responseSize)
  {
    timeoutCounter++;
    if (timeoutCounter > 10)
    {
      // timeout when reading response
      return -3;
    }
    delay(50);
  }

  // we have 7 bytes ready to be read
  for (int i = 0; i < responseSize; i++)
  {
    CO2Response[i] = _stream->read();
    if ((CO2Response[i] == 0xFE) && (datapos == -1))
    {
      datapos = i;
    }
    Serial.print(CO2Response[i], HEX);
    Serial.print(":");
  }
  return CO2Response[datapos + 3] * 256 + CO2Response[datapos + 4];
}