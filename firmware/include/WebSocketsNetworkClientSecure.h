#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  WebSocketsNetworkClientSecure.h — Bridge WiFi/LTE pour la lib WebSockets
//
//  Quand WEBSOCKETS_NETWORK_TYPE == NETWORK_CUSTOM (10), la librairie
//  WebSockets utilise ces classes au lieu de WiFiClient/WiFiClientSecure.
//  WebSocketsNetworkClientSecure DOIT hériter de WebSocketsNetworkClient
//  car la lib caste ssl* en tcp* dans certains cas.
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <Client.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include "config.h"

#if LTE_ENABLED
// Juste les déclarations extern — les définitions sont dans modem.h (inclus par main.cpp)
#include <TinyGsmClient.h>
extern TinyGsm              _modem;
extern TinyGsmClient        _modemClient;
extern TinyGsmClientSecure  _modemClientSSL;
#endif

// Flag global posé par main.cpp pour indiquer qu'on est en mode LTE
extern volatile bool _wsUseLte;

// ─────────────────────────────────────────────────────────────────────────────
//  WebSocketsNetworkClient — client TCP non-SSL (WiFiClient ou TinyGsmClient)
// ─────────────────────────────────────────────────────────────────────────────
class WebSocketsNetworkClient : public Client {
protected:
    WiFiClient _wifi;
#if LTE_ENABLED
    TinyGsmClient _gsm;
#endif
    bool _isLte = false;

    Client& active() {
#if LTE_ENABLED
        return _isLte ? (Client&)_gsm : (Client&)_wifi;
#else
        return _wifi;
#endif
    }

public:
#if LTE_ENABLED
    WebSocketsNetworkClient() : _gsm(_modem, 1) {}
#else
    WebSocketsNetworkClient() {}
#endif

    // Constructeur acceptant WiFiClient (requis par WebSocketsServer)
    WebSocketsNetworkClient(WiFiClient c) : _wifi(c) {}

    virtual int connect(const char* host, uint16_t port, int32_t timeout) {
        _isLte = _wsUseLte;
#if LTE_ENABLED
        if (_isLte) return _gsm.connect(host, port, timeout);
#endif
        return _wifi.connect(host, port, timeout);
    }
    int connect(const char* host, uint16_t port) override { return connect(host, port, 5000); }
    int connect(IPAddress ip, uint16_t port) override { return active().connect(ip, port); }

    size_t write(uint8_t b) override { return active().write(b); }
    size_t write(const uint8_t* buf, size_t sz) override { return active().write(buf, sz); }
    size_t write(const char* str) { return active().print(str); }
    int available() override { return active().available(); }
    int read() override { return active().read(); }
    int read(uint8_t* buf, size_t sz) override { return active().read(buf, sz); }
    int peek() override { return active().peek(); }
    void flush() override { active().flush(); }
    void stop() override { active().stop(); }
    uint8_t connected() override { return active().connected(); }
    operator bool() override { return (bool)active(); }
};

// ─────────────────────────────────────────────────────────────────────────────
//  WebSocketsNetworkClientSecure — client TCP+SSL
//  Hérite de WebSocketsNetworkClient pour que la lib puisse caster ssl → tcp
// ─────────────────────────────────────────────────────────────────────────────
class WebSocketsNetworkClientSecure : public WebSocketsNetworkClient {
protected:
    WiFiClientSecure _wifiSSL;
#if LTE_ENABLED
    TinyGsmClientSecure _gsmSSL;
#endif

    Client& activeSSL() {
#if LTE_ENABLED
        return _isLte ? (Client&)_gsmSSL : (Client&)_wifiSSL;
#else
        return _wifiSSL;
#endif
    }

public:
#if LTE_ENABLED
    WebSocketsNetworkClientSecure() : WebSocketsNetworkClient(), _gsmSSL(_modem, 0) {}
#else
    WebSocketsNetworkClientSecure() : WebSocketsNetworkClient() {}
#endif

    // Constructeur acceptant WiFiClient (requis par la lib)
    WebSocketsNetworkClientSecure(WiFiClient c) : WebSocketsNetworkClient(c) {}

