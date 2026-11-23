#include "LocalSettings.h"

#include "Utf.h"

#include <cstdlib>
#include <random>
#include <sstream>
#include <stdexcept>

namespace hla_mqtt
{
namespace
{
std::string trim(std::string const& s)
{
   size_t b = s.find_first_not_of(" \t\r\n");
   if (b == std::string::npos)
      return {};
   size_t e = s.find_last_not_of(" \t\r\n");
   return s.substr(b, e - b + 1);
}

std::string getEnv(char const* name)
{
#if defined(_MSC_VER)
   char* value = nullptr;
   size_t len = 0;
   std::string result;
   if (_dupenv_s(&value, &len, name) == 0 && value != nullptr)
   {
      result = value;
      std::free(value);
   }
   return result;
#else
   char const* value = std::getenv(name);
   return value ? std::string(value) : std::string();
#endif
}

std::string normalizeUri(std::string uri)
{
   if (uri.rfind("mqtt://", 0) == 0)
      uri = "tcp://" + uri.substr(7);
   else if (uri.rfind("mqtts://", 0) == 0)
      uri = "ssl://" + uri.substr(8);
   if (uri.rfind("tcp://", 0) != 0 && uri.rfind("ssl://", 0) != 0 &&
       uri.rfind("ws://", 0) != 0 && uri.rfind("wss://", 0) != 0)
      throw std::invalid_argument("unsupported broker URI scheme: " + uri);
   if (uri.find(':', uri.find("://") + 3) == std::string::npos)
      uri += (uri.rfind("ssl://", 0) == 0) ? ":8883" : ":1883";
   return uri;
}

int toInt(std::string const& key, std::string const& value)
{
   try
   {
      size_t pos = 0;
      int v = std::stoi(value, &pos);
      if (pos != value.size() || v < 0)
         throw std::invalid_argument("");
      return v;
   }
   catch (std::exception const&)
   {
      throw std::invalid_argument("invalid integer for '" + key + "': " + value);
   }
}

std::string randomClientId()
{
   std::random_device rd;
   std::ostringstream s;
   s << "hla-" << std::hex << rd() << rd();
   return s.str();
}
}

LocalSettings LocalSettings::parse(std::wstring const& designator)
{
   LocalSettings settings;

   std::string envBroker = getEnv("HLA_MQTT_BROKER");
   if (!envBroker.empty())
      settings.brokerUri = normalizeUri(envBroker);

   std::string text = trim(toUtf8(designator));
   if (text.empty())
      text = trim(getEnv("HLA_MQTT_SETTINGS"));

   std::istringstream stream(text);
   std::string item;
   bool first = true;
   while (std::getline(stream, item, ';'))
   {
      item = trim(item);
      if (item.empty())
         continue;
      size_t eq = item.find('=');
      bool looksLikeUri = item.find("://") != std::string::npos &&
                          (eq == std::string::npos || eq > item.find("://"));
      if (first && looksLikeUri)
      {
         settings.brokerUri = normalizeUri(item);
         first = false;
         continue;
      }
      first = false;
      if (eq == std::string::npos)
         throw std::invalid_argument("expected key=value, got: " + item);
      std::string key = trim(item.substr(0, eq));
      std::string value = trim(item.substr(eq + 1));
      if (key == "broker")
         settings.brokerUri = normalizeUri(value);
      else if (key == "clientId")
         settings.clientId = value;
      else if (key == "prefix")
         settings.topicPrefix = value;
      else if (key == "connectTimeout")
         settings.connectTimeoutSeconds = toInt(key, value);
      else if (key == "keepAlive")
         settings.keepAliveSeconds = toInt(key, value);
      else if (key == "username")
         settings.username = value;
      else if (key == "password")
         settings.password = value;
      else
         throw std::invalid_argument("unknown setting: " + key);
   }

   if (settings.clientId.empty())
      settings.clientId = randomClientId();
   if (settings.topicPrefix.empty() || settings.topicPrefix.find_first_of("#+") != std::string::npos)
      throw std::invalid_argument("invalid topic prefix: " + settings.topicPrefix);
   return settings;
}
}
