// hla-federation-test: federation management and support services with
// several ambassadors (several MQTT connections) in one process.
// Needs a reachable broker.
//
//   hla-federation-test [--broker mqtt://host:1883]
//
// Standard API only. Exit code 0 = all checks passed.

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS // std::getenv
#endif

#include <RTI/NullFederateAmbassador.h>
#include <RTI/RTI1516.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
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
std::wstring broker = L"mqtt://localhost:1883";

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

std::string narrow(std::wstring const& s)
{
   std::string out;
   for (wchar_t c : s)
      out += (c < 128) ? char(c) : '?';
   return out;
}

std::wstring fom(char const* file)
{
   return widen(std::string(HLA_TEST_FOM_DIR) + "/" + file);
}

// Runs f, expects exception E. Reports the message of any other exception.
template <class E>
void expect(std::string const& what, std::function<void()> f)
{
   std::string detail;
   bool ok = false;
   try
   {
      f();
      detail = " -- no exception";
   }
   catch (E const&)
   {
      ok = true;
   }
   catch (Exception const& e)
   {
      detail = " -- other exception: " + narrow(e.what());
   }
   check(ok, what + detail);
}

bool noThrow(std::string const& what, std::function<void()> f)
{
   try
   {
      f();
      check(true, what);
      return true;
   }
   catch (Exception const& e)
   {
      check(false, what + " -- " + narrow(e.what()));
      return false;
   }
}

struct Federate : NullFederateAmbassador
{
   std::unique_ptr<RTIambassador> rti;
   FederationExecutionInformationVector reported;
   std::atomic<bool> gotReport{false};

   explicit Federate(std::string const& clientId)
   {
      rti = RTIambassadorFactory().createRTIambassador();
      rti->connect(*this, HLA_EVOKED, broker + L";clientId=" + widen(clientId));
   }

   void reportFederationExecutions(FederationExecutionInformationVector const& list) RTI_THROW((FederateInternalError)) override
   {
      reported = list;
      gotReport = true;
   }
};

std::string uniqueSuffix()
{
   std::random_device rd;
   return std::to_string(rd() % 1000000);
}

// Waits until `f` stops throwing (state propagated through the broker).
template <class T>
bool eventually(std::function<T()> f, T& out, int ms = 3000)
{
   auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
   for (;;)
   {
      try
      {
         out = f();
         return true;
      }
      catch (Exception const&)
      {
         if (std::chrono::steady_clock::now() > deadline)
            return false;
         std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
   }
}
} // namespace

