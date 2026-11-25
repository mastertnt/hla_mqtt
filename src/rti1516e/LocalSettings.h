// Parsing of the `localSettingsDesignator` passed to RTIambassador::connect.
//
// Accepted forms (case-sensitive keys, ';' separated):
//   ""                                   -> env HLA_MQTT_SETTINGS, else defaults
//   "tcp://broker:1883"                  -> broker only
//   "mqtt://broker:1883;clientId=fed1;prefix=hla;connectTimeout=5"
//   "broker=mqtt://broker:1883;username=u;password=p"
//
// Keys: broker, clientId, prefix, connectTimeout (s), keepAlive (s),
//       username, password, reconnect (s), staleClientDelay (s).
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

   // Throws std::invalid_argument with a readable message on error.
   static LocalSettings parse(std::wstring const& designator);
};
}
