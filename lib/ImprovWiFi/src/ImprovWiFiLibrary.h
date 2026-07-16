#pragma once

#if defined(ARDUINO_ARCH_ESP8266)
#include <ESP8266WiFi.h>
#define WIFI_OPEN ENC_TYPE_NONE
#elif defined(ARDUINO_ARCH_ESP32)
#include <WiFi.h>
#define WIFI_OPEN WIFI_AUTH_OPEN
#endif

#include <Stream.h>
#include "ImprovTypes.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

class ImprovWiFi {
 private:
  const char* const CHIP_FAMILY_DESC[5] = {
    "ESP32", "ESP32-C3", "ESP32-S2", "ESP32-S3", "ESP8266"
  };
  ImprovTypes::ImprovWiFiParamsStruct improvWiFiParams;
  uint8_t _buffer[128];
  uint8_t _position = 0;
  Stream* serial;

  void sendDeviceUrl(ImprovTypes::Command cmd);
  bool onCommandCallback(ImprovTypes::ImprovCommand cmd);
  void onErrorCallback(ImprovTypes::Error err);
  void setState(ImprovTypes::State state);
  void sendResponse(std::vector<uint8_t>& response);
  void setError(ImprovTypes::Error error);
  void getAvailableWifiNetworks();
  inline void replaceAll(std::string& str, const std::string& from, const std::string& to);
  bool parseImprovSerial(size_t position, uint8_t byte, const uint8_t* buffer);
  ImprovTypes::ImprovCommand parseImprovData(const std::vector<uint8_t>& data,
                                             bool checkChecksum = true);
  ImprovTypes::ImprovCommand parseImprovData(const uint8_t* data, size_t length,
                                             bool checkChecksum = true);
  std::vector<uint8_t> buildRpcResponse(ImprovTypes::Command command,
                                        const std::vector<std::string>& datum,
                                        bool addChecksum);

 public:
  explicit ImprovWiFi(Stream* stream) : serial(stream) {}

  typedef void(OnImprovError)(ImprovTypes::Error);
  typedef void(OnImprovConnected)(const char* ssid, const char* password);
  typedef bool(CustomConnectWiFi)(const char* ssid, const char* password);

  void handleSerial();
  void setDeviceInfo(ImprovTypes::ChipFamily chipFamily, const char* firmwareName,
                     const char* firmwareVersion, const char* deviceName,
                     const char* deviceUrl);
  void setDeviceInfo(ImprovTypes::ChipFamily chipFamily, const char* firmwareName,
                     const char* firmwareVersion, const char* deviceName);
  void onImprovError(OnImprovError* errorCallback);
  void onImprovConnected(OnImprovConnected* connectedCallback);
  void setCustomConnectWiFi(CustomConnectWiFi* connectWiFiCallback);
  bool tryConnectToWifi(const char* ssid, const char* password);
  bool isConnected();

 private:
  OnImprovError* onImproErrorCallback = nullptr;
  OnImprovConnected* onImprovConnectedCallback = nullptr;
  CustomConnectWiFi* customConnectWiFiCallback = nullptr;
};
