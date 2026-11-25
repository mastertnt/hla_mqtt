// Configurable log of the RTI (#41), complementing the HLA_MQTT_TRACE mode
// (Trace.cpp, exceptions thrown by the DLL).
//
// Level: off, error, warning, info, debug (default: warning). Output: stderr,
// or appended to a file. Process-wide: set from the environment when the DLL
// loads (HLA_MQTT_LOG_LEVEL, HLA_MQTT_LOG_FILE), then by the logLevel and
// logFile settings of each connect (LocalSettings).
//
//   error    publication lost, connection lost for good
//   warning  connection lost (reconnecting), records ignored, callback errors
//   info     connect, disconnect, reconnection, join, resign, presence lost
//   debug    every publication and every message received
//
// Line format: <ISO 8601 UTC time> <LEVEL> [<clientId>] <message>
#pragma once

#include <sstream>
#include <string>

namespace hla_mqtt
{
namespace log
{
enum class Level
{
   Off = 0,
   Error = 1,
   Warning = 2,
   Info = 3,
   Debug = 4
};

// Throws std::invalid_argument on an unknown name.
Level parseLevel(std::string const& name);
char const* levelName(Level level);

void configure(Level level, std::string const& file); // file "": stderr
void setLevel(Level level);
void setFile(std::string const& file); // "": stderr
Level level();
inline bool enabled(Level l)
{
   return l != Level::Off && static_cast<int>(l) <= static_cast<int>(level());
}
void write(Level l, std::string const& clientId, std::string const& message);
} // namespace log
} // namespace hla_mqtt

// HLA_MQTT_LOG(Info, clientId, "joined " << name): the message is only built when enabled.
#define HLA_MQTT_LOG(lvl, clientId, expr)                                                                    \
   do                                                                                                        \
   {                                                                                                         \
      if (::hla_mqtt::log::enabled(::hla_mqtt::log::Level::lvl))                                             \
      {                                                                                                      \
         std::ostringstream hla_mqtt_log_stream_;                                                            \
         hla_mqtt_log_stream_ << expr;                                                                       \
         ::hla_mqtt::log::write(::hla_mqtt::log::Level::lvl, (clientId), hla_mqtt_log_stream_.str());        \
      }                                                                                                      \
   } while (false)
