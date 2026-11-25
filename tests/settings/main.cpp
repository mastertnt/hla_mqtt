// hla-settings-test: configuration of a connection (LocalSettings) and log
// (Log), white-box, no broker: designator, configuration file, environment
// variables and their precedence, TLS keys, errors, log levels and file.
//
// Exit code 0 = all checks passed.

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include "LocalSettings.h"
#include "Log.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

using hla_mqtt::LocalSettings;
namespace logns = hla_mqtt::log;

namespace
{
int failures = 0;
int checks = 0;

void check(bool ok, std::string const& what)
{
   ++checks;
   if (!ok)
      ++failures;
   std::cout << (ok ? "[PASS] " : "[FAIL] ") << what << std::endl;
}

std::map<std::string, std::string> g_env; // fake environment

std::string fakeEnv(char const* name)
{
   auto it = g_env.find(name);
   return it == g_env.end() ? std::string() : it->second;
}

LocalSettings parse(std::wstring const& designator)
{
   return LocalSettings::parse(designator, &fakeEnv);
}

// Error message of a parse that must fail ("" if it did not).
std::string parseError(std::wstring const& designator)
{
   try
   {
      parse(designator);
   }
   catch (std::invalid_argument const& e)
   {
      return e.what();
   }
   return std::string();
}

void writeFile(std::string const& path, std::string const& content)
{
   std::ofstream(path) << content;
}

std::string readFile(std::string const& path)
{
   std::ifstream in(path);
   std::stringstream s;
   s << in.rdbuf();
   return s.str();
}
} // namespace

