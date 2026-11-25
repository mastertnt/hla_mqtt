#include "LocalSettings.h"

#include "Log.h"
#include "Utf.h"

#include <cstdlib>
#include <fstream>
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

std::string processEnv(char const* name)
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

bool toBool(std::string const& key, std::string const& value)
{
   std::string v;
   for (char c : value)
      v += (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
   if (v == "true" || v == "1" || v == "yes" || v == "on")
      return true;
   if (v == "false" || v == "0" || v == "no" || v == "off")
      return false;
   throw std::invalid_argument("invalid boolean for '" + key + "': " + value);
}

std::string randomClientId()
{
   std::random_device rd;
   std::ostringstream s;
   s << "hla-" << std::hex << rd() << rd();
   return s.str();
}

// key -> environment variable
std::vector<std::pair<std::string, std::string>> const kKeys = {
   {"broker", "HLA_MQTT_BROKER"},
   {"clientId", "HLA_MQTT_CLIENT_ID"},
   {"prefix", "HLA_MQTT_PREFIX"},
   {"connectTimeout", "HLA_MQTT_CONNECT_TIMEOUT"},
   {"keepAlive", "HLA_MQTT_KEEP_ALIVE"},
   {"username", "HLA_MQTT_USERNAME"},
   {"password", "HLA_MQTT_PASSWORD"},
   {"reconnect", "HLA_MQTT_RECONNECT"},
   {"staleClientDelay", "HLA_MQTT_STALE_CLIENT_DELAY"},
   {"caFile", "HLA_MQTT_CA_FILE"},
   {"caPath", "HLA_MQTT_CA_PATH"},
   {"certFile", "HLA_MQTT_CERT_FILE"},
   {"keyFile", "HLA_MQTT_KEY_FILE"},
   {"keyPassword", "HLA_MQTT_KEY_PASSWORD"},
   {"verifyServer", "HLA_MQTT_VERIFY_SERVER"},
   {"logLevel", "HLA_MQTT_LOG_LEVEL"},
   {"logFile", "HLA_MQTT_LOG_FILE"},
};

void apply(LocalSettings& s, std::string const& key, std::string const& value)
{
   if (key == "broker")
      s.brokerUri = normalizeUri(value);
   else if (key == "clientId")
      s.clientId = value;
   else if (key == "prefix")
      s.topicPrefix = value;
   else if (key == "connectTimeout")
      s.connectTimeoutSeconds = toInt(key, value);
   else if (key == "keepAlive")
      s.keepAliveSeconds = toInt(key, value);
   else if (key == "username")
      s.username = value;
   else if (key == "password")
      s.password = value;
   else if (key == "reconnect")
      s.reconnectSeconds = toInt(key, value);
   else if (key == "staleClientDelay")
      s.staleClientDelaySeconds = toInt(key, value);
   else if (key == "caFile")
      s.caFile = value;
   else if (key == "caPath")
      s.caPath = value;
   else if (key == "certFile")
      s.certFile = value;
   else if (key == "keyFile")
      s.keyFile = value;
   else if (key == "keyPassword")
      s.keyPassword = value;
   else if (key == "verifyServer")
      s.verifyServer = toBool(key, value);
   else if (key == "logLevel")
   {
      log::parseLevel(value); // validated here
      s.logLevel = value;
   }
   else if (key == "logFile")
   {
      s.logFile = value;
      s.logFileSet = true;
   }
   else
      throw std::invalid_argument("unknown setting: " + key);
}

void applyFile(LocalSettings& s, std::string const& path)
{
   std::ifstream in(path);
   if (!in)
      throw std::invalid_argument("cannot read configuration file: " + path);
   std::string line;
   int n = 0;
   while (std::getline(in, line))
   {
      ++n;
      size_t const hash = line.find('#');
      if (hash != std::string::npos)
         line.erase(hash);
      line = trim(line);
      if (line.empty())
         continue;
      size_t const eq = line.find('=');
      try
      {
         if (eq == std::string::npos)
            throw std::invalid_argument("expected key=value, got: " + line);
         std::string const key = trim(line.substr(0, eq));
         if (key == "config")
            throw std::invalid_argument("'config' is not allowed in a configuration file");
         apply(s, key, trim(line.substr(eq + 1)));
      }
      catch (std::invalid_argument const& e)
      {
         throw std::invalid_argument(path + ":" + std::to_string(n) + ": " + e.what());
      }
   }
}
} // namespace

std::vector<std::pair<std::string, std::string>> LocalSettings::keys()
{
   return kKeys;
}

LocalSettings LocalSettings::parse(std::wstring const& designator)
{
   return parse(designator, &processEnv);
}

LocalSettings LocalSettings::parse(std::wstring const& designator, Environment env)
{
   LocalSettings settings;

   std::string const file = trim(env("HLA_MQTT_CONFIG"));
   if (!file.empty())
      applyFile(settings, file);

   for (auto const& kv : kKeys)
   {
      std::string const value = trim(env(kv.second.c_str()));
      if (value.empty())
         continue;
      try
      {
         apply(settings, kv.first, value);
      }
      catch (std::invalid_argument const& e)
      {
         throw std::invalid_argument(kv.second + ": " + e.what());
      }
   }

   std::string text = trim(toUtf8(designator));
   if (text.empty())
      text = trim(env("HLA_MQTT_SETTINGS"));

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
      if (key == "config")
         applyFile(settings, value);
      else
         apply(settings, key, value);
   }

   if (settings.clientId.empty())
      settings.clientId = randomClientId();
   if (settings.topicPrefix.empty() || settings.topicPrefix.find_first_of("#+") != std::string::npos)
      throw std::invalid_argument("invalid topic prefix: " + settings.topicPrefix);
   if (!settings.tls() && (!settings.caFile.empty() || !settings.caPath.empty() || !settings.certFile.empty() ||
                           !settings.keyFile.empty()))
      throw std::invalid_argument("TLS settings need an ssl://, mqtts:// or wss:// broker, not " + settings.brokerUri);
   if (!settings.keyFile.empty() && settings.certFile.empty())
      throw std::invalid_argument("keyFile needs certFile");
   return settings;
}
}
