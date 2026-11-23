// hla-objects-test: declaration and object management between several
// ambassadors (several MQTT connections) in one process. Needs a broker.
//
//   hla-objects-test [--broker mqtt://host:1883]
//
// Standard API only. Exit code 0 = all checks passed.

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS // std::getenv
#endif

#include <RTI/NullFederateAmbassador.h>
#include <RTI/RTI1516.h>
#include <RTI/time/HLAfloat64Time.h>
#include <RTI/time/HLAinteger64Time.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <string>
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

VariableLengthData text(std::string const& s)
{
   return VariableLengthData(s.data(), s.size());
}

std::string str(VariableLengthData const& d)
{
   return d.data() ? std::string(static_cast<char const*>(d.data()), d.size()) : std::string();
}

struct Discovered
{
   ObjectClassHandle cls;
   std::wstring name;
};

struct Reflection
{
   ObjectInstanceHandle object;
   AttributeHandleValueMap values;
   std::string tag;
   bool hasTime = false;
   double time = 0;
};

struct Received
{
   InteractionClassHandle cls;
   ParameterHandleValueMap values;
   std::string tag;
   bool hasTime = false;
   double time = 0;
};

struct Federate : NullFederateAmbassador
{
   std::unique_ptr<RTIambassador> rti;
   std::map<ObjectInstanceHandle, Discovered> discovered;
   int discoverCount = 0;
   std::vector<Reflection> reflections;
   std::vector<Received> interactions;
   std::map<ObjectInstanceHandle, std::string> removed; // object -> tag
   std::vector<std::pair<ObjectInstanceHandle, AttributeHandleSet>> provides;
   std::map<std::wstring, bool> reservations;
   std::set<std::wstring> multipleSucceeded, multipleFailed;

   explicit Federate(std::string const& clientId)
   {
      rti = RTIambassadorFactory().createRTIambassador();
      rti->connect(*this, HLA_EVOKED, broker + L";clientId=" + widen(clientId));
   }

   // Delivers callbacks until `done` or the timeout.
   bool pump(std::function<bool()> done, double seconds = 3.0)
   {
      auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
      while (!done())
      {
         if (std::chrono::steady_clock::now() > deadline)
            return false;
         rti->evokeMultipleCallbacks(0.01, 0.1);
      }
      return true;
   }
   void settle(double seconds = 0.3) { rti->evokeMultipleCallbacks(seconds, seconds + 0.2); }

   void discoverObjectInstance(ObjectInstanceHandle o, ObjectClassHandle c, std::wstring const& n)
      RTI_THROW((FederateInternalError)) override
   {
      discovered[o] = Discovered{c, n};
      ++discoverCount;
   }
   void reflectAttributeValues(ObjectInstanceHandle o, AttributeHandleValueMap const& v, VariableLengthData const& tag,
                               OrderType, TransportationType, SupplementalReflectInfo)
      RTI_THROW((FederateInternalError)) override
   {
      reflections.push_back(Reflection{o, v, str(tag)});
   }
   void reflectAttributeValues(ObjectInstanceHandle o, AttributeHandleValueMap const& v, VariableLengthData const& tag,
                               OrderType, TransportationType, LogicalTime const& t, OrderType, SupplementalReflectInfo)
      RTI_THROW((FederateInternalError)) override
   {
      reflections.push_back(Reflection{o, v, str(tag), true, dynamic_cast<HLAfloat64Time const&>(t).getTime()});
   }
   void receiveInteraction(InteractionClassHandle c, ParameterHandleValueMap const& v, VariableLengthData const& tag,
                           OrderType, TransportationType, SupplementalReceiveInfo)
      RTI_THROW((FederateInternalError)) override
   {
      interactions.push_back(Received{c, v, str(tag)});
   }
   void receiveInteraction(InteractionClassHandle c, ParameterHandleValueMap const& v, VariableLengthData const& tag,
                           OrderType, TransportationType, LogicalTime const& t, OrderType, SupplementalReceiveInfo)
      RTI_THROW((FederateInternalError)) override
   {
      interactions.push_back(Received{c, v, str(tag), true, dynamic_cast<HLAfloat64Time const&>(t).getTime()});
   }
   void removeObjectInstance(ObjectInstanceHandle o, VariableLengthData const& tag, OrderType, SupplementalRemoveInfo)
      RTI_THROW((FederateInternalError)) override
   {
      removed[o] = str(tag);
   }
   void provideAttributeValueUpdate(ObjectInstanceHandle o, AttributeHandleSet const& a, VariableLengthData const&)
      RTI_THROW((FederateInternalError)) override
   {
      provides.push_back(std::make_pair(o, a));
   }
   void objectInstanceNameReservationSucceeded(std::wstring const& n) RTI_THROW((FederateInternalError)) override
   {
      reservations[n] = true;
   }
   void objectInstanceNameReservationFailed(std::wstring const& n) RTI_THROW((FederateInternalError)) override
   {
      reservations[n] = false;
   }
   void multipleObjectInstanceNameReservationSucceeded(std::set<std::wstring> const& n)
      RTI_THROW((FederateInternalError)) override
   {
      multipleSucceeded = n;
   }
   void multipleObjectInstanceNameReservationFailed(std::set<std::wstring> const& n)
      RTI_THROW((FederateInternalError)) override
   {
      multipleFailed = n;
   }
};
} // namespace

