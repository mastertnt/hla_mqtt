// hla-ddm-test: data distribution management services (IEEE 1516.1-2010,
// chapter 9, and 10.28 to 10.30) between two ambassadors in one process:
// regions, update region associations, subscriptions with regions.
// Needs a broker.
//
//   hla-ddm-test [--broker mqtt://host:1883]
//
// Part 1: services and their exceptions. Part 2 (filterTests): filtering by
// region overlap, scope and relevance advisories, sends and requests with
// regions.
//
// Standard API only. Exit code 0 = all checks passed.

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS // std::getenv
#endif

#include <RTI/NullFederateAmbassador.h>
#include <RTI/RTI1516.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
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

void noThrow(std::string const& what, std::function<void()> f)
{
   try
   {
      f();
      check(true, what);
   }
   catch (Exception const& e)
   {
      check(false, what + " -- " + narrow(e.what()));
   }
}

VariableLengthData text(std::string const& s)
{
   return VariableLengthData(s.data(), s.size());
}

struct Federate : NullFederateAmbassador
{
   std::unique_ptr<RTIambassador> rti;
   std::map<ObjectInstanceHandle, ObjectClassHandle> discovered;
   int reflections = 0;
   int interactions = 0;
   std::vector<std::string> tags; // tags of reflections and interactions
   int provides = 0;
   std::set<AttributeHandle> inScope, updatesOn;
   int outOfScope = 0;
   bool got(std::string const& tag) const
   {
      for (std::string const& t : tags)
         if (t == tag)
            return true;
      return false;
   }

   explicit Federate(std::string const& clientId)
   {
      rti = RTIambassadorFactory().createRTIambassador();
      rti->connect(*this, HLA_EVOKED, broker + L";clientId=" + widen(clientId));
   }

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
   void settle(double seconds = 0.4) { rti->evokeMultipleCallbacks(seconds, seconds + 0.2); }

   void discoverObjectInstance(ObjectInstanceHandle o, ObjectClassHandle c, std::wstring const&)
      RTI_THROW((FederateInternalError)) override
   {
      discovered[o] = c;
   }
   static std::string str(VariableLengthData const& d)
   {
      return d.data() ? std::string(static_cast<char const*>(d.data()), d.size()) : std::string();
   }
   void reflectAttributeValues(ObjectInstanceHandle, AttributeHandleValueMap const&, VariableLengthData const& tag,
                               OrderType, TransportationType, SupplementalReflectInfo)
      RTI_THROW((FederateInternalError)) override
   {
      ++reflections;
      tags.push_back(str(tag));
   }
   void receiveInteraction(InteractionClassHandle, ParameterHandleValueMap const&, VariableLengthData const& tag,
                           OrderType, TransportationType, SupplementalReceiveInfo)
      RTI_THROW((FederateInternalError)) override
   {
      ++interactions;
      tags.push_back(str(tag));
   }
   void provideAttributeValueUpdate(ObjectInstanceHandle, AttributeHandleSet const&, VariableLengthData const&)
      RTI_THROW((FederateInternalError)) override
   {
      ++provides;
   }
   void attributesInScope(ObjectInstanceHandle, AttributeHandleSet const& a) RTI_THROW((FederateInternalError)) override
   {
      inScope.insert(a.begin(), a.end());
   }
   void attributesOutOfScope(ObjectInstanceHandle, AttributeHandleSet const& a) RTI_THROW((FederateInternalError)) override
   {
      ++outOfScope;
      for (AttributeHandle const& h : a)
         inScope.erase(h);
   }
   void turnUpdatesOnForObjectInstance(ObjectInstanceHandle, AttributeHandleSet const& a)
      RTI_THROW((FederateInternalError)) override
   {
      updatesOn.insert(a.begin(), a.end());
   }
   void turnUpdatesOffForObjectInstance(ObjectInstanceHandle, AttributeHandleSet const& a)
      RTI_THROW((FederateInternalError)) override
   {
      for (AttributeHandle const& h : a)
         updatesOn.erase(h);
   }
};

