/**
 * IotWebConf.cpp -- IotWebConf is an ESP8266/ESP32
 *   non blocking WiFi/AP web configuration library for Arduino.
 *   https://github.com/prampec/IotWebConf
 *
 * Copyright (C) 2018 Balazs Kelemen <prampec+arduino@gmail.com>
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <EEPROM.h>

#include "x_IotWebConf.h"

#ifdef IOTWEBCONF_CONFIG_USE_MDNS
//# ifdef ESP8266
#  include <ESP8266mDNS.h>
//# else defined(ESP32)
//#  include <ESPmDNS.h>
//# endif
#endif

#define IOTWEBCONF_STATUS_ENABLED (this->_statusPin >= 0)

IotWebConfParameter::IotWebConfParameter()
{
}
IotWebConfParameter::IotWebConfParameter(
  const char *label,
  const char *id,
  char *valueBuffer,
  int length,
  const char *type,
  const char *placeholder,
  const char *defaultValue,
  const char *customHtml,
  boolean visible)
{
  this->label = label;
  this->_id = id;
  this->valueBuffer = valueBuffer;
  this->_length = length;
  this->type = type;
  this->placeholder = placeholder;
  this->customHtml = customHtml;
  this->visible = visible;
}
IotWebConfParameter::IotWebConfParameter(
  const char *id,
  char *valueBuffer,
  int length,
  const char *customHtml,
  const char *type)
{
  this->label = NULL;
  this->_id = id;
  this->valueBuffer = valueBuffer;
  this->_length = length;
  this->type = type;
  this->customHtml = customHtml;
  this->visible = true;
}

IotWebConfSeparator::IotWebConfSeparator()  : IotWebConfParameter(NULL, NULL, NULL, 0, NULL, NULL, NULL, NULL, true)
{
}

////////////////////////////////////////////////////////////////

IotWebConf::IotWebConf(const char* defaultThingName, DNSServer* dnsServer, WebServer* server,
  const char *initialApPassword,
  const char* configVersion)
{
  strncpy(this->_thingName, defaultThingName, IOTWEBCONF_WORD_LEN);
  this->_dnsServer = dnsServer;
  this->_server = server;
  this->_initialApPassword = initialApPassword;
  this->_configVersion = configVersion;
  itoa(this->_apTimeoutMs / 1000, this->_apTimeoutStr, 10);

  this->_thingNameParameter = IotWebConfParameter("Ger&aumlte-Name", "iwcThingName", this->_thingName, IOTWEBCONF_WORD_LEN);
  this->_apPasswordParameter = IotWebConfParameter("Ger&aumlte-Passwort", "iwcApPassword", this->_apPassword, IOTWEBCONF_WORD_LEN, "password");
  this->_wifiSsidParameter = IotWebConfParameter("WLAN SSID", "iwcWifiSsid", this->_wifiSsid, IOTWEBCONF_WORD_LEN);
  this->_wifiPasswordParameter = IotWebConfParameter("WLAN Passwort", "iwcWifiPassword", this->_wifiPassword, IOTWEBCONF_WORD_LEN, "password");
  this->_apTimeoutParameter = IotWebConfParameter("Startverz&oumlgerung (Sekunden)", "iwcApTimeout", this->_apTimeoutStr, IOTWEBCONF_WORD_LEN, "number", NULL, "min='1' max='600'", NULL, false);
  this->addParameter(&this->_thingNameParameter);
  this->addParameter(&this->_apPasswordParameter);
  this->addParameter(&this->_wifiSsidParameter);
  this->addParameter(&this->_wifiPasswordParameter);
  this->addParameter(&this->_apTimeoutParameter);
}

char* IotWebConf::getThingName()
{
  return this->_thingName;
}

void IotWebConf::setConfigPin(int configPin)
{
  this->_configPin = configPin;
}

void IotWebConf::setStatusPin(int statusPin)
{
  this->_statusPin = statusPin;
}

void IotWebConf::setupUpdateServer(HTTPUpdateServer* updateServer, const char* updatePath)
{
  this->_updateServer = updateServer;
  this->_updatePath = updatePath;
}

boolean IotWebConf::init()
{
  // -- Setup pins.
  if (this->_configPin >= 0)
  {
    pinMode(this->_configPin, INPUT_PULLUP);
    this->_forceDefaultPassword = (digitalRead(this->_configPin) == LOW);
  }
  if (IOTWEBCONF_STATUS_ENABLED)
  {
    pinMode(this->_statusPin, OUTPUT);
    digitalWrite(this->_statusPin, IOTWEBCONF_STATUS_ON);
  }

  // -- Load configuration from EEPROM.
  this->configInit();
  boolean validConfig = this->configLoad();
  if (!validConfig)
  {
    // -- No config
    this->_apPassword[0] = '\0';
    this->_wifiSsid[0] = '\0';
    this->_wifiPassword[0] = '\0';
    this->_apTimeoutMs = IOTWEBCONF_DEFAULT_AP_MODE_TIMEOUT_MS;
  }
  else
  {
    this->_apTimeoutMs = atoi(this->_apTimeoutStr) * 1000;
  }

  // -- Setup mdns
//#ifdef ESP8266
  WiFi.hostname(this->_thingName);
//#else defined(ESP32)
//   WiFi.setHostname(this->_thingName);
//#endif
#ifdef IOTWEBCONF_CONFIG_USE_MDNS
  MDNS.begin(this->_thingName);
  MDNS.addService("http", "tcp", 80);
#endif

  return validConfig;
}

//////////////////////////////////////////////////////////////////

bool IotWebConf::addParameter(IotWebConfParameter *parameter)
{
/*
#ifdef IOTWEBCONF_DEBUG_TO_SERIAL
  Serial.print("Adding parameter '");
  Serial.print(parameter->getId());
  Serial.println("'");
#endif
*/
  if (this->_firstParameter == NULL)
  {
    this->_firstParameter = parameter;
//    IOTWEBCONF_DEBUG_LINE(F("Adding as first"));
    return true;
  }
  IotWebConfParameter *current = this->_firstParameter;
  while(current->_nextParameter != NULL)
  {
    current = current->_nextParameter;
  }

  current->_nextParameter = parameter;
  return true;
}

