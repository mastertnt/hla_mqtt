// Configuration of a connection: the `localSettingsDesignator` passed to
// RTIambassador::connect, a configuration file and environment variables
// (README, "Configuration").
//
// Sources, each one overriding the previous ones, key by key:
//   1. defaults
//   2. the file named by HLA_MQTT_CONFIG
//   3. the variables HLA_MQTT_<KEY> (HLA_MQTT_BROKER, HLA_MQTT_CLIENT_ID, ...)
//   4. the designator, or HLA_MQTT_SETTINGS when the designator is empty;
//      a "config=<file>" item loads that file at its place in the list.
//
// Designator: items separated by ';', key=value, the first item may be a
// bare broker URI:
//   "mqtt://broker:1883;clientId=fed1;prefix=hla;connectTimeout=5"
// File: one key=value per line, '#' starts a comment.
//
// Keys (environment variable in parentheses):
//   broker (HLA_MQTT_BROKER)                 tcp:// mqtt:// ssl:// mqtts:// ws:// wss://
//   clientId (HLA_MQTT_CLIENT_ID)            generated when empty
//   prefix (HLA_MQTT_PREFIX)                 topic prefix, "hla"
//   connectTimeout (HLA_MQTT_CONNECT_TIMEOUT) s
//   keepAlive (HLA_MQTT_KEEP_ALIVE)          s
//   username, password (HLA_MQTT_USERNAME, HLA_MQTT_PASSWORD)
//   reconnect (HLA_MQTT_RECONNECT)           s
//   staleClientDelay (HLA_MQTT_STALE_CLIENT_DELAY) s
//   caFile, caPath (HLA_MQTT_CA_FILE, HLA_MQTT_CA_PATH)       TLS: trusted CAs (PEM file, directory)
//   certFile, keyFile (HLA_MQTT_CERT_FILE, HLA_MQTT_KEY_FILE) TLS: client certificate and its key (PEM)
//   keyPassword (HLA_MQTT_KEY_PASSWORD)      TLS: password of an encrypted key
//   verifyServer (HLA_MQTT_VERIFY_SERVER)    TLS: check the server certificate and host name (true)
//   logLevel (HLA_MQTT_LOG_LEVEL)            off, error, warning, info, debug
//   logFile (HLA_MQTT_LOG_FILE)              log appended to this file (stderr when empty)
// Scheme mapping: mqtt:// -> tcp://, mqtts:// -> ssl://.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace hla_mqtt
{
struct LocalSettings
{
   std::string brokerUri = "tcp://localhost:1883";
   std::string clientId;           // generated when empty
   std::string topicPrefix = "hla";
   int connectTimeoutSeconds = 5;
   int keepAliveSeconds = 20;
   std::string username;
   std::string password;
   // After an unexpected loss of the broker connection, reconnection is tried
   // for this long before connectionLost (0: connectionLost at once).
   int reconnectSeconds = 30;
   // A client whose presence stays "offline" (Last Will) this long has its
   // retained presence removed; -1: reconnect + 2 x keepAlive.
   int staleClientDelaySeconds = -1;
   int staleClientDelay() const
   {
      return staleClientDelaySeconds >= 0 ? staleClientDelaySeconds : reconnectSeconds + 2 * keepAliveSeconds;
   }
   // TLS (ssl:// and wss:// brokers).
   std::string caFile, caPath;
   std::string certFile, keyFile, keyPassword;
   bool verifyServer = true;
   bool tls() const { return brokerUri.rfind("ssl://", 0) == 0 || brokerUri.rfind("wss://", 0) == 0; }
   // Log (Log.h); empty: unchanged.
   std::string logLevel;
   std::string logFile;
   bool logFileSet = false;

   // Throws std::invalid_argument with a readable message on error.
   static LocalSettings parse(std::wstring const& designator);
   // The same with an explicit environment (tests): returns "" for an unset variable.
   using Environment = std::string (*)(char const* name);
   static LocalSettings parse(std::wstring const& designator, Environment env);
   // Keys and their environment variables, in documentation order.
   static std::vector<std::pair<std::string, std::string>> keys();
};
}