AttributeHandleSetRegionHandleSetPairVector pairs(AttributeHandle a, RegionHandle r)
{
   return AttributeHandleSetRegionHandleSetPairVector{
      std::make_pair(AttributeHandleSet{a}, RegionHandleSet{r})};
}

// One-dimension region [lower, upper) on Zone, committed.
RegionHandle zoneRegion(Federate& f, DimensionHandle zone, unsigned long lower, unsigned long upper)
{
   RegionHandle const r = f.rti->createRegion(DimensionHandleSet{zone});
   f.rti->setRangeBounds(r, zone, RangeBounds(lower, upper));
   f.rti->commitRegionModifications(RegionHandleSet{r});
   return r;
}

void move(Federate& f, RegionHandle r, DimensionHandle zone, unsigned long lower, unsigned long upper)
{
   f.rti->setRangeBounds(r, zone, RangeBounds(lower, upper));
   f.rti->commitRegionModifications(RegionHandleSet{r});
}

// Filtering by overlap, advisories, sends and requests with regions.
void filterTests(std::string const& run)
{
   std::wstring const fed = widen("ddmfilter-" + run);
   Federate a("ddmfilter-a-" + run), b("ddmfilter-b-" + run);
   a.rti->createFederationExecution(fed, widen(std::string(HLA_TEST_FOM_DIR) + "/base.xml"));
   a.rti->joinFederationExecution(L"a", L"t", fed);
   b.rti->joinFederationExecution(L"b", L"t", fed);
   ObjectClassHandle const vehicle = a.rti->getObjectClassHandle(L"Vehicle");
   AttributeHandle const position = a.rti->getAttributeHandle(vehicle, L"Position");
   AttributeHandle const name = a.rti->getAttributeHandle(vehicle, L"Name");
   InteractionClassHandle const message = a.rti->getInteractionClassHandle(L"Message");
   InteractionClassHandle const urgent = a.rti->getInteractionClassHandle(L"Message.Urgent");
   DimensionHandle const zone = a.rti->getDimensionHandle(L"Zone");
   ParameterHandleValueMap const none;

   a.rti->enableAttributeRelevanceAdvisorySwitch();
   b.rti->enableAttributeScopeAdvisorySwitch();
   a.rti->publishObjectClassAttributes(vehicle, AttributeHandleSet{position, name});
   a.rti->publishInteractionClass(message);
   a.rti->publishInteractionClass(urgent);

   RegionHandle const ua = zoneRegion(a, zone, 10, 20);
   ObjectInstanceHandle const object = a.rti->registerObjectInstanceWithRegions(vehicle, pairs(name, ua));
   RegionHandle const sb = zoneRegion(b, zone, 30, 40);
   b.rti->subscribeObjectClassAttributesWithRegions(vehicle, pairs(name, sb));
   check(b.pump([&] { return b.discovered.count(object) != 0; }), "filter: discovery is not filtered by regions");
   a.settle(0.3);
   check(!a.updatesOn.count(name), "no turnUpdatesOn while the regions do not overlap ([10,20) / [30,40))");
   check(b.inScope.empty(), "no attributesInScope while the regions do not overlap");
   auto send = [&](std::string const& tag) {
      AttributeHandleValueMap values;
      values[name] = text(tag);
      a.rti->updateAttributeValues(object, values, text(tag));
   };
   send("n1");
   b.settle();
   check(!b.got("n1"), "update outside the subscription region is not reflected");

   move(a, ua, zone, 35, 36);
   check(b.pump([&] { return b.inScope.count(name) != 0; }),
         "attributesInScope when the update region moves into the subscription region");
   check(a.pump([&] { return a.updatesOn.count(name) != 0; }), "turnUpdatesOn once the regions overlap");
   send("n2");
   check(b.pump([&] { return b.got("n2"); }), "update inside the subscription region is reflected");

   move(b, sb, zone, 0, 5);
   check(b.pump([&] { return b.outOfScope > 0 && !b.inScope.count(name); }),
         "attributesOutOfScope when the subscription region moves away");
   check(a.pump([&] { return !a.updatesOn.count(name); }), "turnUpdatesOff when the subscriber's region moves away");
   send("n3");
   b.settle();
   check(!b.got("n3"), "no reflection after the subscription region moved away");

   b.rti->subscribeObjectClassAttributes(vehicle, AttributeHandleSet{name});
   send("n4");
   check(b.pump([&] { return b.got("n4"); }), "a default-region subscription receives whatever the regions");
   b.rti->unsubscribeObjectClass(vehicle);

   // ---------------------------------- 9.13 requestAttributeValueUpdateWithRegions
   RegionHandle const rq = zoneRegion(b, zone, 30, 40);
   b.rti->requestAttributeValueUpdateWithRegions(vehicle, pairs(name, rq), text("rq1"));
   check(a.pump([&] { return a.provides == 1; }), "request with an overlapping region -> provideAttributeValueUpdate");
   move(b, rq, zone, 0, 5);
   b.rti->requestAttributeValueUpdateWithRegions(vehicle, pairs(name, rq), text("rq2"));
   a.settle();
   check(a.provides == 1, "request with a region that misses the update region -> nothing to provide");
   expect<InvalidRegionContext>("requestAttributeValueUpdateWithRegions on Position -> InvalidRegionContext",
                                [&] { b.rti->requestAttributeValueUpdateWithRegions(vehicle, pairs(position, rq), text("")); });

   // ------------------------------------------- 9.12 sendInteractionWithRegions
   b.rti->subscribeInteractionClassWithRegions(message, RegionHandleSet{sb}); // [0, 5)
   RegionHandle const si1 = zoneRegion(a, zone, 2, 3);
   RegionHandle const si2 = zoneRegion(a, zone, 50, 60);
   a.rti->sendInteractionWithRegions(message, none, RegionHandleSet{si1}, text("i1"));
   check(b.pump([&] { return b.got("i1"); }), "interaction sent with an overlapping region is received");
   a.rti->sendInteractionWithRegions(message, none, RegionHandleSet{si2}, text("i2"));
   a.rti->sendInteraction(message, none, text("i3"));
   check(b.pump([&] { return b.got("i3"); }) && !b.got("i2"),
         "interaction sent with a distant region is filtered, one sent without region is received");
   expect<InvalidRegionContext>("sendInteractionWithRegions on a class without dimension -> InvalidRegionContext",
                                [&] { a.rti->sendInteractionWithRegions(urgent, none, RegionHandleSet{si1}, text("")); });
   expect<RegionNotCreatedByThisFederate>("sendInteractionWithRegions with B's region -> RegionNotCreatedByThisFederate",
                                          [&] { a.rti->sendInteractionWithRegions(message, none, RegionHandleSet{sb}, text("")); });
   expect<InteractionClassNotPublished>("sendInteractionWithRegions, not published -> InteractionClassNotPublished",
                                        [&] { b.rti->sendInteractionWithRegions(message, none, RegionHandleSet{rq}, text("")); });

   a.rti->resignFederationExecution(DELETE_OBJECTS);
   b.rti->resignFederationExecution(NO_ACTION);
   a.rti->destroyFederationExecution(fed);
}
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
   std::wstring const fed = widen("ddmtest-" + run);
   std::cout << "federation: " << narrow(fed) << std::endl;

   try
   {
      Federate a("ddmtest-a-" + run), b("ddmtest-b-" + run);
      a.rti->createFederationExecution(fed, widen(std::string(HLA_TEST_FOM_DIR) + "/base.xml"));
      a.rti->joinFederationExecution(L"a", L"t", fed);
      b.rti->joinFederationExecution(L"b", L"t", fed);

      ObjectClassHandle const vehicle = a.rti->getObjectClassHandle(L"Vehicle");
      ObjectClassHandle const car = a.rti->getObjectClassHandle(L"Vehicle.Car");
      AttributeHandle const position = a.rti->getAttributeHandle(vehicle, L"Position"); // no dimension
      AttributeHandle const name = a.rti->getAttributeHandle(vehicle, L"Name");         // Zone
      AttributeHandle const speed = a.rti->getAttributeHandle(car, L"Speed");
      InteractionClassHandle const message = a.rti->getInteractionClassHandle(L"Message"); // Zone
      InteractionClassHandle const urgent = a.rti->getInteractionClassHandle(L"Message.Urgent");
      DimensionHandle const zone = a.rti->getDimensionHandle(L"Zone");

      // ------------------------------------------------ 9.2 createRegion
      RegionHandle const r = a.rti->createRegion(DimensionHandleSet{zone});
      check(r.isValid(), "createRegion returns a valid handle");
      check(a.rti->getDimensionHandleSet(r) == DimensionHandleSet{zone}, "getDimensionHandleSet = {Zone}");
      RangeBounds rb = a.rti->getRangeBounds(r, zone);
      check(rb.getLowerBound() == 0 && rb.getUpperBound() == 100, "new region spans [0, upper bound) = [0, 100)");
      expect<InvalidDimensionHandle>("createRegion with an invalid dimension -> InvalidDimensionHandle",
                                     [&] { a.rti->createRegion(DimensionHandleSet{DimensionHandle()}); });
      RegionHandle const r2 = a.rti->createRegion(DimensionHandleSet{zone});
      check(r2.isValid() && !(r2 == r), "a second region has another handle");
      check(a.rti->decodeRegionHandle(r.encode()) == r, "region handle encode/decode round trip");

      // ---------------------------------------- 10.29, 10.30 range bounds
      a.rti->setRangeBounds(r, zone, RangeBounds(10, 20));
      rb = a.rti->getRangeBounds(r, zone);
      check(rb.getLowerBound() == 10 && rb.getUpperBound() == 20, "setRangeBounds then getRangeBounds = [10, 20)");
      expect<InvalidRangeBound>("setRangeBounds lower >= upper -> InvalidRangeBound",
                                [&] { a.rti->setRangeBounds(r, zone, RangeBounds(20, 10)); });
      expect<InvalidRangeBound>("setRangeBounds beyond the dimension upper bound -> InvalidRangeBound",
                                [&] { a.rti->setRangeBounds(r, zone, RangeBounds(0, 101)); });
      RegionHandle const empty = a.rti->createRegion(DimensionHandleSet());
      check(a.rti->getDimensionHandleSet(empty).empty(), "region without dimension");
      expect<RegionDoesNotContainSpecifiedDimension>(
         "getRangeBounds on a missing dimension -> RegionDoesNotContainSpecifiedDimension",
         [&] { a.rti->getRangeBounds(empty, zone); });
      expect<RegionDoesNotContainSpecifiedDimension>(
         "setRangeBounds on a missing dimension -> RegionDoesNotContainSpecifiedDimension",
         [&] { a.rti->setRangeBounds(empty, zone, RangeBounds(1, 2)); });
      expect<InvalidRegion>("getDimensionHandleSet on an invalid region -> InvalidRegion",
                            [&] { a.rti->getDimensionHandleSet(RegionHandle()); });

      // ------------------------------------------ regions of another federate
      RegionHandle const rb1 = b.rti->createRegion(DimensionHandleSet{zone});
      expect<RegionNotCreatedByThisFederate>("setRangeBounds on B's region -> RegionNotCreatedByThisFederate",
                                             [&] { a.rti->setRangeBounds(rb1, zone, RangeBounds(1, 2)); });
      expect<RegionNotCreatedByThisFederate>(
         "commitRegionModifications with B's region -> RegionNotCreatedByThisFederate",
         [&] { a.rti->commitRegionModifications(RegionHandleSet{r, rb1}); });
      expect<RegionNotCreatedByThisFederate>("deleteRegion of B's region -> RegionNotCreatedByThisFederate",
                                             [&] { a.rti->deleteRegion(rb1); });
      expect<InvalidRegion>("getDimensionHandleSet on B's region -> InvalidRegion",
                            [&] { a.rti->getDimensionHandleSet(rb1); });

      // ------------------------------------------ 9.3, 9.4 commit, delete
      noThrow("commitRegionModifications", [&] { a.rti->commitRegionModifications(RegionHandleSet{r, r2}); });
      expect<InvalidRegion>("commitRegionModifications with an invalid region -> InvalidRegion",
                            [&] { a.rti->commitRegionModifications(RegionHandleSet{RegionHandle()}); });
      noThrow("deleteRegion of an unused region", [&] { a.rti->deleteRegion(empty); });
      expect<InvalidRegion>("deleted region -> InvalidRegion", [&] { a.rti->getDimensionHandleSet(empty); });
      expect<InvalidRegion>("deleteRegion twice -> InvalidRegion", [&] { a.rti->deleteRegion(empty); });

      // ------------------------------------ 9.5 registerObjectInstanceWithRegions
      expect<ObjectClassNotPublished>("registerObjectInstanceWithRegions, class not published -> ObjectClassNotPublished",
                                      [&] { a.rti->registerObjectInstanceWithRegions(vehicle, pairs(name, r)); });
      a.rti->publishObjectClassAttributes(vehicle, AttributeHandleSet{position, name});
      a.rti->publishObjectClassAttributes(car, AttributeHandleSet{speed});
      expect<AttributeNotDefined>("registerObjectInstanceWithRegions, attribute of a subclass -> AttributeNotDefined",
                                  [&] { a.rti->registerObjectInstanceWithRegions(vehicle, pairs(speed, r)); });
      expect<AttributeNotPublished>("registerObjectInstanceWithRegions, attribute not published -> AttributeNotPublished",
                                    [&] { a.rti->registerObjectInstanceWithRegions(car, pairs(name, r)); });
      expect<InvalidRegionContext>(
         "registerObjectInstanceWithRegions, region on an attribute without dimension -> InvalidRegionContext",
         [&] { a.rti->registerObjectInstanceWithRegions(vehicle, pairs(position, r)); });
      expect<RegionNotCreatedByThisFederate>(
         "registerObjectInstanceWithRegions with B's region -> RegionNotCreatedByThisFederate",
         [&] { a.rti->registerObjectInstanceWithRegions(vehicle, pairs(name, rb1)); });
      ObjectInstanceHandle obj;
      noThrow("registerObjectInstanceWithRegions(Vehicle, {Name: r})",
              [&] { obj = a.rti->registerObjectInstanceWithRegions(vehicle, pairs(name, r)); });
      expect<RegionInUseForUpdateOrSubscription>("deleteRegion of a region used for updates -> RegionInUse",
                                                 [&] { a.rti->deleteRegion(r); });

      // ---------------------------------------- 9.6, 9.7 (un)associate
      expect<ObjectInstanceNotKnown>("associateRegionsForUpdates on an unknown object -> ObjectInstanceNotKnown",
                                     [&] { a.rti->associateRegionsForUpdates(ObjectInstanceHandle(), pairs(name, r2)); });
      expect<InvalidRegionContext>("associateRegionsForUpdates on Position -> InvalidRegionContext",
                                   [&] { a.rti->associateRegionsForUpdates(obj, pairs(position, r2)); });
      noThrow("associateRegionsForUpdates(obj, {Name: r2})",
              [&] { a.rti->associateRegionsForUpdates(obj, pairs(name, r2)); });
      a.rti->setRangeBounds(r2, zone, RangeBounds(50, 60));
      noThrow("commitRegionModifications of an associated region (info record republished)",
              [&] { a.rti->commitRegionModifications(RegionHandleSet{r2}); });
      noThrow("unassociateRegionsForUpdates(obj, {Name: r})",
              [&] { a.rti->unassociateRegionsForUpdates(obj, pairs(name, r)); });
      noThrow("deleteRegion once no longer associated", [&] { a.rti->deleteRegion(r); });
      expect<InvalidRegion>("unassociateRegionsForUpdates with a deleted region -> InvalidRegion",
                            [&] { a.rti->unassociateRegionsForUpdates(obj, pairs(name, r)); });

      // ------------------------- 9.8, 9.9 object class subscription with regions
      RegionHandle const sub = b.rti->createRegion(DimensionHandleSet{zone});
      b.rti->commitRegionModifications(RegionHandleSet{sub});
      expect<InvalidRegionContext>(
         "subscribeObjectClassAttributesWithRegions on Position -> InvalidRegionContext",
         [&] { b.rti->subscribeObjectClassAttributesWithRegions(vehicle, pairs(position, sub)); });
      expect<ObjectClassNotDefined>(
         "subscribeObjectClassAttributesWithRegions, invalid class -> ObjectClassNotDefined",
         [&] { b.rti->subscribeObjectClassAttributesWithRegions(ObjectClassHandle(), pairs(name, sub)); });
      expect<InvalidUpdateRateDesignator>(
         "subscribeObjectClassAttributesWithRegions, unknown rate -> InvalidUpdateRateDesignator",
         [&] { b.rti->subscribeObjectClassAttributesWithRegions(vehicle, pairs(name, sub), true, L"Nope"); });
      b.rti->subscribeObjectClassAttributesWithRegions(vehicle, pairs(name, sub));
      check(b.pump([&] { return b.discovered.count(obj) != 0; }), "subscription with regions: B discovers the object");
      AttributeHandleValueMap values;
      values[name] = text("n");
      a.rti->updateAttributeValues(obj, values, text(""));
      check(b.pump([&] { return b.reflections == 1; }), "subscription with regions: B reflects Name");
      expect<RegionInUseForUpdateOrSubscription>("deleteRegion of a subscription region -> RegionInUse",
                                                 [&] { b.rti->deleteRegion(sub); });

      // default-region and region subscriptions coexist
      b.rti->subscribeObjectClassAttributes(vehicle, AttributeHandleSet{name});
      b.rti->unsubscribeObjectClass(vehicle);
      a.rti->updateAttributeValues(obj, values, text(""));
      check(b.pump([&] { return b.reflections == 2; }),
            "unsubscribeObjectClass keeps the subscription with regions");
      b.rti->unsubscribeObjectClassAttributesWithRegions(vehicle, pairs(name, sub));
      a.rti->updateAttributeValues(obj, values, text(""));
      b.settle();
      check(b.reflections == 2, "unsubscribeObjectClassAttributesWithRegions: no more reflections");
      noThrow("deleteRegion once unsubscribed", [&] { b.rti->deleteRegion(sub); });

      // -------------------------- 9.10, 9.11 interaction subscription with regions
      RegionHandle const isub = b.rti->createRegion(DimensionHandleSet{zone});
      expect<InvalidRegionContext>("subscribeInteractionClassWithRegions on a class without dimension -> InvalidRegionContext",
                                   [&] { b.rti->subscribeInteractionClassWithRegions(urgent, RegionHandleSet{isub}); });
      expect<InteractionClassNotDefined>(
         "subscribeInteractionClassWithRegions, invalid class -> InteractionClassNotDefined",
         [&] { b.rti->subscribeInteractionClassWithRegions(InteractionClassHandle(), RegionHandleSet{isub}); });
      expect<RegionNotCreatedByThisFederate>(
         "subscribeInteractionClassWithRegions with A's region -> RegionNotCreatedByThisFederate",
         [&] { b.rti->subscribeInteractionClassWithRegions(message, RegionHandleSet{r2}); });
      b.rti->subscribeInteractionClassWithRegions(message, RegionHandleSet{isub});
      a.rti->publishInteractionClass(message);
      a.settle(0.2);
      a.rti->sendInteraction(message, ParameterHandleValueMap(), text(""));
      check(b.pump([&] { return b.interactions == 1; }), "interaction subscription with regions: B receives Message");
      b.rti->unsubscribeInteractionClassWithRegions(message, RegionHandleSet{isub});
      a.rti->sendInteraction(message, ParameterHandleValueMap(), text(""));
      b.settle();
      check(b.interactions == 1, "unsubscribeInteractionClassWithRegions: no more interactions");
      noThrow("deleteRegion of the interaction region", [&] { b.rti->deleteRegion(isub); });

      // ---------------------------------------------------------- resign
      a.rti->resignFederationExecution(DELETE_OBJECTS);
      b.rti->resignFederationExecution(NO_ACTION);
      a.rti->destroyFederationExecution(fed);
      filterTests(run);
   }
   catch (Exception const& e)
   {
      check(false, "unexpected exception: " + narrow(e.what()));
   }

   std::cout << (failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED") << " (" << checks << " checks, "
             << failures << " failure(s))" << std::endl;
   return failures == 0 ? 0 : 1;
}
