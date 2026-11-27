// hla-openrti-run <prefix> <program> [args...]
//
// Runs one OpenRTI test program against this RTI on a clean topic prefix:
// removes the retained messages under <prefix>/ (left by an earlier run that
// failed before destroying its federation), runs the program with
// HLA_MQTT_SETTINGS=prefix=<prefix> (plus the previous HLA_MQTT_SETTINGS
// items, if any), then removes them again. Exit code: the program's.
// Broker: HLA_MQTT_BROKER, default tcp://localhost:1883.

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <mqtt/async_client.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
std::string broker()
{
   char const* env = std::getenv("HLA_MQTT_BROKER");
   std::string uri = env && *env ? env : "tcp://localhost:1883";
   if (uri.rfind("mqtt://", 0) == 0)
      uri = "tcp://" + uri.substr(7);
   return uri;
}

// Clears every retained message under prefix/.
void purge(std::string const& prefix)
{
   mqtt::async_client client(broker(), "hla-openrti-run-" + std::to_string(std::random_device()()));
   std::mutex m;
   std::set<std::string> topics;
   client.set_message_callback([&](mqtt::const_message_ptr msg) {
      if (msg->is_retained() && !msg->get_payload().empty())
      {
         std::lock_guard<std::mutex> l(m);
         topics.insert(msg->get_topic());
      }
   });
   client.connect(mqtt::connect_options_builder().clean_session(true).finalize())->wait_for(std::chrono::seconds(5));
   client.subscribe(prefix + "/#", 1)->wait_for(std::chrono::seconds(5));
   std::this_thread::sleep_for(std::chrono::milliseconds(500)); // retained messages arrive
   std::set<std::string> copy;
   {
      std::lock_guard<std::mutex> l(m);
      copy = topics;
   }
   for (std::string const& t : copy)
      client.publish(t, "", 0, 1, true)->wait_for(std::chrono::seconds(5));
   client.disconnect()->wait_for(std::chrono::seconds(5));
   if (!copy.empty())
      std::cout << "[hla-openrti-run] " << copy.size() << " retained messages removed under " << prefix << "/"
                << std::endl;
}

int run(std::vector<std::string> const& args)
{
#if defined(_WIN32)
   std::vector<std::string> quoted;
   for (std::string const& a : args)
   {
      std::string q = "\"";
      for (char c : a)
         q += (c == '"') ? std::string("\\\"") : std::string(1, c);
      quoted.push_back(q + "\"");
   }
   std::vector<char const*> argv;
   for (std::string const& q : quoted)
      argv.push_back(q.c_str());
   argv.push_back(nullptr);
   intptr_t const rc = _spawnv(_P_WAIT, args[0].c_str(), argv.data());
   if (rc == -1)
   {
      std::cerr << "[hla-openrti-run] cannot run " << args[0] << std::endl;
      return 127;
   }
   return int(rc);
#else
   pid_t const pid = fork();
   if (pid == 0)
   {
      std::vector<char*> argv;
      for (std::string const& a : args)
         argv.push_back(const_cast<char*>(a.c_str()));
      argv.push_back(nullptr);
      execv(args[0].c_str(), argv.data());
      _exit(127);
   }
   int status = 0;
   waitpid(pid, &status, 0);
   return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
#endif
}
} // namespace

int main(int argc, char* argv[])
{
   if (argc < 3)
   {
      std::cerr << "usage: hla-openrti-run <prefix> <program> [args...]" << std::endl;
      return 2;
   }
   std::string const prefix = argv[1];
   std::vector<std::string> args(argv + 2, argv + argc);
   try
   {
      purge(prefix);
   }
   catch (std::exception const& e)
   {
      std::cerr << "[hla-openrti-run] broker " << broker() << ": " << e.what() << std::endl;
      return 1;
   }
   std::string settings = "prefix=" + prefix;
   if (char const* previous = std::getenv("HLA_MQTT_SETTINGS"))
      if (*previous)
         settings = std::string(previous) + ";" + settings;
#if defined(_WIN32)
   _putenv_s("HLA_MQTT_SETTINGS", settings.c_str());
#else
   setenv("HLA_MQTT_SETTINGS", settings.c_str(), 1);
#endif
   int const rc = run(args);
   try
   {
      purge(prefix);
   }
   catch (std::exception const&)
   {
   }
   return rc;
}