int main(int argc, char** argv)
{
   for (int i = 1; i < argc; ++i)
      if (std::string(argv[i]) == "--broker" && i + 1 < argc)
         broker = widen(argv[++i]);
   if (char const* env = std::getenv("HLA_MQTT_BROKER"))
      broker = widen(env);

   std::string const run = uniqueSuffix();
   std::wstring const fed = widen("fedtest-" + run);
   std::cout << "federation: " << narrow(fed) << "  FOM dir: " << HLA_TEST_FOM_DIR << std::endl;

   try
   {
      {
         Federate notConnected("unused-" + run);
         notConnected.rti->disconnect();
         expect<NotConnected>("create while not connected -> NotConnected",
                              [&] { notConnected.rti->createFederationExecution(fed, fom("base.xml")); });
      }

      Federate a("fedtest-a-" + run), b("fedtest-b-" + run), c("fedtest-c-" + run);

      // ---------------------------------------------------------- create
      expect<CouldNotOpenFDD>("create with a missing file -> CouldNotOpenFDD",
                              [&] { a.rti->createFederationExecution(fed, fom("nothere.xml")); });
      expect<ErrorReadingFDD>("create with malformed XML -> ErrorReadingFDD",
                              [&] { a.rti->createFederationExecution(fed, fom("malformed.xml")); });
      expect<InconsistentFDD>("create with inconsistent modules -> InconsistentFDD", [&] {
         a.rti->createFederationExecution(fed, std::vector<std::wstring>{fom("base.xml"), fom("inconsistent.xml")});
      });
      expect<CouldNotCreateLogicalTimeFactory>("create with an unknown time -> CouldNotCreateLogicalTimeFactory",
                                               [&] { a.rti->createFederationExecution(fed, fom("base.xml"), L"Bogus"); });
      noThrow("create (HLAinteger64Time)",
              [&] { a.rti->createFederationExecution(fed, fom("base.xml"), L"HLAinteger64Time"); });
      expect<FederationExecutionAlreadyExists>("second create -> FederationExecutionAlreadyExists",
                                               [&] { b.rti->createFederationExecution(fed, fom("base.xml")); });

      // ----------------------------------------------------------- list
      noThrow("listFederationExecutions", [&] { a.rti->listFederationExecutions(); });
      a.rti->evokeMultipleCallbacks(0.1, 1.0);
      bool listed = false;
      for (auto const& info : a.reported)
         if (info.federationExecutionName == fed && info.logicalTimeImplementationName == L"HLAinteger64Time")
            listed = true;
      check(a.gotReport && listed, "reportFederationExecutions lists the federation and its time");

      // ------------------------------------------------------------- join
      expect<FederationExecutionDoesNotExist>("join an unknown federation -> FederationExecutionDoesNotExist",
                                              [&] { b.rti->joinFederationExecution(L"x", L"t", fed + L"-nope"); });
      FederateHandle ha, hb;
      noThrow("A joins as alpha", [&] { ha = a.rti->joinFederationExecution(L"alpha", L"typeA", fed); });
      expect<FederateAlreadyExecutionMember>("A joins twice -> FederateAlreadyExecutionMember",
                                             [&] { a.rti->joinFederationExecution(L"alpha2", L"typeA", fed); });
      expect<FederateNameAlreadyInUse>("B joins as alpha -> FederateNameAlreadyInUse",
                                       [&] { b.rti->joinFederationExecution(L"alpha", L"typeB", fed); });
      expect<InconsistentFDD>("C joins with an inconsistent module -> InconsistentFDD", [&] {
         c.rti->joinFederationExecution(L"gamma", L"typeC", fed, std::vector<std::wstring>{fom("inconsistent.xml")});
      });
      noThrow("B joins as beta with an additional module", [&] {
         hb = b.rti->joinFederationExecution(L"beta", L"typeB", fed, std::vector<std::wstring>{fom("extra.xml")});
      });
      check(ha.isValid() && hb.isValid() && ha != hb, "federate handles valid and distinct");

      // ------------------------------------------------- support services
      ObjectClassHandle carA = a.rti->getObjectClassHandle(L"HLAobjectRoot.Vehicle.Car");
      ObjectClassHandle carB = b.rti->getObjectClassHandle(L"Vehicle.Car");
      ObjectClassHandle vehicle = a.rti->getObjectClassHandle(L"Vehicle");
      check(carA == carB, "object class handles identical in both federates (deterministic)");
      check(a.rti->getObjectClassName(carA) == L"HLAobjectRoot.Vehicle.Car", "getObjectClassName = qualified name");
      AttributeHandle posCar = a.rti->getAttributeHandle(carA, L"Position");
      check(posCar == a.rti->getAttributeHandle(vehicle, L"Position") && posCar == b.rti->getAttributeHandle(carB, L"Position"),
            "inherited attribute: same handle from the subclass and in both federates");
      check(a.rti->getAttributeName(carA, posCar) == L"Position", "getAttributeName");
      noThrow("HLAprivilegeToDeleteObject inherited by every class",
              [&] { a.rti->getAttributeHandle(carA, L"HLAprivilegeToDeleteObject"); });
      AttributeHandle speed = a.rti->getAttributeHandle(carA, L"Speed");
      expect<AttributeNotDefined>("getAttributeName of a subclass attribute on the superclass -> AttributeNotDefined",
                                  [&] { a.rti->getAttributeName(vehicle, speed); });
      expect<NameNotFound>("getObjectClassHandle(unknown) -> NameNotFound",
                           [&] { a.rti->getObjectClassHandle(L"Vehicle.Boat"); });
      expect<InvalidObjectClassHandle>("getObjectClassName(invalid) -> InvalidObjectClassHandle",
                                       [&] { a.rti->getObjectClassName(ObjectClassHandle()); });

      ObjectClassHandle aircraft;
      check(eventually<ObjectClassHandle>([&] { return a.rti->getObjectClassHandle(L"Vehicle.Aircraft"); }, aircraft) &&
               aircraft == b.rti->getObjectClassHandle(L"HLAobjectRoot.Vehicle.Aircraft"),
            "class of B's additional module visible to A");

      InteractionClassHandle urgent = a.rti->getInteractionClassHandle(L"HLAinteractionRoot.Message.Urgent");
      check(urgent == b.rti->getInteractionClassHandle(L"Message.Urgent"), "interaction class handles identical");
      ParameterHandle text = a.rti->getParameterHandle(urgent, L"Text");
      check(a.rti->getParameterName(urgent, text) == L"Text" &&
               text == a.rti->getParameterHandle(a.rti->getInteractionClassHandle(L"Message"), L"Text"),
            "inherited parameter");
      expect<NameNotFound>("getParameterHandle(unknown) -> NameNotFound",
                           [&] { a.rti->getParameterHandle(urgent, L"Nope"); });

      DimensionHandle zone = a.rti->getDimensionHandle(L"Zone");
      check(a.rti->getDimensionName(zone) == L"Zone" && a.rti->getDimensionUpperBound(zone) == 100,
            "dimension name and upper bound");
      DimensionHandleSet dn = a.rti->getAvailableDimensionsForClassAttribute(vehicle, a.rti->getAttributeHandle(vehicle, L"Name"));
      DimensionHandleSet dm = a.rti->getAvailableDimensionsForInteractionClass(a.rti->getInteractionClassHandle(L"Message"));
      check(dn.size() == 1 && dn.count(zone) && dm.size() == 1 && dm.count(zone), "available dimensions");
      check(a.rti->getUpdateRateValue(L"Fast") == 10.0, "getUpdateRateValue");
      check(a.rti->getOrderType(L"TimeStamp") == TIMESTAMP && a.rti->getOrderName(RECEIVE) == L"Receive" &&
               a.rti->getTransportationType(L"HLAbestEffort") == BEST_EFFORT &&
               a.rti->getTransportationName(RELIABLE) == L"HLAreliable",
            "order and transportation names");

      FederateHandle hbSeen;
      check(eventually<FederateHandle>([&] { return a.rti->getFederateHandle(L"beta"); }, hbSeen) && hbSeen == hb &&
               a.rti->getFederateName(hb) == L"beta" && b.rti->getFederateName(ha) == L"alpha",
            "getFederateHandle / getFederateName across federates");
      check(a.rti->getTimeFactory()->getName() == L"HLAinteger64Time", "getTimeFactory follows the federation");
      check(a.rti->decodeObjectClassHandle(carA.encode()) == carA && a.rti->decodeFederateHandle(hb.encode()) == hb,
            "decode handles");
      check(a.rti->getAutomaticResignDirective() == CANCEL_THEN_DELETE_THEN_DIVEST, "default automatic resign directive");

      // --------------------------------------------------------- destroy
      expect<FederatesCurrentlyJoined>("destroy with joined federates -> FederatesCurrentlyJoined",
                                       [&] { c.rti->destroyFederationExecution(fed); });
      expect<FederateIsExecutionMember>("disconnect while joined -> FederateIsExecutionMember",
                                        [&] { a.rti->disconnect(); });
      noThrow("A resigns", [&] { a.rti->resignFederationExecution(NO_ACTION); });
      expect<FederateNotExecutionMember>("resign twice -> FederateNotExecutionMember",
                                         [&] { a.rti->resignFederationExecution(NO_ACTION); });
      noThrow("A joins again with the same name", [&] { a.rti->joinFederationExecution(L"alpha", L"typeA", fed); });
      noThrow("A resigns", [&] { a.rti->resignFederationExecution(DELETE_OBJECTS); });

      // A federate that vanishes without resigning (here: ambassador deleted)
      // leaves a stale record which must not block destroy.
      {
         Federate d("fedtest-d-" + run);
         noThrow("D joins as delta", [&] { d.rti->joinFederationExecution(L"delta", L"typeD", fed); });
         d.rti.reset();
      }
      noThrow("B resigns", [&] { b.rti->resignFederationExecution(CANCEL_THEN_DELETE_THEN_DIVEST); });
      noThrow("destroy (stale record of D ignored)", [&] { c.rti->destroyFederationExecution(fed); });
      expect<FederationExecutionDoesNotExist>("destroy twice -> FederationExecutionDoesNotExist",
                                              [&] { c.rti->destroyFederationExecution(fed); });
      expect<FederationExecutionDoesNotExist>("join a destroyed federation -> FederationExecutionDoesNotExist",
                                              [&] { b.rti->joinFederationExecution(L"beta", L"typeB", fed); });

      // ------------------------------------------- 1516-2000 format module
      std::wstring const legacy = fed + L"-legacy";
      noThrow("create with a 1516-2000 format module", [&] { a.rti->createFederationExecution(legacy, fom("legacy2000.xml")); });
      noThrow("join it", [&] { a.rti->joinFederationExecution(L"old", L"t", legacy); });
      check(a.rti->getTimeFactory()->getName() == L"HLAfloat64Time", "default time representation is HLAfloat64Time");
      noThrow("1516-2000 classes resolved", [&] {
         ObjectClassHandle ship = a.rti->getObjectClassHandle(L"Ship");
         a.rti->getAttributeHandle(ship, L"Heading");
         a.rti->getParameterHandle(a.rti->getInteractionClassHandle(L"Ping"), L"Seq");
      });
      a.rti->resignFederationExecution(NO_ACTION);
      a.rti->destroyFederationExecution(legacy);

      // ------------------------------------------------------------ races
      // Several ambassadors create the same federation at the same time,
      // then join with the same federate name: exactly one wins each time.
      for (int round = 0; round < 3; ++round)
      {
         std::wstring const raceFed = fed + L"-race" + std::to_wstring(round);
         std::vector<std::unique_ptr<Federate>> racers;
         for (int k = 0; k < 4; ++k)
            racers.emplace_back(new Federate("fedtest-r" + std::to_string(k) + "-" + run));
         std::atomic<int> created{0}, exists{0}, joined{0}, inUse{0}, other{0};
         std::vector<std::thread> threads;
         for (auto& r : racers)
            threads.emplace_back([&, rp = r.get()] {
               try
               {
                  rp->rti->createFederationExecution(raceFed, fom("base.xml"));
                  ++created;
               }
               catch (FederationExecutionAlreadyExists const&)
               {
                  ++exists;
               }
               catch (Exception const& e)
               {
                  std::cout << "  create: " << narrow(e.what()) << std::endl;
                  ++other;
               }
            });
         for (auto& t : threads)
            t.join();
         threads.clear();
         check(created == 1 && exists == 3 && other == 0,
               "race " + std::to_string(round) + ": 4 concurrent creates, exactly one succeeds (" +
                  std::to_string(created) + " created, " + std::to_string(exists) + " already exists)");
         for (auto& r : racers)
            threads.emplace_back([&, rp = r.get()] {
               try
               {
                  rp->rti->joinFederationExecution(L"same", L"t", raceFed);
                  ++joined;
               }
               catch (FederateNameAlreadyInUse const&)
               {
                  ++inUse;
               }
               catch (Exception const& e)
               {
                  std::cout << "  join: " << narrow(e.what()) << std::endl;
                  ++other;
               }
            });
         for (auto& t : threads)
            t.join();
         check(joined == 1 && inUse == 3 && other == 0,
               "race " + std::to_string(round) + ": 4 concurrent joins with one name, exactly one succeeds (" +
                  std::to_string(joined) + " joined, " + std::to_string(inUse) + " name in use)");
         for (auto& r : racers)
         {
            try
            {
               r->rti->resignFederationExecution(NO_ACTION);
            }
            catch (FederateNotExecutionMember const&)
            {
            }
         }
         racers[0]->rti->destroyFederationExecution(raceFed);
      }
   }
   catch (Exception const& e)
   {
      check(false, "unexpected exception: " + narrow(e.what()));
   }

   std::cout << (failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED") << " (" << checks << " checks, "
             << failures << " failure(s))" << std::endl;
   return failures == 0 ? 0 : 1;
}
