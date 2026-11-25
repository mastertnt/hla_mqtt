// hla-robustness-test: operation over an unreliable broker connection.
//   - reconnection (issue #38): the connection of a federate is cut by a raw
//     MQTT client that takes its client id over; the federate reconnects
//     without connectionLost and the federation goes on: the others see its
//     objects again, both directions of updates flow, and an object deleted
//     during the outage is removed (its retained record did not come again);
//   - reconnect=0: connectionLost at once;
//   - stale presences (issue #3): a federate process killed without resign
//     leaves an "offline" presence (Last Will), removed by the others after
//     staleClientDelay.
// Needs a broker.
//
//   hla-robustness-test [--broker mqtt://host:1883]
//
// Exit code 0 = all checks passed.

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS // std::getenv
#endif

#include <RTI/NullFederateAmbassador.h>
#include <RTI/RTI1516.h>
#include <RTI/encoding/BasicDataElements.h>

#include <mqtt/async_client.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifndef HLA_TEST_FOM_DIR
#define HLA_TEST_FOM_DIR "."
#endif

using namespace rti1516e;

namespace
{
int failures = 0;
int checks = 0;
std::string broker = "mqtt://localhost:1883";

void check(bool ok, std::string const& what)
{
   ++checks;
   if (!ok)
      ++failures;
   std::cout << (ok ? "[PASS] " : "[FAIL] ") << what << std::endl;
}

std::wstring widen(std::string const& s)
{
   return std::wstring(s.begin(), s.end());
}

std::string pahoUri()
{
   return broker.rfind("mqtt://", 0) == 0 ? "tcp://" + broker.substr(7) : broker;
}

// Raw MQTT client: takes a client id over, watches presence records.
struct Raw
{
   mqtt::async_client client;
   std::mutex mutex;
   std::map<std::string, std::vector<std::string>> seen; // topic -> payloads, in order

   explicit Raw(std::string const& id) : client(pahoUri(), id, mqtt::create_options(MQTTVERSION_3_1_1))
   {
      client.set_message_callback([this](mqtt::const_message_ptr m) {
         std::lock_guard<std::mutex> lock(mutex);
         seen[m->get_topic()].push_back(m->get_payload_str());
      });
   }
   void connect(bool clean)
   {
      client.connect(mqtt::connect_options_builder().clean_session(clean).finalize())->wait_for(std::chrono::seconds(5));
   }
   void disconnect()
   {
      try
      {
         client.disconnect()->wait_for(std::chrono::seconds(5));
      }
      catch (std::exception const&)
      {
      }
   }
   std::vector<std::string> payloads(std::string const& topic)
   {
      std::lock_guard<std::mutex> lock(mutex);
      return seen[topic];
   }
};

bool waitUntil(std::function<bool()> done, double seconds)
{
   auto const deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
   while (!done())
   {
      if (std::chrono::steady_clock::now() > deadline)
         return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
   }
   return true;
}

bool isOnline(std::string const& p)
{
   return p.find("\"state\":\"online\"") != std::string::npos;
}
bool isOffline(std::string const& p)
{
   return p.find("\"state\":\"offline\"") != std::string::npos;
}

struct Federate : NullFederateAmbassador
{
   std::unique_ptr<RTIambassador> rti;
   std::map<ObjectInstanceHandle, ObjectClassHandle> discovered;
   std::map<ObjectInstanceHandle, int> reflections;
   int lost = 0;

   Federate(std::string const& clientId, std::string const& extra = std::string())
   {
      rti = RTIambassadorFactory().createRTIambassador();
      rti->connect(*this, HLA_EVOKED, widen(broker + ";clientId=" + clientId + extra));
   }

