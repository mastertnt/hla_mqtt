// Parsing of the `localSettingsDesignator` passed to RTIambassador::connect.
//
// Accepted forms (case-sensitive keys, ';' separated):
//   ""                                   -> env HLA_MQTT_SETTINGS, else defaults
//   "tcp://broker:1883"                  -> broker only
//   "mqtt://broker:1883;clientId=fed1;prefix=hla;connectTimeout=5"
//   "broker=mqtt://broker:1883;username=u;password=p"
//
// Keys: broker, clientId, prefix, connectTimeout (s), keepAlive (s),
//       username, password.
// Scheme mapping: mqtt:// -> tcp://, mqtts:// -> ssl://.
// Default broker: env HLA_MQTT_BROKER, else tcp://localhost:1883.
#pragma once

#include <string>

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

   // Throws std::invalid_argument with a readable message on error.
   static LocalSettings parse(std::wstring const& designator);
};
}