void IotWebConf::configInit()
{
  int size = 0;
  IotWebConfParameter *current = this->_firstParameter;
  while(current != NULL)
  {
    size += current->getLength();
    current = current->_nextParameter;
  }
#ifdef IOTWEBCONF_DEBUG_TO_SERIAL
  Serial.print("Konfig-Größe: ");
  Serial.println(size);
#endif

  EEPROM.begin(IOTWEBCONF_CONFIG_START + IOTWEBCONF_CONFIG_VESION_LENGTH + size);
}

/**
 * Load the configuration from the eeprom.
 */
boolean IotWebConf::configLoad()
{
  if (this->configTestVersion())
  {
    IotWebConfParameter *current = this->_firstParameter;
    int start = IOTWEBCONF_CONFIG_START + IOTWEBCONF_CONFIG_VESION_LENGTH;
    while(current != NULL)
    {
      if (current->getId() != NULL)
      {
        this->readEepromValue(start, current->valueBuffer, current->getLength());
#ifdef IOTWEBCONF_DEBUG_TO_SERIAL
        Serial.print("Geladene Konfig '");
        Serial.print(current->getId());
        Serial.print("'= '");
        Serial.print(current->valueBuffer);
        Serial.println("'");
#endif

        start += current->getLength();
      }
      current = current->_nextParameter;
    }
    return true;
  }
  else
  {
    IOTWEBCONF_DEBUG_LINE(F("Falsche Konfig-Version."));
    return false;
  }
}

