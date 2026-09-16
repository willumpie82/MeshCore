#pragma once

// Minimal Improv Wi-Fi (https://www.improv-wifi.com/serial/) serial provisioning.
// POC: implements GET_CURRENT_STATE, WIFI_SETTINGS, GET_DEVICE_INFO.
// Credentials are persisted to NVS so provisioning only needs to happen once.

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>

class ImprovWiFiSerial {
  enum Type : uint8_t {
    TYPE_CURRENT_STATE = 0x01,
    TYPE_ERROR_STATE   = 0x02,
    TYPE_RPC           = 0x03,
    TYPE_RPC_RESPONSE  = 0x04,
  };
  enum State : uint8_t {
    STATE_STOPPED       = 0x00,
    STATE_AUTHORIZED    = 0x02,
    STATE_PROVISIONING  = 0x03,
    STATE_PROVISIONED   = 0x04,
  };
  enum Command : uint8_t {
    CMD_WIFI_SETTINGS    = 0x01,
    CMD_GET_CURRENT_STATE = 0x02,
    CMD_GET_DEVICE_INFO  = 0x03,
    CMD_GET_WIFI_NETWORKS = 0x04,
  };
  enum Error : uint8_t {
    ERROR_NONE = 0x00,
    ERROR_INVALID_RPC = 0x01,
    ERROR_UNKNOWN_RPC = 0x02,
    ERROR_UNABLE_TO_CONNECT = 0x03,
  };

  Stream* _s = nullptr;
  const char* _fw_name;
  const char* _fw_version;
  const char* _device_name;

  uint8_t _buf[64];
  size_t _pos = 0;

  Preferences _prefs;

  void sendPacket(Type type, const uint8_t* payload, uint8_t len) {
    uint8_t hdr[9] = {'I','M','P','R','O','V', 1, (uint8_t)type, len};
    uint8_t checksum = 0;
    for (int i = 0; i < 9; i++) checksum += hdr[i];
    for (int i = 0; i < len; i++) checksum += payload[i];
    _s->write(hdr, 9);
    if (len) _s->write(payload, len);
    _s->write(checksum);
  }

  void sendState(State st) {
    uint8_t payload[1] = { (uint8_t)st };
    sendPacket(TYPE_CURRENT_STATE, payload, 1);
  }

  void sendError(Error err) {
    uint8_t payload[1] = { (uint8_t)err };
    sendPacket(TYPE_ERROR_STATE, payload, 1);
  }

  void sendRpcResponse(Command cmd, const char* strs[], int n) {
    uint8_t payload[64];
    int i = 0;
    payload[i++] = (uint8_t)cmd;
    int len_pos = i++;
    int data_start = i;
    for (int s = 0; s < n; s++) {
      uint8_t slen = (uint8_t)strlen(strs[s]);
      payload[i++] = slen;
      memcpy(&payload[i], strs[s], slen);
      i += slen;
    }
    payload[len_pos] = (uint8_t)(i - data_start);
    sendPacket(TYPE_RPC_RESPONSE, payload, i);
  }

  void sendDeviceUrl(Command cmd) {
    IPAddress ip = WiFi.localIP();
    char buf[24];
    snprintf(buf, sizeof(buf), "http://%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
    const char* strs[1] = { buf };
    sendRpcResponse(cmd, strs, 1);
  }

  bool tryConnect(const char* ssid, const char* pass) {
    if (WiFi.status() == WL_CONNECTED) WiFi.disconnect();
    WiFi.begin(ssid, pass);
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);
    return WiFi.status() == WL_CONNECTED;
  }

  void handleRpc(const uint8_t* data, uint8_t len) {
    if (len < 2) return;
    Command cmd = (Command)data[0];

    if (cmd == CMD_GET_CURRENT_STATE) {
      if (WiFi.status() == WL_CONNECTED) {
        sendState(STATE_PROVISIONED);
        sendDeviceUrl(cmd);
      } else {
        sendState(STATE_AUTHORIZED);
      }
    } else if (cmd == CMD_WIFI_SETTINGS) {
      uint8_t ssid_len = data[2];
      const char* ssid_ptr = (const char*)&data[3];
      uint8_t pass_len = data[3 + ssid_len];
      const char* pass_ptr = (const char*)&data[3 + ssid_len + 1];

      char ssid[33] = {0}, pass[65] = {0};
      memcpy(ssid, ssid_ptr, min(ssid_len, (uint8_t)32));
      memcpy(pass, pass_ptr, min(pass_len, (uint8_t)64));

      sendState(STATE_PROVISIONING);

      if (tryConnect(ssid, pass)) {
        _prefs.begin("improv", false);
        _prefs.putString("ssid", ssid);
        _prefs.putString("pass", pass);
        _prefs.end();
        sendError(ERROR_NONE);
        sendState(STATE_PROVISIONED);
        sendDeviceUrl(cmd);
      } else {
        sendState(STATE_STOPPED);
        sendError(ERROR_UNABLE_TO_CONNECT);
      }
    } else if (cmd == CMD_GET_DEVICE_INFO) {
      const char* strs[4] = { _fw_name, _fw_version, "ESP32-S3", _device_name };
      sendRpcResponse(cmd, strs, 4);
    } else {
      sendError(ERROR_UNKNOWN_RPC);
    }
  }

  void feed(uint8_t b) {
    if (_pos == 0) { if (b == 'I') _buf[_pos++] = b; return; }
    if (_pos == 1) { if (b == 'M') _buf[_pos++] = b; else _pos = 0; return; }
    if (_pos == 2) { if (b == 'P') _buf[_pos++] = b; else _pos = 0; return; }
    if (_pos == 3) { if (b == 'R') _buf[_pos++] = b; else _pos = 0; return; }
    if (_pos == 4) { if (b == 'O') _buf[_pos++] = b; else _pos = 0; return; }
    if (_pos == 5) { if (b == 'V') _buf[_pos++] = b; else _pos = 0; return; }
    if (_pos == 6) { if (b == 1) _buf[_pos++] = b; else _pos = 0; return; } // version
    if (_pos >= sizeof(_buf)) { _pos = 0; return; } // overflow guard
    _buf[_pos++] = b;

    if (_pos < 9) return; // need type(7) + len(8)
    uint8_t type = _buf[7];
    uint8_t data_len = _buf[8];
    if (_pos < (size_t)(9 + data_len + 1)) return; // waiting for payload + checksum

    uint8_t checksum = 0;
    for (size_t i = 0; i < 9 + data_len; i++) checksum += _buf[i];
    uint8_t got_checksum = _buf[9 + data_len];
    _pos = 0; // reset for next packet regardless of outcome

    if (checksum != got_checksum) return;
    if (type == TYPE_RPC) handleRpc(&_buf[9], data_len);
  }

public:
  void begin(Stream& s, const char* fw_name, const char* fw_version, const char* device_name) {
    _s = &s;
    _fw_name = fw_name;
    _fw_version = fw_version;
    _device_name = device_name;
  }

  // Returns true once WiFi is connected (either from stored creds, or freshly provisioned)
  bool loadStoredCredsAndConnect() {
    _prefs.begin("improv", true);
    String ssid = _prefs.getString("ssid", "");
    String pass = _prefs.getString("pass", "");
    _prefs.end();
    if (ssid.length() == 0) return false;
    return tryConnect(ssid.c_str(), pass.c_str());
  }

  void loop() {
    while (_s->available()) feed((uint8_t)_s->read());
  }

  bool isConnected() { return WiFi.status() == WL_CONNECTED; }
};