   void discoverObjectInstance(ObjectInstanceHandle o, ObjectClassHandle c, std::wstring const&)
      RTI_THROW((FederateInternalError)) override
   {
      discovered[o] = c;
   }
   void removeObjectInstance(ObjectInstanceHandle o, VariableLengthData const&, OrderType, SupplementalRemoveInfo)
      RTI_THROW((FederateInternalError)) override
   {
      discovered.erase(o);
   }
   void reflectAttributeValues(ObjectInstanceHandle o, AttributeHandleValueMap const&, VariableLengthData const&,
                               OrderType, TransportationType, SupplementalReflectInfo)
      RTI_THROW((FederateInternalError)) override
   {
      ++reflections[o];
   }
   void connectionLost(std::wstring const&) RTI_THROW((FederateInternalError)) override { ++lost; }
};

// Evokes callbacks on every federate until done() or the timeout; `tick`
// runs between rounds (updates sent while waiting).
bool pump(std::vector<Federate*> const& feds, std::function<bool()> done, double seconds,
          std::function<void()> tick = nullptr)
{
   auto const deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
   while (!done())
   {
      if (std::chrono::steady_clock::now() > deadline)
         return false;
      for (Federate* f : feds)
         f->rti->evokeMultipleCallbacks(0.01, 0.05);
      if (tick)
         tick();
   }
   return true;
}

// Child process: joins, then dies without resign or disconnect.
int child(std::string const& fed, std::string const& clientId)
{
   Federate f(clientId);
   f.rti->joinFederationExecution(L"doomed", L"t", widen(fed));
   std::cout.flush();
   std::_Exit(0);
}
} // namespace