void IotWebConf::configSave()
{
  this->configSaveConfigVersion();
  IotWebConfParameter *current = this->_firstParameter;
  int start = IOTWEBCONF_CONFIG_START + IOTWEBCONF_CONFIG_VESION_LENGTH;
  while(current != NULL)
  {
    if (current->getId() != NULL)
    {
#ifdef IOTWEBCONF_DEBUG_TO_SERIAL
      Serial.print("Speichere Konfig '");
      Serial.print(current->getId());
      Serial.print("'= '");
      Serial.print(current->valueBuffer);
      Serial.println("'");
#endif

      this->writeEepromValue(start, current->valueBuffer, current->getLength());
      start += current->getLength();
    }
    current = current->_nextParameter;
  }
  EEPROM.commit();

  this->_apTimeoutMs = atoi(this->_apTimeoutStr) * 1000;

  if (this->_configSavedCallback != NULL)
  {
    this->_configSavedCallback();
  }
}

void IotWebConf::readEepromValue(int start, char* valueBuffer, int length)
{
    for (int t=0; t<length; t++)
    {
      *((char*)valueBuffer + t) = EEPROM.read(start + t);
    }
}
void IotWebConf::writeEepromValue(int start, char* valueBuffer, int length)
{
    for (int t=0; t<length; t++)
    {
      EEPROM.write(start+t, *((char*)valueBuffer + t));
    }
}

boolean IotWebConf::configTestVersion()
{
  for (byte t=0; t<IOTWEBCONF_CONFIG_VESION_LENGTH; t++)
  {
    if(EEPROM.read(IOTWEBCONF_CONFIG_START + t) != this->_configVersion[t])
    {
      return false;
    }
  }
  return true;
}

void IotWebConf::configSaveConfigVersion()
{
  for (byte t=0; t<IOTWEBCONF_CONFIG_VESION_LENGTH; t++)
  {
    EEPROM.write(IOTWEBCONF_CONFIG_START + t, this->_configVersion[t]);
  }
}

void IotWebConf::setWifiConnectionCallback( std::function<void()> func )
{
  this->_wifiConnectionCallback = func;
}

void IotWebConf::setConfigSavedCallback( std::function<void()> func )
{
  this->_configSavedCallback = func;
}

void IotWebConf::setFormValidator( std::function<boolean()> func )
{
  this->_formValidator = func;
}

void IotWebConf::setWifiConnectionTimeoutMs(unsigned long millis)
{
  this->_wifiConnectionTimeoutMs = millis;
}

////////////////////////////////////////////////////////////////////////////////