int main()
{
   // ---------------------------------------------------------- defaults
   {
      g_env.clear();
      LocalSettings const s = parse(L"");
      check(s.brokerUri == "tcp://localhost:1883" && s.topicPrefix == "hla" && s.connectTimeoutSeconds == 5 &&
               s.keepAliveSeconds == 20 && s.reconnectSeconds == 30 && s.staleClientDelay() == 70 &&
               s.verifyServer && !s.tls(),
            "defaults");
      check(s.clientId.rfind("hla-", 0) == 0, "generated clientId");
   }

   // -------------------------------------------------------- designator
   {
      LocalSettings const s = parse(L"mqtt://broker:1884;clientId=fed1;prefix=p;connectTimeout=7;keepAlive=9;"
                                    L"username=u;password=pw;reconnect=0;staleClientDelay=3");
      check(s.brokerUri == "tcp://broker:1884" && s.clientId == "fed1" && s.topicPrefix == "p" &&
               s.connectTimeoutSeconds == 7 && s.keepAliveSeconds == 9 && s.username == "u" && s.password == "pw" &&
               s.reconnectSeconds == 0 && s.staleClientDelay() == 3,
            "every key from the designator");
      check(parse(L"broker=mqtts://secure").brokerUri == "ssl://secure:8883", "mqtts:// -> ssl://, port 8883");
      check(parse(L"tcp://h").brokerUri == "tcp://h:1883", "default port 1883");
      check(parse(L"mqtts://h;caFile=ca.pem;certFile=c.pem;keyFile=k.pem;keyPassword=x;verifyServer=false")
                  .certFile == "c.pem",
            "TLS keys accepted with an ssl broker");
      LocalSettings const t = parse(L"mqtts://h;caFile=ca.pem;caPath=cas;certFile=c.pem;keyFile=k.pem;keyPassword=x;"
                                    L"verifyServer=no");
      check(t.tls() && t.caFile == "ca.pem" && t.caPath == "cas" && t.keyFile == "k.pem" && t.keyPassword == "x" &&
               !t.verifyServer,
            "TLS settings");
   }

   // ------------------------------------------------------------ errors
   check(parseError(L"foo=1").find("unknown setting: foo") != std::string::npos, "unknown key");
   check(parseError(L"keepAlive=abc").find("invalid integer") != std::string::npos, "invalid integer");
   check(parseError(L"verifyServer=maybe").find("invalid boolean") != std::string::npos, "invalid boolean");
   check(parseError(L"http://h").find("unsupported broker URI scheme") != std::string::npos, "unsupported scheme");
   check(parseError(L"prefix=a/#").find("invalid topic prefix") != std::string::npos, "invalid prefix");
   check(parseError(L"tcp://h;caFile=ca.pem").find("TLS settings need") != std::string::npos,
         "TLS keys with a plain broker");
   check(parseError(L"mqtts://h;keyFile=k.pem").find("keyFile needs certFile") != std::string::npos,
         "keyFile without certFile");
   check(parseError(L"logLevel=loud").find("unknown log level") != std::string::npos, "invalid log level");
   check(parseError(L"config=no-such-file.conf").find("cannot read configuration file") != std::string::npos,
         "missing configuration file");

   // ------------------------------------------------- environment, file
   std::string const conf = "hla-settings-test.conf";
   writeFile(conf, "# comment\n"
                   "broker = mqtt://from-file:1000   # trailing comment\n"
                   "prefix=fileprefix\n"
                   "\n"
                   "keepAlive=11\n"
                   "clientId=file-client\n");
   {
      g_env = {{"HLA_MQTT_CONFIG", conf}};
      LocalSettings const s = parse(L"");
      check(s.brokerUri == "tcp://from-file:1000" && s.topicPrefix == "fileprefix" && s.keepAliveSeconds == 11 &&
               s.clientId == "file-client",
            "HLA_MQTT_CONFIG file");
      g_env["HLA_MQTT_PREFIX"] = "envprefix";
      g_env["HLA_MQTT_KEEP_ALIVE"] = "12";
      LocalSettings const e = parse(L"");
      check(e.topicPrefix == "envprefix" && e.keepAliveSeconds == 12 && e.brokerUri == "tcp://from-file:1000",
            "HLA_MQTT_<KEY> overrides the file");
      LocalSettings const d = parse(L"keepAlive=13");
      check(d.keepAliveSeconds == 13 && d.topicPrefix == "envprefix", "the designator overrides the environment");
      g_env["HLA_MQTT_SETTINGS"] = "keepAlive=14";
      check(parse(L"").keepAliveSeconds == 14, "HLA_MQTT_SETTINGS when the designator is empty");
      check(parse(L"keepAlive=15").keepAliveSeconds == 15, "HLA_MQTT_SETTINGS ignored with a designator");
      g_env["HLA_MQTT_KEEP_ALIVE"] = "x";
      check(parseError(L"").find("HLA_MQTT_KEEP_ALIVE") != std::string::npos,
            "invalid environment value names its variable");
   }
   {
      g_env.clear();
      LocalSettings const s = parse(L"keepAlive=21;config=" + std::wstring(conf.begin(), conf.end()) + L";prefix=last");
      check(s.keepAliveSeconds == 11 && s.topicPrefix == "last" && s.brokerUri == "tcp://from-file:1000",
            "config=<file> applied at its place in the designator");
      writeFile(conf, "keepAlive=1\nbogus=2\n");
      std::string const err = parseError(L"config=" + std::wstring(conf.begin(), conf.end()));
      check(err.find(conf + ":2:") != std::string::npos && err.find("unknown setting: bogus") != std::string::npos,
            "file error names the file and the line");
      std::remove(conf.c_str());
   }
   {
      g_env.clear();
      bool all = true;
      for (auto const& kv : LocalSettings::keys())
         all = all && kv.second.rfind("HLA_MQTT_", 0) == 0;
      check(all && LocalSettings::keys().size() == 17, "17 keys, each with its HLA_MQTT_ variable");
   }

   // --------------------------------------------------------------- log
   {
      check(logns::parseLevel("DEBUG") == logns::Level::Debug && logns::parseLevel("warn") == logns::Level::Warning &&
               logns::parseLevel("off") == logns::Level::Off,
            "log level names");
      std::string const file = "hla-settings-test.log";
      std::remove(file.c_str());
      logns::configure(logns::Level::Info, file);
      HLA_MQTT_LOG(Info, "client-1", "hello " << 42);
      HLA_MQTT_LOG(Debug, "client-1", "hidden");
      HLA_MQTT_LOG(Error, "client-1", "bad");
      std::string const text = readFile(file);
      check(text.find("INFO [client-1] hello 42\n") != std::string::npos &&
               text.find("ERROR [client-1] bad\n") != std::string::npos && text.find("hidden") == std::string::npos,
            "log file: levels up to info, format");
      check(text.size() > 24 && text[4] == '-' && text[10] == 'T' && text.find("Z INFO") != std::string::npos,
            "log line starts with an ISO 8601 UTC time");
      logns::setLevel(logns::Level::Off);
      HLA_MQTT_LOG(Error, "client-1", "silenced");
      check(readFile(file).find("silenced") == std::string::npos, "level off: nothing written");
      logns::configure(logns::Level::Warning, "");
      std::remove(file.c_str());
   }

   std::cout << (checks - failures) << "/" << checks << " checks passed" << std::endl;
   return failures == 0 ? 0 : 1;
}