int main(int argc, char** argv)
{
   for (int i = 1; i < argc; ++i)
      if (std::string(argv[i]) == "--broker" && i + 1 < argc)
         broker = argv[++i];
   if (char const* env = std::getenv("HLA_MQTT_BROKER"))
      broker = env;
   if (argc == 4 && std::string(argv[1]) == "--child")
   {
      try
      {
         return child(argv[2], argv[3]);
      }
      catch (Exception const&)
      {
         return 2;
      }
   }

   std::random_device rd;
   std::string const run = std::to_string(rd() % 1000000);
   std::string const fedName = "robust-" + run;
   std::wstring const fed = widen(fedName);
   std::cout << "federation: " << fedName << std::endl;

   try
   {
      std::string const idA = "robust-a-" + run, idB = "robust-b-" + run;
      Federate a(idA, ";reconnect=10"), b(idB);
      a.rti->createFederationExecution(fed, widen(std::string(HLA_TEST_FOM_DIR) + "/base.xml"));
      a.rti->joinFederationExecution(L"a", L"t", fed);
      b.rti->joinFederationExecution(L"b", L"t", fed);

      ObjectClassHandle const vehicle = a.rti->getObjectClassHandle(L"HLAobjectRoot.Vehicle");
      AttributeHandle const name = a.rti->getAttributeHandle(vehicle, L"Name");
      AttributeHandleSet attrs;
      attrs.insert(name);
      for (Federate* f : {&a, &b})
      {
         f->rti->publishObjectClassAttributes(vehicle, attrs);
         f->rti->subscribeObjectClassAttributes(vehicle, attrs);
      }
      ObjectInstanceHandle const oa = a.rti->registerObjectInstance(vehicle);
      ObjectInstanceHandle const ob = b.rti->registerObjectInstance(vehicle);
      check(pump({&a, &b}, [&] { return b.discovered.count(oa) && a.discovered.count(ob); }, 5.0),
            "a and b discover each other's object");

      auto update = [&](Federate& f, ObjectInstanceHandle o) {
         AttributeHandleValueMap v;
         v[name] = HLAunicodeString(L"x").encode();
         try
         {
            f.rti->updateAttributeValues(o, v, VariableLengthData());
         }
         catch (Exception const&)
         {
         }
      };

      Raw watch("robust-watch-" + run);
      watch.connect(true);
      std::string const presenceA = "hla/clients/" + idA;
      watch.client.subscribe(presenceA, 1)->wait_for(std::chrono::seconds(5));

      // ------------------------------------------- reconnection, two rounds
      // The thief takes over with a clean session, then a persistent one; in
      // the second round, b deletes an object while a is away (a reconnects
      // after about a second): a learns it from the missing retained record.
      for (bool clean : {true, false})
      {
         std::string const round = clean ? " (takeover, clean session)" : " (takeover, persistent session)";
         size_t const before = watch.payloads(presenceA).size();
         ObjectInstanceHandle gone;
         if (!clean)
         {
            gone = b.rti->registerObjectInstance(vehicle);
            check(pump({&a, &b}, [&] { return a.discovered.count(gone) != 0; }, 5.0), "a discovers b's second object");
         }
         {
            Raw thief(idA); // takes a's client id over: the broker drops a
            thief.connect(clean);
            if (!clean)
               b.rti->deleteObjectInstance(gone, VariableLengthData());
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            thief.disconnect();
         }
         if (!clean)
            check(pump({&a, &b}, [&] { return a.discovered.count(gone) == 0; }, 10.0),
                  "object deleted during the outage removed after reconnection");
         int const reflB = b.reflections[oa], reflA = a.reflections[ob];
         bool const flowing = pump(
            {&a, &b},
            [&] { return b.discovered.count(oa) && b.reflections[oa] > reflB && a.reflections[ob] > reflA; }, 15.0,
            [&] {
               update(a, oa);
               update(b, ob);
            });
         check(a.lost == 0, "no connectionLost while reconnecting" + round);
         check(flowing, "after reconnection, b sees a's object and both directions of updates flow" + round);
         std::vector<std::string> const p = watch.payloads(presenceA);
         check(p.size() > before && isOnline(p.back()), "a's presence is online again" + round);
      }

      // ------------------------------------------- reconnect=0
      {
         Federate c("robust-c-" + run, ";reconnect=0");
         c.rti->joinFederationExecution(L"c", L"t", fed);
         Raw thief("robust-c-" + run);
         thief.connect(true);
         check(pump({&c}, [&] { return c.lost == 1; }, 5.0), "reconnect=0: connectionLost at once");
         thief.disconnect();
      }

      // ------------------------------------------- stale presence removed
      {
         Federate observer("robust-o-" + run, ";staleClientDelay=1");
         std::string const idD = "robust-d-" + run, presenceD = "hla/clients/" + idD;
         watch.client.subscribe(presenceD, 1)->wait_for(std::chrono::seconds(5));
#if defined(_WIN32)
         std::string const cmd = "\"\"" + std::string(argv[0]) + "\" --child " + fedName + " " + idD + "\"";
#else
         std::string const cmd = "\"" + std::string(argv[0]) + "\" --child " + fedName + " " + idD;
#endif
         int const rc = std::system(cmd.c_str());
         check(rc == 0, "child federate joined, then died without resign");
         bool const removed = waitUntil(
            [&] {
               std::vector<std::string> const p = watch.payloads(presenceD);
               return p.size() >= 3 && p.back().empty();
            },
            8.0);
         std::vector<std::string> const p = watch.payloads(presenceD);
         check(removed && isOnline(p[0]) && isOffline(p[p.size() - 2]),
               "presence online, offline (Last Will), then removed after staleClientDelay");
         check(a.lost == 0 && b.lost == 0, "a and b unaffected");
      }

      watch.disconnect();
      b.rti->resignFederationExecution(DELETE_OBJECTS);
      a.rti->resignFederationExecution(DELETE_OBJECTS);
      a.rti->destroyFederationExecution(fed);
   }
   catch (Exception const& e)
   {
      std::wcout << L"exception: " << e.what() << std::endl;
      ++failures;
   }
   catch (std::exception const& e)
   {
      std::cout << "exception: " << e.what() << std::endl;
      ++failures;
   }

   std::cout << (checks - (failures > checks ? checks : failures)) << "/" << checks << " checks passed" << std::endl;
   return failures == 0 ? 0 : 1;
}