void IotWebConf::handleConfig()
{
  if (this->_state == IOTWEBCONF_STATE_ONLINE)
  {
    // -- Authenticate
    if(!this->_server->authenticate(IOTWEBCONF_ADMIN_USER_NAME, this->_apPassword))
    {
      IOTWEBCONF_DEBUG_LINE(F("Authentifizierung anfordern."));
      this->_server->requestAuthentication();
      return;
    }
  }

  if (!this->_server->hasArg("iotSave") || !this->validateForm())
  {
    // -- Display config portal
    IOTWEBCONF_DEBUG_LINE(F("Konfigurationsseite angefordert."));
    String page = FPSTR(IOTWEBCONF_HTTP_HEAD);
    page.replace("{v}", "GW60-ESP");
    page += FPSTR(IOTWEBCONF_HTTP_SCRIPT);
    page += FPSTR(IOTWEBCONF_HTTP_STYLE);
    page += FPSTR(IOTWEBCONF_HTTP_HEAD_END);

    page += FPSTR(IOTWEBCONF_HTTP_FORM_START);
    char parLength[5];
    // -- Add parameters to the form
    IotWebConfParameter *current = this->_firstParameter;
    while(current != NULL)
    {
      if (current->getId() == NULL)
      {
#ifdef IOTWEBCONF_DEBUG_TO_SERIAL
        Serial.println("Trennlinie rendern");
#endif
        page += "</fieldset><fieldset>";
      }
      else if (current->visible)
      {
#ifdef IOTWEBCONF_DEBUG_TO_SERIAL
        Serial.print("Rendere '");
        Serial.print(current->getId());
        Serial.print("' mit Wert: ");
        Serial.println(current->valueBuffer);
#endif

        String pitem = FPSTR(IOTWEBCONF_HTTP_FORM_PARAM);
        if (current->label != NULL) {
          pitem.replace("{b}", current->label);
          pitem.replace("{t}", current->type);
          pitem.replace("{i}", current->getId());
          pitem.replace("{p}", current->placeholder);
          snprintf(parLength, 5, "%d", current->getLength());
          pitem.replace("{l}", parLength);
          if (strcmp("password", current->type) == 0)
          {
            // -- Value of password is not rendered
            pitem.replace("{v}", "");
          }
          else if (this->_server->hasArg(current->getId()))
          {
            // -- Value from previous submit
            pitem.replace("{v}", this->_server->arg(current->getId()));
          }
          else
          {
            // -- Value from config
            pitem.replace("{v}", current->valueBuffer);
          }
          pitem.replace("{c}", current->customHtml);
          pitem.replace("{e}", current->errorMessage);
          pitem.replace("{s}", current->errorMessage == NULL ? "" : "de"); // Div style class.
        }
        else
        {
          pitem = current->customHtml;
        }

        page += pitem;
      }
      current = current->_nextParameter;
    }

    page += FPSTR(IOTWEBCONF_HTTP_FORM_END);

    if (this->_updatePath != NULL)
    {
      String pitem = FPSTR(IOTWEBCONF_HTTP_UPDATE);
      pitem.replace("{u}", this->_updatePath);
      page += pitem;
    }

    // -- Fill config version string;
    {
      String pitem = FPSTR(IOTWEBCONF_HTTP_CONFIG_VER);
      pitem.replace("{v}", this->_configVersion);
      page += pitem;
    }

    page += FPSTR(IOTWEBCONF_HTTP_END);

    this->_server->sendHeader("Content-Length", String(page.length()));
    this->_server->send(200, "text/html", page);
  }
  else
  {
    // -- Save config
    IOTWEBCONF_DEBUG_LINE(F("Aktuallisiere Konfig"));
    char temp[IOTWEBCONF_WORD_LEN];

    IotWebConfParameter *current = this->_firstParameter;
    while(current != NULL)
    {
      if ((current->getId() != NULL) && (current->visible))
      {
        if ((strcmp("password", current->type) == 0) && (current->getLength() <= IOTWEBCONF_WORD_LEN))
        {
          // TODO: Passwords longer than IOTWEBCONF_WORD_LEN not supported.
          this->readParamValue(current->getId(), temp, current->getLength());
          if (temp[0] != '\0')
          {
            // -- Value was set.
            strncpy(current->valueBuffer, temp, current->getLength());
#ifdef IOTWEBCONF_DEBUG_TO_SERIAL
            Serial.print(current->getId());
            Serial.println(" wurde gesetzt");
#endif
          }
          else
          {
#ifdef IOTWEBCONF_DEBUG_TO_SERIAL
            Serial.print(current->getId());
            Serial.println(" hat sich nicht verändert");
#endif
          }
        }
        else
        {
          this->readParamValue(current->getId(), current->valueBuffer, current->getLength());
#ifdef IOTWEBCONF_DEBUG_TO_SERIAL
          Serial.print(current->getId());
          Serial.print("='");
          Serial.print(current->valueBuffer);
          Serial.println("'");
#endif
        }
      }
      current = current->_nextParameter;
    }

    this->configSave();

    String page = FPSTR(IOTWEBCONF_HTTP_HEAD);
    page.replace("{v}", "GW60-ESP");
    page += FPSTR(IOTWEBCONF_HTTP_SCRIPT);
    page += FPSTR(IOTWEBCONF_HTTP_STYLE);
//    page += _customHeadElement;
    page += FPSTR(IOTWEBCONF_HTTP_HEAD_END);
    page += "Konfiguration gespeichert. ";
    if (this->_apPassword[0] == '\0')
    {
      page += F("Du musst das Standard Ger&aumlte-Passwort ersetzen. Gehe zur&uumlck zur <a href=''>Konfigurations-Seite</a>.");
    }
    else if (this->_wifiSsid[0] == '\0')
    {
      page += F("Du musst ein WLAN-Kennwort eingeben. Gehe zur&uumlck zur <a href=''>Konfigurations-Seite</a>.");
    }
    else if (this->_state == IOTWEBCONF_STATE_NOT_CONFIGURED)
    {
      page += F("Bitte trenne Dich vom WLAN-AP zum Fortsetzen!");
    }
    else
    {
      page += F("Gehe zur&uumlck zur <a href='/'>Start-Seite</a>.");
    }
    page += FPSTR(IOTWEBCONF_HTTP_END);

    this->_server->sendHeader("Content-Length", String(page.length()));
    this->_server->send(200, "text/html", page);
  }
}

