#pragma once
// ─────────────────────────────────────────────────────────────────────────────
//  WebSocketsNetworkClientSecure.h — Bridge WiFi/LTE pour la lib WebSockets
//
//  Quand WEBSOCKETS_NETWORK_TYPE == NETWORK_CUSTOM (10), la librairie
//  WebSockets utilise ces classes au lieu de WiFiClient/WiFiClientSecure.
//  Elles délèguent au WiFi ou au TinyGSM selon connGetType().
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <Client.h>
#include <WiFiClientSecure.h>
#include <TinyGsmClient.h>
#include "config.h"
#include "connectivity.h"

// Instances modem déclarées dans modem.h (ou stubs si LTE désactivé)
extern TinyGsm       _modem;
#if LTE_ENABLED
extern TinyGsmClientSecure _modemClientSSL;
#endif

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
    WebSocketsNetworkClient() : _gsm(_modem, 0) {}
#else
    WebSocketsNetworkClient() {}
#endif

    int connect(const char* host, uint16_t port, int timeout) {
        _isLte = (connGetType() == CONN_LTE);
#if LTE_ENABLED
        if (_isLte) return _gsm.connect(host, port, timeout);
#endif
        return _wifi.connect(host, port, timeout);
    }
    int connect(const char* host, uint16_t port) override { return connect(host, port, 5); }
    int connect(IPAddress ip, uint16_t port) override { return active().connect(ip, port); }

    size_t write(uint8_t b) override { return active().write(b); }
    size_t write(const uint8_t* buf, size_t sz) override { return active().write(buf, sz); }
    int available() override { return active().available(); }
    int read() override { return active().read(); }
    int read(uint8_t* buf, size_t sz) override { return active().read(buf, sz); }
    int peek() override { return active().peek(); }
    void flush() override { active().flush(); }
    void stop() override { active().stop(); }
    uint8_t connected() override { return active().connected(); }
    operator bool() { return (bool)active(); }
};

// ─────────────────────────────────────────────────────────────────────────────
//  WebSocketsNetworkClientSecure — client TCP+SSL (WiFiClientSecure ou TinyGsmClientSecure)
// ─────────────────────────────────────────────────────────────────────────────
class WebSocketsNetworkClientSecure : public Client {
protected:
    WiFiClientSecure _wifi;
#if LTE_ENABLED
    TinyGsmClientSecure _gsm;
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
    WebSocketsNetworkClientSecure() : _gsm(_modem, 1) {}
#else
    WebSocketsNetworkClientSecure() {}
#endif

    int connect(const char* host, uint16_t port, int timeout) {
        _isLte = (connGetType() == CONN_LTE);
        Serial.printf("[ws-net] connect %s:%d via %s\n", host, port, _isLte ? "LTE" : "WiFi");
#if LTE_ENABLED
        if (_isLte) return _gsm.connect(host, port, timeout);
#endif
        _wifi.setInsecure();  // pas de vérification certificat (Cloudflare)
        return _wifi.connect(host, port, timeout);
    }
    int connect(const char* host, uint16_t port) override { return connect(host, port, 10); }
    int connect(IPAddress ip, uint16_t port) override {
        _isLte = (connGetType() == CONN_LTE);
#if LTE_ENABLED
        if (_isLte) return _gsm.connect(ip, port);
#endif
        _wifi.setInsecure();
        return _wifi.connect(ip, port);
    }

    size_t write(uint8_t b) override { return active().write(b); }
    size_t write(const uint8_t* buf, size_t sz) override { return active().write(buf, sz); }
    int available() override { return active().available(); }
    int read() override { return active().read(); }
    int read(uint8_t* buf, size_t sz) override { return active().read(buf, sz); }
    int peek() override { return active().peek(); }
    void flush() override { active().flush(); }
    void stop() override { active().stop(); }
    uint8_t connected() override { return active().connected(); }
    operator bool() { return (bool)active(); }

    // Méthodes appelées par la lib WebSockets (mode SSL_AXTLS)
    void setInsecure() { _wifi.setInsecure(); }
    bool verify(const char* fp, const char* host) {
        if (_isLte) return true;  // modem gère SSL
        return true;  // pas de vérification fingerprint
    }
    void setCACert(const char* ca) { _wifi.setCACert(ca); }
    void setCertificate(const char* cert) { _wifi.setCertificate(cert); }
    void setPrivateKey(const char* key) { _wifi.setPrivateKey(key); }
    void setTimeout(uint32_t t) { _wifi.setTimeout(t); }
    void setNoDelay(bool nd) { if (!_isLte) _wifi.setNoDelay(nd); }
};
