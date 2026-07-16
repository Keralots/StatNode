#include "ImprovWiFiLibrary.h"

void ImprovWiFi::handleSerial() {
  if (serial->available() <= 0) return;
  const uint8_t byte = serial->read();
  if (parseImprovSerial(_position, byte, _buffer)) _buffer[_position++] = byte;
  else _position = 0;
}

void ImprovWiFi::onErrorCallback(ImprovTypes::Error error) {
  if (onImproErrorCallback) onImproErrorCallback(error);
}

bool ImprovWiFi::onCommandCallback(ImprovTypes::ImprovCommand cmd) {
  switch (cmd.command) {
    case ImprovTypes::Command::GET_CURRENT_STATE:
      if (isConnected()) {
        setState(ImprovTypes::State::STATE_PROVISIONED);
        sendDeviceUrl(cmd.command);
      } else {
        setState(ImprovTypes::State::STATE_AUTHORIZED);
      }
      break;

    case ImprovTypes::Command::WIFI_SETTINGS: {
      if (cmd.ssid.empty()) {
        setError(ImprovTypes::Error::ERROR_INVALID_RPC);
        break;
      }
      setState(ImprovTypes::STATE_PROVISIONING);
      const bool success = customConnectWiFiCallback
        ? customConnectWiFiCallback(cmd.ssid.c_str(), cmd.password.c_str())
        : tryConnectToWifi(cmd.ssid.c_str(), cmd.password.c_str());
      if (success) {
        setError(ImprovTypes::Error::ERROR_NONE);
        setState(ImprovTypes::STATE_PROVISIONED);
        sendDeviceUrl(cmd.command);
        if (onImprovConnectedCallback)
          onImprovConnectedCallback(cmd.ssid.c_str(), cmd.password.c_str());
      } else {
        setState(ImprovTypes::STATE_STOPPED);
        setError(ImprovTypes::ERROR_UNABLE_TO_CONNECT);
        onErrorCallback(ImprovTypes::ERROR_UNABLE_TO_CONNECT);
      }
      break;
    }

    case ImprovTypes::Command::GET_DEVICE_INFO: {
      std::vector<std::string> info = {
        improvWiFiParams.firmwareName,
        improvWiFiParams.firmwareVersion,
        CHIP_FAMILY_DESC[improvWiFiParams.chipFamily],
        improvWiFiParams.deviceName
      };
      std::vector<uint8_t> data = buildRpcResponse(ImprovTypes::GET_DEVICE_INFO, info, false);
      sendResponse(data);
      break;
    }

    case ImprovTypes::Command::GET_WIFI_NETWORKS:
      getAvailableWifiNetworks();
      break;

    default:
      setError(ImprovTypes::ERROR_UNKNOWN_RPC);
      return false;
  }
  return true;
}

void ImprovWiFi::setDeviceInfo(ImprovTypes::ChipFamily chipFamily,
                               const char* firmwareName,
                               const char* firmwareVersion,
                               const char* deviceName) {
  improvWiFiParams.chipFamily = chipFamily;
  improvWiFiParams.firmwareName = firmwareName;
  improvWiFiParams.firmwareVersion = firmwareVersion;
  improvWiFiParams.deviceName = deviceName;
}

void ImprovWiFi::setDeviceInfo(ImprovTypes::ChipFamily chipFamily,
                               const char* firmwareName,
                               const char* firmwareVersion,
                               const char* deviceName,
                               const char* deviceUrl) {
  setDeviceInfo(chipFamily, firmwareName, firmwareVersion, deviceName);
  improvWiFiParams.deviceUrl = deviceUrl;
}

bool ImprovWiFi::isConnected() { return WiFi.status() == WL_CONNECTED; }

void ImprovWiFi::sendDeviceUrl(ImprovTypes::Command command) {
  const IPAddress address = WiFi.localIP();
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", address[0], address[1], address[2], address[3]);
  const std::string ip(buffer);
  if (improvWiFiParams.deviceUrl.empty()) improvWiFiParams.deviceUrl = "http://" + ip;
  else replaceAll(improvWiFiParams.deviceUrl, "{LOCAL_IPV4}", ip);
  std::vector<uint8_t> data = buildRpcResponse(command, {improvWiFiParams.deviceUrl}, false);
  sendResponse(data);
}

void ImprovWiFi::onImprovError(OnImprovError* callback) {
  onImproErrorCallback = callback;
}

void ImprovWiFi::onImprovConnected(OnImprovConnected* callback) {
  onImprovConnectedCallback = callback;
}

void ImprovWiFi::setCustomConnectWiFi(CustomConnectWiFi* callback) {
  customConnectWiFiCallback = callback;
}

bool ImprovWiFi::tryConnectToWifi(const char* ssid, const char* password) {
  if (isConnected()) {
    WiFi.disconnect();
    delay(100);
  }
  WiFi.begin(ssid, password);
  for (uint8_t attempt = 0; attempt <= MAX_ATTEMPTS_WIFI_CONNECTION; attempt++) {
    if (isConnected()) return true;
    delay(DELAY_MS_WAIT_WIFI_CONNECTION);
  }
  WiFi.disconnect();
  return false;
}

void ImprovWiFi::getAvailableWifiNetworks() {
  const int count = WiFi.scanNetworks();
  for (int id = 0; id < count; id++) {
    std::vector<std::string> network = {
      WiFi.SSID(id).c_str(),
      std::to_string(WiFi.RSSI(id)),
      WiFi.encryptionType(id) == WIFI_OPEN ? "NO" : "YES"
    };
    std::vector<uint8_t> data = buildRpcResponse(ImprovTypes::GET_WIFI_NETWORKS,
                                                  network, false);
    sendResponse(data);
    delay(1);
  }
  std::vector<uint8_t> done = buildRpcResponse(ImprovTypes::GET_WIFI_NETWORKS, {}, false);
  sendResponse(done);
}