void IotWebConf::readParamValue(const char* paramName, char* target, unsigned int len)
{
  String value = this->_server->arg(paramName);
#ifdef IOTWEBCONF_DEBUG_TO_SERIAL
  Serial.print("Wert von '");
  Serial.print(paramName);
  Serial.print("' ist:");
  Serial.println(value);
#endif
  value.toCharArray(target, len);
}

boolean IotWebConf::validateForm()
{
  // -- Clean previous error messages.
  IotWebConfParameter *current = this->_firstParameter;
  while(current != NULL)
  {
    current->errorMessage = NULL;
    current = current->_nextParameter;
  }

  // -- Call external validator.
  boolean valid = true;
  if (this->_formValidator != NULL)
  {
    valid = this->_formValidator();
  }

  // -- Internal validation.
  int l = this->_server->arg(this->_thingNameParameter.getId()).length();
  if (3 > l)
  {
    this->_thingNameParameter.errorMessage = "Gib mindestens 3 Zeichen ein.";
    valid = false;
  }
  l = this->_server->arg(this->_apPasswordParameter.getId()).length();
  if ((0 < l) && (l < 8))
  {
    this->_apPasswordParameter.errorMessage = "Das Passwort muss mindestens 8 Zeichen lang sein.";
    valid = false;
  }
  l = this->_server->arg(this->_wifiPasswordParameter.getId()).length();
  if ((0 < l) && (l < 8))
  {
    this->_wifiPasswordParameter.errorMessage = "Das Passwort muss mindestens 8 Zeichen lang sein.";
    valid = false;
  }

  return valid;
}


void IotWebConf::handleNotFound() {
  if (this->handleCaptivePortal()) { // If captive portal redirect instead of displaying the error page.
    return;
  }
#ifdef IOTWEBCONF_DEBUG_TO_SERIAL
  Serial.print("Nicht existierende Seite angefordert '");
  Serial.print(this->_server->uri());
  Serial.print("' Argumente(");
  Serial.print(this->_server->method() == HTTP_GET ? "GET" : "POST");
  Serial.print("):");
  Serial.println(this->_server->args());
#endif
  String message = "Seite nicht gefunden\n\n";
  message += "URI: ";
  message += this->_server->uri();
  message += "\nMethode: ";
  message += ( this->_server->method() == HTTP_GET ) ? "GET" : "POST";
  message += "\nArgumente: ";
  message += this->_server->args();
  message += "\n";

  for ( uint8_t i = 0; i < this->_server->args(); i++ ) {
    message += " " + this->_server->argName ( i ) + ": " + this->_server->arg ( i ) + "\n";
  }
  this->_server->sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  this->_server->sendHeader("Pragma", "no-cache");
  this->_server->sendHeader("Expires", "-1");
  this->_server->sendHeader("Content-Length", String(message.length()));
  this->_server->send ( 404, "text/plain", message );
}

/**
 * Redirect to captive portal if we got a request for another domain.
 * Return true in that case so the page handler do not try to handle the request again.
 * (Code from WifiManager project.)
 */
