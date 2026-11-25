#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS // getenv, fopen, gmtime
#endif

#include "Log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <stdexcept>

namespace hla_mqtt
{
namespace log
{
namespace
{
std::atomic<int> g_level{static_cast<int>(Level::Warning)};
std::mutex g_mutex;
std::string g_file; // g_mutex; "" = stderr

std::string lower(std::string s)
{
   for (char& c : s)
      if (c >= 'A' && c <= 'Z')
         c = char(c - 'A' + 'a');
   return s;
}

std::string timestamp()
{
   auto const now = std::chrono::system_clock::now();
   std::time_t const t = std::chrono::system_clock::to_time_t(now);
   long long const ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
   std::tm tm{};
#if defined(_WIN32)
   gmtime_s(&tm, &t);
#else
   gmtime_r(&t, &tm);
#endif
   char buf[40];
   std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ", tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
   return buf;
}

// Environment, when the DLL loads.
struct EnvConfig
{
   EnvConfig()
   {
      char const* l = std::getenv("HLA_MQTT_LOG_LEVEL");
      char const* f = std::getenv("HLA_MQTT_LOG_FILE");
      Level level = Level::Warning;
      try
      {
         if (l && *l)
            level = parseLevel(l);
      }
      catch (std::invalid_argument const&)
      {
      }
      configure(level, f ? f : "");
   }
} g_env;
} // namespace

Level parseLevel(std::string const& name)
{
   std::string const n = lower(name);
   if (n == "off" || n == "none" || n == "0")
      return Level::Off;
   if (n == "error")
      return Level::Error;
   if (n == "warning" || n == "warn")
      return Level::Warning;
   if (n == "info")
      return Level::Info;
   if (n == "debug")
      return Level::Debug;
   throw std::invalid_argument("unknown log level: " + name + " (off, error, warning, info, debug)");
}

char const* levelName(Level level)
{
   switch (level)
   {
   case Level::Off:
      return "OFF";
   case Level::Error:
      return "ERROR";
   case Level::Warning:
      return "WARNING";
   case Level::Info:
      return "INFO";
   case Level::Debug:
      return "DEBUG";
   }
   return "?";
}

void configure(Level level, std::string const& file)
{
   std::lock_guard<std::mutex> lock(g_mutex);
   g_level = static_cast<int>(level);
   g_file = file;
}

void setLevel(Level level)
{
   g_level = static_cast<int>(level);
}

void setFile(std::string const& file)
{
   std::lock_guard<std::mutex> lock(g_mutex);
   g_file = file;
}

Level level()
{
   return static_cast<Level>(g_level.load());
}

void write(Level l, std::string const& clientId, std::string const& message)
{
   if (!enabled(l))
      return;
   std::string const line = timestamp() + " " + levelName(l) + " [" + clientId + "] " + message + "\n";
   std::lock_guard<std::mutex> lock(g_mutex);
   if (!g_file.empty())
   {
      if (FILE* f = std::fopen(g_file.c_str(), "a"))
      {
         std::fputs(line.c_str(), f);
         std::fclose(f);
         return;
      }
   }
   std::fputs(line.c_str(), stderr); // also when the file cannot be opened
   std::fflush(stderr);
}
} // namespace log
} // namespace hla_mqtt