    int connect(const char* host, uint16_t port, int32_t timeout) {
        _isLte = _wsUseLte;
        _sslConnected = false;
        Serial.printf("[ws-net] connect SSL %s:%d via %s\n", host, port, _isLte ? "LTE" : "WiFi");
        int rc;
#if LTE_ENABLED
        if (_isLte) {
            rc = _gsmSSL.connect(host, port, timeout);
            if (rc) {
                _sslConnected = true;
                _lastDataReceived = millis();  // Reset silence timeout après reconnexion
            }
            return rc;
        }
#endif
        _wifiSSL.setInsecure();
        rc = _wifiSSL.connect(host, port, timeout);
        if (rc) _sslConnected = true;
        return rc;
    }
    int connect(const char* host, uint16_t port) override { return connect(host, port, 10000); }
    int connect(IPAddress ip, uint16_t port) override {
        _isLte = _wsUseLte;
#if LTE_ENABLED
        if (_isLte) return _gsmSSL.connect(ip, port);
#endif
        _wifiSSL.setInsecure();
        return _wifiSSL.connect(ip, port);
    }

    size_t write(uint8_t b) override {
        size_t r = activeSSL().write(b);
#if LTE_ENABLED
        if (_isLte && r == 0) { _sslConnected = false; }
#endif
        return r;
    }
    size_t write(const uint8_t* buf, size_t sz) override {
        size_t r = activeSSL().write(buf, sz);
#if LTE_ENABLED
        if (_isLte && r == 0 && sz > 0) { _sslConnected = false; }
#endif
        return r;
    }
    size_t write(const char* str) { return activeSSL().print(str); }
    int available() override {
        int a = activeSSL().available();
#if LTE_ENABLED
        if (_isLte && a > 0) _lastDataReceived = millis();
        // Timeout silencieux : si aucune donnée reçue depuis 60s, connexion morte
        // (le proxy envoie des pings toutes les 20s, donc 60s = 3 pings manqués)
        if (_isLte && _sslConnected && _lastDataReceived > 0 &&
            millis() - _lastDataReceived > 60000) {
            Serial.println("[ws-net] timeout silence 60s — connexion morte");
            _sslConnected = false;
        }
#endif
        return a;
    }
    int read() override { return activeSSL().read(); }
    uint32_t _lastDataReceived = 0;
    int read(uint8_t* buf, size_t sz) override { return activeSSL().read(buf, sz); }
    int peek() override { return activeSSL().peek(); }
    void flush() override { activeSSL().flush(); }
    void stop() override {
        _sslConnected = false;
        activeSSL().stop();
    }
    uint8_t connected() override {
#if LTE_ENABLED
        // En mode LTE, NE PAS appeler TinyGSM connected() — il envoie des
        // commandes AT qui bloquent et corrompent le flux SSL.
        // On fait confiance à notre flag interne : vrai après connect(),
        // faux après stop() ou si write() échoue.
        if (_isLte) return _sslConnected ? 1 : 0;
#endif
        return activeSSL().connected();
    }
    operator bool() override {
#if LTE_ENABLED
        if (_isLte) return _sslConnected;
#endif
        return (bool)activeSSL();
    }
    bool _sslConnected = false;

    // Méthodes SSL appelées par la lib WebSockets
    void setInsecure() { _wifiSSL.setInsecure(); }
    bool verify(const char* fp, const char* host) { return true; }
    void setCACert(const char* ca) { _wifiSSL.setCACert(ca); }
    void setCertificate(const char* cert) { _wifiSSL.setCertificate(cert); }
    void setPrivateKey(const char* key) { _wifiSSL.setPrivateKey(key); }
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 4)
    void setCACertBundle(const uint8_t* bundle, size_t sz) { _wifiSSL.setCACertBundle(bundle, sz); }
#else
    void setCACertBundle(const uint8_t* bundle) { _wifiSSL.setCACertBundle(bundle); }
#endif
    void setTimeout(uint32_t t) { _wifiSSL.setTimeout(t); }
    void setNoDelay(bool nd) { if (!_isLte) _wifiSSL.setNoDelay(nd); }
};