boolean IotWebConf::handleCaptivePortal() {
  String host = this->_server->hostHeader();
  String thingName = String(this->_thingName);
  thingName.toLowerCase();
  if (!isIp(host) && !host.startsWith(thingName)) {
#ifdef IOTWEBCONF_DEBUG_TO_SERIAL
    Serial.print("Anfrage für ");
    Serial.print(host);
    Serial.print(" umgeleitet nach ");
    Serial.println(this->_server->client().localIP());
#endif
    this->_server->sendHeader("Location", String("http://") + toStringIp(this->_server->client().localIP()), true);
    this->_server->send ( 302, "text/plain", ""); // Empty content inhibits Content-length header so we have to close the socket ourselves.
    this->_server->client().stop(); // Stop is needed because we sent no content length
    return true;
  }
  return false;
}

/** Is this an IP? */
boolean IotWebConf::isIp(String str) {
  for (size_t i = 0; i < str.length(); i++) {
    int c = str.charAt(i);
    if (c != '.' && (c < '0' || c > '9')) {
      return false;
    }
  }
  return true;
}

/** IP to String? */
String IotWebConf::toStringIp(IPAddress ip) {
  String res = "";
  for (int i = 0; i < 3; i++) {
    res += String((ip >> (8 * i)) & 0xFF) + ".";
  }
  res += String(((ip >> 8 * 3)) & 0xFF);
  return res;
}

/////////////////////////////////////////////////////////////////////////////////

void IotWebConf::delay(unsigned long m)
{
  unsigned long delayStart = millis();
  while(m > millis() - delayStart)
  {
    this->doLoop();
    delay(1);
  }
}

void IotWebConf::doLoop()
{
  doBlink();
  if (this->_state == IOTWEBCONF_STATE_BOOT)
  {
    // -- After boot, fall immediately to AP mode.
    this->changeState(IOTWEBCONF_STATE_AP_MODE);
  }
  else if (
    (this->_state ==IOTWEBCONF_STATE_NOT_CONFIGURED)
    || (this->_state == IOTWEBCONF_STATE_AP_MODE))
  {
    // -- We must only leave the AP mode, when no slaves are connected.
    // -- Other than that AP mode has a timeout. E.g. after boot, or when retry connecting to WiFi
    checkConnection();
    checkApTimeout();
    this->_dnsServer->processNextRequest();
    this->_server->handleClient();
  }
  else if (this->_state == IOTWEBCONF_STATE_CONNECTING)
  {
      if (checkWifiConnection())
      {
        this->changeState(IOTWEBCONF_STATE_ONLINE);
        return;
      }
  }
  else if (this->_state == IOTWEBCONF_STATE_ONLINE)
  {
    // -- In server mode we provide web interface. And check whether it is time to run the client.
    this->_server->handleClient();
    if (WiFi.status() != WL_CONNECTED)
    {
      this->changeState(IOTWEBCONF_STATE_CONNECTING);
      return;
    }
  }
}

/**
 * What happens, when a state changed...
 */
void IotWebConf::changeState(byte newState)
{
  switch(newState)
  {
    case IOTWEBCONF_STATE_AP_MODE:
    {
      // -- In AP mode we must override the default AP password. Otherwise we stay in STATE_NOT_CONFIGURED.
      if (this->_forceDefaultPassword || (this->_apPassword[0] == '\0'))
      {
#ifdef IOTWEBCONF_DEBUG_TO_SERIAL
        if (this->_forceDefaultPassword)
        {
          Serial.println("AP-Mode erzwungen durch Reset-Pin");
        }
        else
        {
          Serial.println("AP-Passwort wurde in der Konfig nicht gesetzt");
        }
#endif
        newState = IOTWEBCONF_STATE_NOT_CONFIGURED;
      }
      break;
    }
    default:
      break;
  }
#ifdef IOTWEBCONF_DEBUG_TO_SERIAL
  Serial.print("Statuswechsel von: ");
  Serial.print(this->_state);
  Serial.print(" zu ");
  Serial.println(newState);
#endif
  byte oldState = this->_state;
  this->_state = newState;
  this->stateChanged(oldState, newState);
#ifdef IOTWEBCONF_DEBUG_TO_SERIAL
  Serial.print("Statuswechsel von: ");
  Serial.print(oldState);
  Serial.print(" zu ");
  Serial.println(newState);
#endif
}