inline void ImprovWiFi::replaceAll(std::string& value, const std::string& from,
                                   const std::string& to) {
  size_t position = 0;
  while ((position = value.find(from, position)) != std::string::npos) {
    value.replace(position, from.length(), to);
    position += to.length();
  }
}

bool ImprovWiFi::parseImprovSerial(size_t position, uint8_t byte,
                                   const uint8_t* buffer) {
  static const char magic[] = "IMPROV";
  if (position < 6) return byte == (uint8_t)magic[position];
  if (position == 6) return byte == ImprovTypes::IMPROV_SERIAL_VERSION;
  if (position <= 8) return true;

  const uint8_t type = buffer[7];
  const uint8_t dataLength = buffer[8];
  if (position <= 8 + dataLength) return true;
  if (position != 9 + dataLength) return false;

  uint8_t checksum = 0;
  for (size_t i = 0; i < position; i++) checksum += buffer[i];
  if (checksum != byte) {
    _position = 0;
    onErrorCallback(ImprovTypes::Error::ERROR_INVALID_RPC);
    return false;
  }
  if (type == ImprovTypes::ImprovSerialType::TYPE_RPC) {
    _position = 0;
    return onCommandCallback(parseImprovData(&buffer[9], dataLength, false));
  }
  return false;
}

ImprovTypes::ImprovCommand ImprovWiFi::parseImprovData(
    const std::vector<uint8_t>& data, bool checkChecksum) {
  return parseImprovData(data.data(), data.size(), checkChecksum);
}

ImprovTypes::ImprovCommand ImprovWiFi::parseImprovData(
    const uint8_t* data, size_t length, bool checkChecksum) {
  ImprovTypes::ImprovCommand result = {};
  result.command = ImprovTypes::Command::UNKNOWN;
  if (!data || length < 2) return result;
  const ImprovTypes::Command command = (ImprovTypes::Command)data[0];
  const uint8_t dataLength = data[1];
  if (dataLength != length - 2 - (checkChecksum ? 1 : 0)) return result;

  if (checkChecksum) {
    uint8_t calculated = 0;
    for (size_t i = 0; i + 1 < length; i++) calculated += data[i];
    if (calculated != data[length - 1]) {
      result.command = ImprovTypes::Command::BAD_CHECKSUM;
      return result;
    }
  }

  if (command == ImprovTypes::Command::WIFI_SETTINGS) {
    if (length < 4) return result;
    const size_t ssidLength = data[2];
    const size_t ssidStart = 3;
    const size_t ssidEnd = ssidStart + ssidLength;
    if (ssidEnd >= length) return result;
    const size_t passwordLength = data[ssidEnd];
    const size_t passwordStart = ssidEnd + 1;
    const size_t passwordEnd = passwordStart + passwordLength;
    if (passwordEnd > length - (checkChecksum ? 1 : 0)) return result;
    result.command = command;
    result.ssid.assign((const char*)data + ssidStart, ssidLength);
    result.password.assign((const char*)data + passwordStart, passwordLength);
    return result;
  }
  result.command = command;
  return result;
}

void ImprovWiFi::setState(ImprovTypes::State state) {
  std::vector<uint8_t> data = {'I', 'M', 'P', 'R', 'O', 'V'};
  data.resize(11);
  data[6] = ImprovTypes::IMPROV_SERIAL_VERSION;
  data[7] = ImprovTypes::TYPE_CURRENT_STATE;
  data[8] = 1;
  data[9] = state;
  uint8_t checksum = 0;
  for (size_t i = 0; i < 10; i++) checksum += data[i];
  data[10] = checksum;
  serial->write(data.data(), data.size());
}

void ImprovWiFi::setError(ImprovTypes::Error error) {
  std::vector<uint8_t> data = {'I', 'M', 'P', 'R', 'O', 'V'};
  data.resize(11);
  data[6] = ImprovTypes::IMPROV_SERIAL_VERSION;
  data[7] = ImprovTypes::TYPE_ERROR_STATE;
  data[8] = 1;
  data[9] = error;
  uint8_t checksum = 0;
  for (size_t i = 0; i < 10; i++) checksum += data[i];
  data[10] = checksum;
  serial->write(data.data(), data.size());
}

void ImprovWiFi::sendResponse(std::vector<uint8_t>& response) {
  std::vector<uint8_t> data = {'I', 'M', 'P', 'R', 'O', 'V'};
  data.resize(9);
  data[6] = ImprovTypes::IMPROV_SERIAL_VERSION;
  data[7] = ImprovTypes::TYPE_RPC_RESPONSE;
  data[8] = response.size();
  data.insert(data.end(), response.begin(), response.end());
  uint8_t checksum = 0;
  for (uint8_t byte : data) checksum += byte;
  data.push_back(checksum);
  serial->write(data.data(), data.size());
}

std::vector<uint8_t> ImprovWiFi::buildRpcResponse(
    ImprovTypes::Command command, const std::vector<std::string>& values,
    bool addChecksum) {
  std::vector<uint8_t> out = {(uint8_t)command, 0};
  uint8_t length = 0;
  for (const std::string& value : values) {
    const uint8_t itemLength = min(value.size(), (size_t)255);
    length += itemLength + 1;
    out.push_back(itemLength);
    out.insert(out.end(), value.begin(), value.begin() + itemLength);
  }
  out[1] = length;
  if (addChecksum) {
    uint8_t checksum = 0;
    for (uint8_t byte : out) checksum += byte;
    out.push_back(checksum);
  }
  return out;
}