int main(int argc, char** argv)
{
   for (int i = 1; i < argc; ++i)
      if (std::string(argv[i]) == "--broker" && i + 1 < argc)
         broker = widen(argv[++i]);
   if (char const* env = std::getenv("HLA_MQTT_BROKER"))
      broker = widen(env);

   std::random_device rd;
   std::string const run = std::to_string(rd() % 1000000);
   std::wstring const fed = widen("objtest-" + run);
   std::cout << "federation: " << narrow(fed) << std::endl;

   try
   {
      Federate a("objtest-a-" + run), b("objtest-b-" + run), c("objtest-c-" + run);
      a.rti->createFederationExecution(fed, fom("base.xml"));
      a.rti->joinFederationExecution(L"producer", L"t", fed);
      b.rti->joinFederationExecution(L"consumer", L"t", fed);
      c.rti->joinFederationExecution(L"late", L"t", fed);

      ObjectClassHandle const vehicle = a.rti->getObjectClassHandle(L"Vehicle");
      ObjectClassHandle const car = a.rti->getObjectClassHandle(L"Vehicle.Car");
      AttributeHandle const position = a.rti->getAttributeHandle(car, L"Position");
      AttributeHandle const nameAttr = a.rti->getAttributeHandle(car, L"Name");
      AttributeHandle const speed = a.rti->getAttributeHandle(car, L"Speed");
      InteractionClassHandle const message = a.rti->getInteractionClassHandle(L"Message");
      InteractionClassHandle const urgent = a.rti->getInteractionClassHandle(L"Message.Urgent");
      ParameterHandle const textParam = a.rti->getParameterHandle(urgent, L"Text");
      ParameterHandle const level = a.rti->getParameterHandle(urgent, L"Level");

      // ----------------------------------------------------- declarations
      expect<ObjectClassNotPublished>("register an unpublished class -> ObjectClassNotPublished",
                                      [&] { a.rti->registerObjectInstance(car); });
      expect<AttributeNotDefined>("publish an attribute of a subclass on the superclass -> AttributeNotDefined",
                                  [&] { a.rti->publishObjectClassAttributes(vehicle, AttributeHandleSet{speed}); });
      expect<InteractionClassNotPublished>("send an unpublished interaction -> InteractionClassNotPublished",
                                           [&] { a.rti->sendInteraction(urgent, ParameterHandleValueMap(), text("")); });
      expect<InvalidUpdateRateDesignator>("subscribe with an unknown update rate -> InvalidUpdateRateDesignator", [&] {
         b.rti->subscribeObjectClassAttributes(vehicle, AttributeHandleSet{position}, true, L"Turbo");
      });
      expect<ObjectClassRelevanceAdvisorySwitchIsOn>("advisory switch enabled twice -> ...IsOn", [&] {
         a.rti->enableObjectClassRelevanceAdvisorySwitch();
         a.rti->enableObjectClassRelevanceAdvisorySwitch();
      });

      a.rti->publishObjectClassAttributes(car, AttributeHandleSet{position, nameAttr, speed});
      a.rti->publishInteractionClass(urgent);
      b.rti->subscribeObjectClassAttributes(vehicle, AttributeHandleSet{position, nameAttr}, true, L"Fast");
      b.rti->subscribeInteractionClass(message);

      // ------------------------------------------------ register/discover
      ObjectInstanceHandle const car1 = a.rti->registerObjectInstance(car);
      check(b.pump([&] { return b.discovered.count(car1) != 0; }) && b.discovered[car1].cls == vehicle,
            "B discovers the Car as Vehicle (its subscribed superclass)");
      check(b.discovered[car1].name == a.rti->getObjectInstanceName(car1) &&
               b.rti->getObjectInstanceHandle(b.discovered[car1].name) == car1,
            "instance name and handle agree between federates");
      check(b.rti->getKnownObjectClassHandle(car1) == vehicle && a.rti->getKnownObjectClassHandle(car1) == car,
            "getKnownObjectClassHandle: Vehicle for B, Car for the owner");

      // ------------------------------------------------- update/reflect
      AttributeHandleValueMap values;
      values[position] = text("pos-1");
      values[speed] = text("speed-1");
      a.rti->updateAttributeValues(car1, values, text("tag-1"));
      check(b.pump([&] { return !b.reflections.empty(); }) && b.reflections[0].object == car1 &&
               b.reflections[0].values.size() == 1 && str(b.reflections[0].values[position]) == "pos-1" &&
               b.reflections[0].tag == "tag-1",
            "B reflects only the subscribed attribute (Position), with the tag");

      // Late subscriber: discovers the existing object as Car.
      c.rti->subscribeObjectClassAttributes(car, AttributeHandleSet{speed, position}, true, L"");
      check(c.pump([&] { return c.discovered.count(car1) != 0; }) && c.discovered[car1].cls == car,
            "late subscriber C discovers the existing object as Car (retained registration)");
      AttributeHandleValueMap speedOnly;
      speedOnly[speed] = text("speed-2");
      a.rti->updateAttributeValues(car1, speedOnly, text(""));
      check(c.pump([&] { return !c.reflections.empty(); }) && str(c.reflections[0].values[speed]) == "speed-2",
            "C reflects Speed");
      b.settle();
      check(b.reflections.size() == 1, "B gets nothing for an update of unsubscribed attributes only");

      expect<AttributeNotOwned>("B updates A's object -> AttributeNotOwned",
                                [&] { b.rti->updateAttributeValues(car1, values, text("")); });
      expect<DeletePrivilegeNotHeld>("B deletes A's object -> DeletePrivilegeNotHeld",
                                     [&] { b.rti->deleteObjectInstance(car1, text("")); });
      expect<InvalidLogicalTime>("update with a time of another type -> InvalidLogicalTime",
                                 [&] { a.rti->updateAttributeValues(car1, values, text(""), HLAinteger64Time(5)); });

      // Time-stamped update: delivered (receive order) with its time.
      AttributeHandleValueMap posOnly;
      posOnly[position] = text("pos-t");
      a.rti->updateAttributeValues(car1, posOnly, text("timed"), HLAfloat64Time(2.5));
      check(b.pump([&] { return b.reflections.size() == 2; }) && b.reflections[1].hasTime &&
               b.reflections[1].time == 2.5 && b.reflections[1].tag == "timed",
            "time-stamped update reflected with its time");

      // ----------------------------------------------------- interactions
      c.rti->subscribeInteractionClass(urgent);
      ParameterHandleValueMap params;
      params[textParam] = text("hello");
      params[level] = text("\x00\x00\x00\x03");
      a.rti->sendInteraction(urgent, params, text("itag"));
      check(b.pump([&] { return !b.interactions.empty(); }) && b.interactions[0].cls == message &&
               b.interactions[0].values.size() == 1 && str(b.interactions[0].values[textParam]) == "hello" &&
               b.interactions[0].tag == "itag",
            "B receives Urgent as Message (subscribed superclass), Message parameters only");
      check(c.pump([&] { return !c.interactions.empty(); }) && c.interactions[0].cls == urgent &&
               c.interactions[0].values.size() == 2,
            "C receives Urgent with all its parameters");
      a.rti->sendInteraction(urgent, params, text(""), HLAfloat64Time(7.0));
      check(b.pump([&] { return b.interactions.size() == 2; }) && b.interactions[1].hasTime &&
               b.interactions[1].time == 7.0,
            "time-stamped interaction received with its time");
      a.settle(0.2);
      check(a.interactions.empty(), "a federate does not receive its own interactions");

      // ----------------------------------------------------------- names
      a.rti->reserveObjectInstanceName(L"Ford");
      check(a.pump([&] { return a.reservations.count(L"Ford") != 0; }) && a.reservations[L"Ford"],
            "A reserves the name Ford");
      b.rti->reserveObjectInstanceName(L"Ford");
      check(b.pump([&] { return b.reservations.count(L"Ford") != 0; }) && !b.reservations[L"Ford"],
            "B cannot reserve Ford (reservation failed callback)");
      expect<IllegalName>("reserve a name starting with HLA -> IllegalName",
                          [&] { a.rti->reserveObjectInstanceName(L"HLAcar"); });
      expect<ObjectInstanceNameNotReserved>("register with an unreserved name -> ObjectInstanceNameNotReserved",
                                            [&] { a.rti->registerObjectInstance(car, L"Mustang"); });
      ObjectInstanceHandle const ford = a.rti->registerObjectInstance(car, L"Ford");
      expect<ObjectInstanceNameInUse>("register Ford twice -> ObjectInstanceNameInUse",
                                      [&] { a.rti->registerObjectInstance(car, L"Ford"); });
      check(b.pump([&] { return b.discovered.count(ford) != 0; }) && b.discovered[ford].name == L"Ford",
            "B discovers Ford by its name");
      b.rti->reserveMultipleObjectInstanceName(std::set<std::wstring>{L"Truck1", L"Truck2"});
      check(b.pump([&] { return !b.multipleSucceeded.empty(); }) && b.multipleSucceeded.size() == 2,
            "multiple name reservation succeeds");
      a.rti->reserveMultipleObjectInstanceName(std::set<std::wstring>{L"Truck2", L"Truck3"});
      check(a.pump([&] { return !a.multipleFailed.empty(); }), "multiple reservation with one taken name fails");
      b.rti->releaseMultipleObjectInstanceName(std::set<std::wstring>{L"Truck1", L"Truck2"});
      expect<ObjectInstanceNameNotReserved>("release a name not reserved -> ObjectInstanceNameNotReserved",
                                            [&] { b.rti->releaseObjectInstanceName(L"Truck1"); });
      a.multipleFailed.clear();
      a.rti->reserveMultipleObjectInstanceName(std::set<std::wstring>{L"Truck2", L"Truck3"});
      check(a.pump([&] { return !a.multipleSucceeded.empty(); }), "released names can be reserved again");

      // ------------------------------------------------ update requests
      b.rti->requestAttributeValueUpdate(car1, AttributeHandleSet{position}, text(""));
      check(a.pump([&] { return !a.provides.empty(); }) && a.provides[0].first == car1 &&
               a.provides[0].second == AttributeHandleSet{position},
            "instance request: the owner gets provideAttributeValueUpdate");
      a.provides.clear();
      c.rti->requestAttributeValueUpdate(car, AttributeHandleSet{speed}, text(""));
      check(a.pump([&] { return a.provides.size() == 2; }), "class request: provide for each Car of the owner");

      // ------------------------------------------- local delete/rediscover
      int const before = b.discoverCount;
      b.rti->localDeleteObjectInstance(car1);
      expect<ObjectInstanceNotKnown>("locally deleted object is unknown",
                                     [&] { b.rti->getKnownObjectClassHandle(car1); });
      a.rti->updateAttributeValues(car1, posOnly, text(""));
      check(b.pump([&] { return b.discoverCount == before + 1; }) && b.discovered[car1].cls == vehicle,
            "object rediscovered by its next update after localDeleteObjectInstance");

      // --------------------------------------------------------- delete
      a.rti->deleteObjectInstance(car1, text("bye"));
      check(b.pump([&] { return b.removed.count(car1) != 0; }) && b.removed[car1] == "bye",
            "B gets removeObjectInstance with the tag");
      check(c.pump([&] { return c.removed.count(car1) != 0; }), "C gets removeObjectInstance");
      expect<ObjectInstanceNotKnown>("update a deleted object -> ObjectInstanceNotKnown",
                                     [&] { a.rti->updateAttributeValues(car1, values, text("")); });

      // A federate vanishing without resign: its objects are removed.
      {
         Federate d("objtest-d-" + run);
         d.rti->joinFederationExecution(L"doomed", L"t", fed);
         d.rti->publishObjectClassAttributes(car, AttributeHandleSet{position});
         ObjectInstanceHandle const ghost = d.rti->registerObjectInstance(car);
         check(b.pump([&] { return b.discovered.count(ghost) != 0; }), "B discovers D's object");
         d.rti.reset(); // no resign
         check(b.pump([&] { return b.removed.count(ghost) != 0; }), "D vanishes: B gets removeObjectInstance");
      }

      // Resign with UNCONDITIONALLY_DIVEST_ATTRIBUTES: the object stays.
      ObjectInstanceHandle orphan;
      {
         Federate e("objtest-e-" + run);
         e.rti->joinFederationExecution(L"divester", L"t", fed);
         e.rti->publishObjectClassAttributes(car, AttributeHandleSet{position});
         orphan = e.rti->registerObjectInstance(car);
         check(b.pump([&] { return b.discovered.count(orphan) != 0; }), "B discovers E's object");
         e.rti->resignFederationExecution(UNCONDITIONALLY_DIVEST_ATTRIBUTES);
      }
      b.settle(0.5);
      check(b.removed.count(orphan) == 0, "divested object stays in the federation after E leaves");

      // ------------------------------------------------------ throughput
      int const burst = 1000;
      b.reflections.clear();
      auto t0 = std::chrono::steady_clock::now();
      for (int k = 0; k < burst; ++k)
      {
         AttributeHandleValueMap v;
         v[position] = text(std::to_string(k));
         a.rti->updateAttributeValues(ford, v, text(""));
      }
      bool all = b.pump([&] { return int(b.reflections.size()) >= burst; }, 20.0);
      double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      bool ordered = all;
      for (int k = 0; all && k < burst; ++k)
         ordered = ordered && str(b.reflections[k].values[position]) == std::to_string(k);
      check(all && ordered, std::to_string(burst) + " updates received in order (" + std::to_string(int(burst / secs)) +
                               " updates/s)");

      // ---------------------------------------------------------- resign
      expect<FederateOwnsAttributes>("resign NO_ACTION while owning an object -> FederateOwnsAttributes",
                                     [&] { a.rti->resignFederationExecution(NO_ACTION); });
      a.rti->resignFederationExecution(DELETE_OBJECTS);
      check(b.pump([&] { return b.removed.count(ford) != 0; }), "resign DELETE_OBJECTS: B gets removeObjectInstance");
      b.rti->resignFederationExecution(NO_ACTION);
      c.rti->resignFederationExecution(NO_ACTION);
      a.rti->destroyFederationExecution(fed);
   }
   catch (Exception const& e)
   {
      check(false, "unexpected exception: " + narrow(e.what()));
   }

   std::cout << (failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED") << " (" << checks << " checks, "
             << failures << " failure(s))" << std::endl;
   return failures == 0 ? 0 : 1;
}