/**
 * What happens, when a state changed...
 */
void IotWebConf::stateChanged(byte oldState, byte newState)
{
//  updateOutput();
  switch(newState)
  {
    case IOTWEBCONF_STATE_AP_MODE:
    case IOTWEBCONF_STATE_NOT_CONFIGURED:
      if (newState == IOTWEBCONF_STATE_AP_MODE)
      {
        this->blinkInternal(300, 90);
      }
      else
      {
        this->blinkInternal(300, 50);
      }
      setupAp();
      if (this->_updateServer != NULL)
      {
        this->_updateServer->setup(this->_server, this->_updatePath);
      }
      this->_server->begin();
      this->_apConnectionStatus = IOTWEBCONF_AP_CONNECTION_STATE_NC;
      this->_apStartTimeMs = millis();
      break;
    case IOTWEBCONF_STATE_CONNECTING:
      if ((oldState == IOTWEBCONF_STATE_AP_MODE) || (oldState == IOTWEBCONF_STATE_NOT_CONFIGURED))
      {
        stopAp();
      }
      this->blinkInternal(1000, 50);
#ifdef IOTWEBCONF_DEBUG_TO_SERIAL
      Serial.print("Verbunden mit [");
      Serial.print(this->_wifiSsid);
      Serial.print("] mit Passwort [");
      Serial.print(this->_wifiPassword);
      Serial.println("]");
#endif
      this->_wifiConnectionStart = millis();
      WiFi.begin(this->_wifiSsid, this->_wifiPassword);
      break;
    case IOTWEBCONF_STATE_ONLINE:
      this->blinkInternal(8000, 2);
      if (this->_updateServer != NULL)
      {
        this->_updateServer->updateCredentials(IOTWEBCONF_ADMIN_USER_NAME, this->_apPassword);
      }
      this->_server->begin();
      IOTWEBCONF_DEBUG_LINE(F("Verbindung wird angenommen"));
      if (this->_wifiConnectionCallback != NULL)
      {
        this->_wifiConnectionCallback();
      }
      break;
    default:
      break;
  }
}

void IotWebConf::checkApTimeout()
{
  if ((this->_wifiSsid[0] != '\0')
    && (this->_apPassword[0] != '\0')
    && (!this->_forceDefaultPassword))
  {
    // -- Only move on, when we have a valid WifF and AP configured.
    if (
      (this->_apConnectionStatus == IOTWEBCONF_AP_CONNECTION_STATE_DC)
       ||
      ((this->_apTimeoutMs < millis() - this->_apStartTimeMs)
      && (this->_apConnectionStatus != IOTWEBCONF_AP_CONNECTION_STATE_C)))
    {
      this->changeState(IOTWEBCONF_STATE_CONNECTING);
    }
  }
}

/**
 * Checks whether we have anyone joined to our AP.
 * If so, we must not change state. But when our guest leaved, we can immediately move on.
 */
void IotWebConf::checkConnection()
{
  if ((this->_apConnectionStatus == IOTWEBCONF_AP_CONNECTION_STATE_NC)
    && (WiFi.softAPgetStationNum() > 0))
  {
    this->_apConnectionStatus = IOTWEBCONF_AP_CONNECTION_STATE_C;
    IOTWEBCONF_DEBUG_LINE(F("Verbindung mit AP."));
  }
  else if ((this->_apConnectionStatus == IOTWEBCONF_AP_CONNECTION_STATE_C)
    && (WiFi.softAPgetStationNum() == 0))
  {
    this->_apConnectionStatus = IOTWEBCONF_AP_CONNECTION_STATE_DC;
    IOTWEBCONF_DEBUG_LINE(F("Getrennt von AP."));
    if (this->_forceDefaultPassword)
    {
      IOTWEBCONF_DEBUG_LINE(F("Hebe gezwungenen AP-Mode auf."));
      this->_forceDefaultPassword = false;
    }
  }
}

boolean IotWebConf::checkWifiConnection()
{
  if (WiFi.status() != WL_CONNECTED) {
    if (this->_wifiConnectionTimeoutMs < millis() - this->_wifiConnectionStart)
    {
      // -- WiFi not available, fall back to AP mode.
      IOTWEBCONF_DEBUG_LINE(F("Gebe auf."));
      WiFi.disconnect(true);
      this->changeState(IOTWEBCONF_STATE_AP_MODE);
    }
    return false;
  }

  // -- Connected
#ifdef IOTWEBCONF_DEBUG_TO_SERIAL
  Serial.println("WLAN verbunden");
  Serial.print("IP-Adresse: ");
  Serial.println(WiFi.localIP());
#endif

  return true;
}

void IotWebConf::setupAp()
{
  WiFi.mode(WIFI_AP);

#ifdef IOTWEBCONF_DEBUG_TO_SERIAL
  Serial.print("Aktiviere AP-Modus: ");
  Serial.println(this->_thingName);
#endif
  if (this->_state == IOTWEBCONF_STATE_NOT_CONFIGURED)
  {
#ifdef IOTWEBCONF_DEBUG_TO_SERIAL
    Serial.print("Mit Initial-Passwort: ");
    Serial.println(this->_initialApPassword);
#endif
    WiFi.softAP(this->_thingName, this->_initialApPassword);
  }
  else
  {
#ifdef IOTWEBCONF_DEBUG_TO_SERIAL
    Serial.print("Benutze Passwort: ");
    Serial.println(this->_apPassword);
#endif
    WiFi.softAP(this->_thingName, this->_apPassword);
  }

#ifdef IOTWEBCONF_DEBUG_TO_SERIAL
  Serial.print(F("AP IP-Adresse: "));
  Serial.println(WiFi.softAPIP());
#endif
//  delay(500); // Without delay I've seen the IP address blank
//  Serial.print(F("AP IP address: "));
//  Serial.println(WiFi.softAPIP());

  /* Setup the DNS server redirecting all the domains to the apIP */
  this->_dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
  this->_dnsServer->start(IOTWEBCONF_DNS_PORT, "*", WiFi.softAPIP());
}

void IotWebConf::stopAp()
{
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
}

////////////////////////////////////////////////////////////////////

void IotWebConf::blink(unsigned long repeatMs, byte dutyCyclePercent)
{
  if (repeatMs == 0)
  {
    this->_blinkOnMs = this->_internalBlinkOnMs;
    this->_blinkOffMs = this->_internalBlinkOffMs;
  }
  else
  {
    this->_blinkOnMs = repeatMs * dutyCyclePercent / 100;
    this->_blinkOffMs = repeatMs * (100 - dutyCyclePercent) / 100;
  }
}

void IotWebConf::blinkInternal(unsigned long repeatMs, byte dutyCyclePercent)
{
  this->blink(repeatMs, dutyCyclePercent);
  this->_internalBlinkOnMs = this->_blinkOnMs;
  this->_internalBlinkOffMs = this->_blinkOffMs;
}

void IotWebConf::doBlink()
{
  if (IOTWEBCONF_STATUS_ENABLED)
  {
    unsigned long now = millis();
    unsigned long delayMs =
     this->_blinkState == LOW ?
      this->_blinkOnMs : this->_blinkOffMs;
    if (delayMs < now - this->_lastBlinkTime)
    {
      this->_blinkState = 1 - this->_blinkState;
      this->_lastBlinkTime = now;
      digitalWrite(this->_statusPin, this->_blinkState);
    }
  }
}
