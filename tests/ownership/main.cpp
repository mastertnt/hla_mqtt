// hla-ownership-test: ownership management (IEEE 1516.1-2010, chapter 7)
// between ambassadors in one process: unconditional and negotiated
// divestiture, acquisition (with and without "if available"), release
// denied, divestiture if wanted, cancellations, queries, concurrent
// acquisitions of an unowned attribute, the delete privilege, resign actions,
// the loss of an owner and the MOM HLAmodifyAttributeState. Needs a broker.
//
//   hla-ownership-test [--broker mqtt://host:1883]
//
// Standard API only. Exit code 0 = all checks passed.

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS // std::getenv
#endif

#include <RTI/NullFederateAmbassador.h>
#include <RTI/RTI1516.h>
#include <RTI/encoding/BasicDataElements.h>
#include <RTI/encoding/HLAopaqueData.h>

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

VariableLengthData text(std::string const& s)
{
   return VariableLengthData(s.data(), s.size());
}

template <class H>
VariableLengthData encHandle(H const& h) // HLAhandle
{
   VariableLengthData const e = h.encode();
   return HLAopaqueData(static_cast<Octet const*>(e.data()), e.size()).encode();
}

std::string str(VariableLengthData const& d)
{
   return d.size() ? std::string(static_cast<char const*>(d.data()), d.size()) : std::string();
}

// One ownership callback.
struct Event
{
   std::string kind;
   ObjectInstanceHandle object;
   AttributeHandleSet attrs;
   std::string tag;
   FederateHandle owner;
};

struct Federate : NullFederateAmbassador
{
   std::unique_ptr<RTIambassador> rti;
   std::map<ObjectInstanceHandle, ObjectClassHandle> discovered;
   std::set<ObjectInstanceHandle> removed;
   std::map<ObjectInstanceHandle, AttributeHandleValueMap> values;
   std::vector<Event> events;
   std::vector<InteractionClassHandle> interactions;

   explicit Federate(std::string const& clientId)
   {
      rti = RTIambassadorFactory().createRTIambassador();
      rti->connect(*this, HLA_EVOKED, broker + L";clientId=" + widen(clientId));
   }

   // Index of the first event of `kind` at or after `from` (-1: none).
   int find(std::string const& kind, size_t from = 0, AttributeHandleSet const* attrs = nullptr) const
   {
      for (size_t i = from; i < events.size(); ++i)
         if (events[i].kind == kind && (!attrs || events[i].attrs == *attrs))
            return int(i);
      return -1;
   }
   Event const* last(std::string const& kind) const
   {
      for (size_t i = events.size(); i > 0; --i)
         if (events[i - 1].kind == kind)
            return &events[i - 1];
      return nullptr;
   }

   void add(std::string kind, ObjectInstanceHandle o, AttributeHandleSet const& a, std::string tag = std::string(),
            FederateHandle owner = FederateHandle())
   {
      events.push_back(Event{std::move(kind), o, a, std::move(tag), owner});
   }

   // ---------------------------------------------------------- objects
   void discoverObjectInstance(ObjectInstanceHandle o, ObjectClassHandle c, std::wstring const&)
      RTI_THROW((FederateInternalError)) override
   {
      discovered[o] = c;
   }
   void removeObjectInstance(ObjectInstanceHandle o, VariableLengthData const&, OrderType, SupplementalRemoveInfo)
      RTI_THROW((FederateInternalError)) override
   {
      discovered.erase(o);
      removed.insert(o);
   }
   void reflectAttributeValues(ObjectInstanceHandle o, AttributeHandleValueMap const& v, VariableLengthData const&,
                               OrderType, TransportationType, SupplementalReflectInfo)
      RTI_THROW((FederateInternalError)) override
   {
      for (auto const& kv : v)
         values[o][kv.first] = kv.second;
   }

   void receiveInteraction(InteractionClassHandle c, ParameterHandleValueMap const&, VariableLengthData const&,
                           OrderType, TransportationType, SupplementalReceiveInfo) RTI_THROW((FederateInternalError)) override
   {
      interactions.push_back(c);
   }

   // -------------------------------------------------------- ownership
   void requestAttributeOwnershipAssumption(ObjectInstanceHandle o, AttributeHandleSet const& a,
                                            VariableLengthData const& tag) RTI_THROW((FederateInternalError)) override
   {
      add("assumption", o, a, str(tag));
   }
   void requestDivestitureConfirmation(ObjectInstanceHandle o, AttributeHandleSet const& a)
      RTI_THROW((FederateInternalError)) override
   {
      add("confirm", o, a);
   }
   void attributeOwnershipAcquisitionNotification(ObjectInstanceHandle o, AttributeHandleSet const& a,
                                                  VariableLengthData const& tag) RTI_THROW((FederateInternalError)) override
   {
      add("acquired", o, a, str(tag));
   }
   void attributeOwnershipUnavailable(ObjectInstanceHandle o, AttributeHandleSet const& a)
      RTI_THROW((FederateInternalError)) override
   {
      add("unavailable", o, a);
   }
   void requestAttributeOwnershipRelease(ObjectInstanceHandle o, AttributeHandleSet const& a,
                                         VariableLengthData const& tag) RTI_THROW((FederateInternalError)) override
   {
      add("release", o, a, str(tag));
   }
   void confirmAttributeOwnershipAcquisitionCancellation(ObjectInstanceHandle o, AttributeHandleSet const& a)
      RTI_THROW((FederateInternalError)) override
   {
      add("cancelled", o, a);
   }
   void informAttributeOwnership(ObjectInstanceHandle o, AttributeHandle a, FederateHandle f)
      RTI_THROW((FederateInternalError)) override
   {
      add("owner", o, AttributeHandleSet{a}, std::string(), f);
   }
   void attributeIsNotOwned(ObjectInstanceHandle o, AttributeHandle a) RTI_THROW((FederateInternalError)) override
   {
      add("notowned", o, AttributeHandleSet{a});
   }
   void attributeIsOwnedByRTI(ObjectInstanceHandle o, AttributeHandle a) RTI_THROW((FederateInternalError)) override
   {
      add("rti", o, AttributeHandleSet{a});
   }
};

// Evokes the callbacks of all the federates until `done` (or the timeout).
bool pump(std::vector<Federate*> const& feds, std::function<bool()> done, double seconds = 4.0)
{
   auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
   while (!done())
   {
      if (std::chrono::steady_clock::now() > deadline)
         return false;
      for (Federate* f : feds)
         f->rti->evokeMultipleCallbacks(0.0, 0.02);
   }
   return true;
}

void settleAll(std::vector<Federate*> const& feds, double seconds = 0.5)
{
   pump(feds, [] { return false; }, seconds);
}

// Answer of queryAttributeOwnership: "owner" (with its handle), "notowned" or "rti".
Event query(Federate& f, std::vector<Federate*> const& feds, ObjectInstanceHandle o, AttributeHandle a)
{
   size_t const from = f.events.size();
   f.rti->queryAttributeOwnership(o, a);
   Event answer;
   pump(feds, [&] {
      for (size_t i = from; i < f.events.size(); ++i)
         if (f.events[i].kind == "owner" || f.events[i].kind == "notowned" || f.events[i].kind == "rti")
         {
            answer = f.events[i];
            return true;
         }
      return false;
   });
   return answer;
}

// Polls queryAttributeOwnership until the answer matches.
bool ownerBecomes(Federate& f, std::vector<Federate*> const& feds, ObjectInstanceHandle o, AttributeHandle a,
                  std::string const& kind, FederateHandle owner = FederateHandle(), double seconds = 4.0)
{
   auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
   while (std::chrono::steady_clock::now() < deadline)
   {
      Event const e = query(f, feds, o, a);
      if (e.kind == kind && (kind != "owner" || e.owner == owner))
         return true;
      settleAll(feds, 0.1);
   }
   return false;
}

template <class Exc, class Fn>
bool throws(Fn fn)
{
   try
   {
      fn();
   }
   catch (Exc const&)
   {
      return true;
   }
   catch (Exception const& e)
   {
      std::cout << "   (other exception: " << narrow(e.what()) << ")" << std::endl;
      return false;
   }
   return false;
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
   std::wstring const fed = widen("owntest-" + run);
   std::cout << "federation: " << narrow(fed) << std::endl;

   try
   {
      Federate a("owntest-a-" + run), b("owntest-b-" + run), c("owntest-c-" + run);
      std::vector<Federate*> const all{&a, &b, &c};
      a.rti->createFederationExecution(fed, widen(std::string(HLA_TEST_FOM_DIR) + "/base.xml"));
      FederateHandle const ha = a.rti->joinFederationExecution(L"a", L"t", fed);
      FederateHandle const hb = b.rti->joinFederationExecution(L"b", L"t", fed);
      FederateHandle const hc = c.rti->joinFederationExecution(L"c", L"t", fed);
      RTIambassador& ra = *a.rti;
      RTIambassador& rb = *b.rti;
      RTIambassador& rc = *c.rti;

      ObjectClassHandle const vehicle = ra.getObjectClassHandle(L"Vehicle");
      AttributeHandle const pos = ra.getAttributeHandle(vehicle, L"Position");
      AttributeHandle const name = ra.getAttributeHandle(vehicle, L"Name");
      AttributeHandle const priv = ra.getAttributeHandle(vehicle, L"HLAprivilegeToDeleteObject");
      AttributeHandleSet const both{pos, name};
      AttributeHandleSet const onlyPos{pos}, onlyName{name}, onlyPriv{priv};
      // A and B also publish the delete privilege; C does not.
      ra.publishObjectClassAttributes(vehicle, AttributeHandleSet{pos, name, priv});
      rb.publishObjectClassAttributes(vehicle, AttributeHandleSet{pos, name, priv});
      rc.publishObjectClassAttributes(vehicle, both);
      for (Federate* f : all)
         f->rti->subscribeObjectClassAttributes(vehicle, both);

      ObjectInstanceHandle const v = ra.registerObjectInstance(vehicle);
      check(pump(all, [&] { return b.discovered.count(v) && c.discovered.count(v); }), "B and C discover A's vehicle");

      // ------------------------------------------------------- #18 queries
      check(ra.isAttributeOwnedByFederate(v, pos) && ra.isAttributeOwnedByFederate(v, priv),
            "isAttributeOwnedByFederate: the registrant owns Position and the delete privilege");
      check(!rb.isAttributeOwnedByFederate(v, pos), "isAttributeOwnedByFederate: B does not own Position");
      Event e = query(b, all, v, pos);
      check(e.kind == "owner" && e.owner == ha, "queryAttributeOwnership -> informAttributeOwnership(A)");
      e = query(c, all, v, priv);
      check(e.kind == "owner" && e.owner == ha, "delete privilege owned by A");
      {
         ObjectClassHandle const federateClass = rb.getObjectClassHandle(L"HLAmanager.HLAfederate");
         AttributeHandle const fName = rb.getAttributeHandle(federateClass, L"HLAfederateName");
         rb.subscribeObjectClassAttributes(federateClass, AttributeHandleSet{fName});
         ObjectInstanceHandle momObject;
         pump(all, [&] {
            for (auto const& kv : b.discovered)
               if (kv.second == federateClass)
                  momObject = kv.first;
            return momObject.isValid();
         });
         e = momObject.isValid() ? query(b, all, momObject, fName) : Event();
         check(e.kind == "rti", "MOM HLAfederate object -> attributeIsOwnedByRTI");
         rb.unsubscribeObjectClass(federateClass);
      }

      // ------------------------------------------------------ exceptions
      check(throws<AttributeNotOwned>([&] { rb.unconditionalAttributeOwnershipDivestiture(v, onlyPos); }),
            "unconditional divestiture of an attribute not owned -> AttributeNotOwned");
      check(throws<FederateOwnsAttributes>([&] { ra.attributeOwnershipAcquisition(v, onlyPos, text("")); }),
            "acquisition of an owned attribute -> FederateOwnsAttributes");
      check(throws<AttributeNotPublished>([&] { rc.attributeOwnershipAcquisition(v, onlyPriv, text("")); }),
            "acquisition of an attribute not published -> AttributeNotPublished");
      check(throws<AttributeDivestitureWasNotRequested>([&] { ra.confirmDivestiture(v, onlyPos, text("")); }),
            "confirmDivestiture without negotiated divestiture -> AttributeDivestitureWasNotRequested");
      check(throws<AttributeAcquisitionWasNotRequested>([&] { rb.cancelAttributeOwnershipAcquisition(v, onlyPos); }),
            "cancelAttributeOwnershipAcquisition without acquisition -> AttributeAcquisitionWasNotRequested");
      check(throws<AttributeDivestitureWasNotRequested>(
               [&] { ra.cancelNegotiatedAttributeOwnershipDivestiture(v, onlyPos); }),
            "cancelNegotiatedAttributeOwnershipDivestiture without divestiture -> AttributeDivestitureWasNotRequested");

      // ------------------------------------------ #14 unconditional divestiture
      ra.unconditionalAttributeOwnershipDivestiture(v, onlyName);
      check(!ra.isAttributeOwnedByFederate(v, name), "unconditional divestiture: A no longer owns Name");
      check(throws<AttributeNotOwned>([&] {
               AttributeHandleValueMap values;
               values[name] = text("n");
               ra.updateAttributeValues(v, values, text(""));
            }),
            "update of a divested attribute -> AttributeNotOwned");
      check(ownerBecomes(b, all, v, name, "notowned"), "Name unowned for B (attributeIsNotOwned)");

      // ----------------------------- #20 concurrent acquisitions of an unowned attribute
      size_t const mb = b.events.size(), mc = c.events.size();
      rb.attributeOwnershipAcquisition(v, onlyName, text("from-b"));
      rc.attributeOwnershipAcquisition(v, onlyName, text("from-c"));
      pump(all, [&] { return b.find("acquired", mb) >= 0 || c.find("acquired", mc) >= 0; });
      settleAll(all, 1.0);
      bool const bWon = b.find("acquired", mb) >= 0, cWon = c.find("acquired", mc) >= 0;
      check(bWon != cWon, "concurrent acquisitions of an unowned attribute: exactly one federate acquires it");
      Federate& winner = bWon ? b : c;
      Federate& loser = bWon ? c : b;
      FederateHandle const hw = bWon ? hb : hc;
      check(winner.last("acquired") && winner.last("acquired")->tag == (bWon ? "from-b" : "from-c"),
            "acquisition notification carries the acquirer's tag");
      check(winner.rti->isAttributeOwnedByFederate(v, name) && !loser.rti->isAttributeOwnedByFederate(v, name),
            "only the winner owns Name");
      check(pump(all, [&] { return winner.find("release") >= 0; }),
            "the loser's acquisition becomes a request to the winner (requestAttributeOwnershipRelease)");
      check(winner.last("release") && winner.last("release")->tag == (bWon ? "from-c" : "from-b"),
            "release request carries the requester's tag");
      check(ownerBecomes(a, all, v, name, "owner", hw), "A sees the winner as owner of Name");
      size_t ml = loser.events.size();
      check(throws<AttributeAlreadyBeingAcquired>(
               [&] { loser.rti->attributeOwnershipAcquisitionIfAvailable(v, onlyName); }),
            "acquisitionIfAvailable during an acquisition -> AttributeAlreadyBeingAcquired");
      loser.rti->cancelAttributeOwnershipAcquisition(v, onlyName);
      check(pump(all, [&] { return loser.find("cancelled", ml, &onlyName) >= 0; }),
            "cancelAttributeOwnershipAcquisition -> confirmAttributeOwnershipAcquisitionCancellation");

      // --------------------------------------- #15 negotiated divestiture
      size_t const ma = a.events.size();
      ml = loser.events.size();
      winner.rti->negotiatedAttributeOwnershipDivestiture(v, onlyName, text("take-it"));
      check(throws<AttributeAlreadyBeingDivested>(
               [&] { winner.rti->negotiatedAttributeOwnershipDivestiture(v, onlyName, text("")); }),
            "negotiated divestiture twice -> AttributeAlreadyBeingDivested");
      check(pump(all, [&] { return a.find("assumption", ma) >= 0 && loser.find("assumption", ml) >= 0; }),
            "negotiated divestiture -> requestAttributeOwnershipAssumption at the other publishers");
      check(a.last("assumption") && a.last("assumption")->tag == "take-it" && a.last("assumption")->attrs == onlyName,
            "assumption request carries the attribute and the tag");
      size_t const mw = winner.events.size();
      ra.attributeOwnershipAcquisitionIfAvailable(v, onlyName);
      check(pump(all, [&] { return winner.find("confirm", mw, &onlyName) >= 0; }),
            "acquisition of an offered attribute -> requestDivestitureConfirmation");
      winner.rti->confirmDivestiture(v, onlyName, text(""));
      check(!winner.rti->isAttributeOwnedByFederate(v, name), "confirmDivestiture: the winner no longer owns Name");
      check(pump(all, [&] { return a.find("acquired", ma, &onlyName) >= 0; }), "A acquires Name (if available)");
      check(ra.isAttributeOwnedByFederate(v, name), "A owns Name again");
      check(ownerBecomes(c, all, v, name, "owner", ha), "C sees A as owner of Name");

      // ------------------------------- #16 / #17 acquisition and release
      size_t const mb2 = b.events.size();
      size_t ma2 = a.events.size();
      rb.attributeOwnershipAcquisition(v, onlyPos, text("please"));
      check(pump(all, [&] { return a.find("release", ma2, &onlyPos) >= 0; }),
            "acquisition of an owned attribute -> requestAttributeOwnershipRelease at the owner");
      check(a.last("release") && a.last("release")->tag == "please", "release request tag");
      AttributeHandleSet divested;
      ra.attributeOwnershipDivestitureIfWanted(v, both, divested);
      check(divested == onlyPos, "attributeOwnershipDivestitureIfWanted divests the requested attribute only");
      check(pump(all, [&] { return b.find("acquired", mb2, &onlyPos) >= 0; }), "B acquires Position");
      check(b.last("acquired") && b.last("acquired")->tag == "please", "acquisition notification carries B's tag");
      {
         AttributeHandleValueMap values;
         values[pos] = text("from-b");
         rb.updateAttributeValues(v, values, text(""));
         check(pump(all, [&] { return c.values[v].count(pos) && str(c.values[v][pos]) == "from-b"; }),
               "the new owner's updates are reflected");
      }

      // release denied: the request stays pending until cancelled
      size_t const mc2 = c.events.size();
      size_t const mb3 = b.events.size();
      rc.attributeOwnershipAcquisition(v, onlyPos, text(""));
      check(pump(all, [&] { return b.find("release", mb3, &onlyPos) >= 0; }), "C's request reaches the owner B");
      rb.attributeOwnershipReleaseDenied(v, onlyPos);
      settleAll(all, 0.6);
      check(c.find("acquired", mc2) < 0 && rb.isAttributeOwnedByFederate(v, pos),
            "attributeOwnershipReleaseDenied: B keeps Position, C's acquisition stays pending");
      rc.cancelAttributeOwnershipAcquisition(v, onlyPos);
      check(pump(all, [&] { return c.find("cancelled", mc2, &onlyPos) >= 0; }), "C cancels its pending acquisition");
      AttributeHandleSet none;
      rb.attributeOwnershipDivestitureIfWanted(v, onlyPos, none);
      check(none.empty(), "divestiture if wanted after the cancellation: nothing divested");

      // if available on a non-offered attribute
      size_t const mc3 = c.events.size();
      rc.attributeOwnershipAcquisitionIfAvailable(v, onlyPos);
      check(pump(all, [&] { return c.find("unavailable", mc3, &onlyPos) >= 0; }),
            "acquisitionIfAvailable of an owned, not offered attribute -> attributeOwnershipUnavailable");

      // cancelled negotiated divestiture: no longer available
      ra.negotiatedAttributeOwnershipDivestiture(v, onlyName, text(""));
      ra.cancelNegotiatedAttributeOwnershipDivestiture(v, onlyName);
      size_t const mc4 = c.events.size();
      rc.attributeOwnershipAcquisitionIfAvailable(v, onlyName);
      check(pump(all, [&] { return c.find("unavailable", mc4, &onlyName) >= 0; }),
            "cancelNegotiatedAttributeOwnershipDivestiture: the attribute is no longer available");
      check(ra.isAttributeOwnedByFederate(v, name), "A still owns Name");

      // ----------------------------------------------- delete privilege
      size_t const mb4 = b.events.size();
      ma2 = a.events.size();
      rb.attributeOwnershipAcquisition(v, onlyPriv, text(""));
      check(pump(all, [&] { return a.find("release", ma2, &onlyPriv) >= 0; }), "B asks for the delete privilege");
      ra.unconditionalAttributeOwnershipDivestiture(v, onlyPriv);
      check(pump(all, [&] { return b.find("acquired", mb4, &onlyPriv) >= 0; }),
            "unconditional divestiture with a pending request: B acquires the privilege");
      check(throws<DeletePrivilegeNotHeld>([&] { ra.deleteObjectInstance(v, text("")); }),
            "A no longer holds the delete privilege -> DeletePrivilegeNotHeld");
      rb.deleteObjectInstance(v, text(""));
      check(pump(all, [&] { return a.removed.count(v) && c.removed.count(v); }),
            "B deletes A's vehicle: A and C remove it");
      check(throws<ObjectInstanceNotKnown>([&] { ra.getObjectInstanceName(v); }),
            "the registrant no longer knows the deleted object");

      // --------------------------------------------------- resign actions
      ObjectInstanceHandle const v2 = ra.registerObjectInstance(vehicle);
      check(pump(all, [&] { return b.discovered.count(v2) && c.discovered.count(v2); }), "second vehicle discovered");
      ma2 = a.events.size();
      size_t const mb5 = b.events.size();
      rb.attributeOwnershipAcquisition(v2, onlyName, text(""));
      pump(all, [&] { return a.find("release", ma2, &onlyName) >= 0; });
      ra.attributeOwnershipDivestitureIfWanted(v2, onlyName, divested);
      check(pump(all, [&] { return b.find("acquired", mb5, &onlyName) >= 0; }), "B owns Name of the second vehicle");
      check(throws<FederateOwnsAttributes>([&] { rb.resignFederationExecution(NO_ACTION); }),
            "resign NO_ACTION while owning attributes -> FederateOwnsAttributes");
      rb.attributeOwnershipAcquisition(v2, onlyPos, text("")); // left unanswered
      check(throws<OwnershipAcquisitionPending>([&] { rb.resignFederationExecution(UNCONDITIONALLY_DIVEST_ATTRIBUTES); }),
            "resign without cancellation while acquiring -> OwnershipAcquisitionPending");
      rb.resignFederationExecution(CANCEL_THEN_DELETE_THEN_DIVEST);
      check(true, "resign CANCEL_THEN_DELETE_THEN_DIVEST");
      std::vector<Federate*> const ac{&a, &c};
      check(ownerBecomes(a, ac, v2, name, "notowned"), "after B's resign, Name of the second vehicle is unowned");
      settleAll(ac, 0.3);
      AttributeHandleSet stillWanted;
      ra.attributeOwnershipDivestitureIfWanted(v2, onlyPos, stillWanted);
      check(stillWanted.empty(), "B's cancelled acquisition is no longer pending at A");
      check(!a.removed.count(v2) && !c.removed.count(v2), "the second vehicle stays in the federation");

      // --------------------------------------------------- loss of an owner
      size_t const removedA = a.removed.size(), removedC = c.removed.size();
      {
         Federate d("owntest-d-" + run);
         RTIambassador& rdd = *d.rti;
         FederateHandle const hd = rdd.joinFederationExecution(L"d", L"t", fed);
         rdd.publishObjectClassAttributes(vehicle, both);
         rdd.subscribeObjectClassAttributes(vehicle, both);
         std::vector<Federate*> const withD{&a, &c, &d};
         ObjectInstanceHandle const v3 = rdd.registerObjectInstance(vehicle);
         check(pump(withD, [&] { return a.discovered.count(v3) && c.discovered.count(v3) && d.discovered.count(v2); }),
               "D registers a vehicle; D discovers the second vehicle");
         size_t const md = d.events.size();
         rdd.attributeOwnershipAcquisition(v2, onlyName, text(""));
         check(pump(withD, [&] { return d.find("acquired", md, &onlyName) >= 0; }),
               "D claims the unowned Name of the second vehicle");
         check(ownerBecomes(c, withD, v2, name, "owner", hd), "C sees D as owner of Name");
         settleAll(withD, 0.2);
      } // destroyed without resign
      check(pump(ac, [&] { return a.removed.size() > removedA && c.removed.size() > removedC; }, 8.0),
            "owner lost: the objects it held the delete privilege of are removed");
      check(ownerBecomes(c, ac, v2, name, "notowned", FederateHandle(), 6.0), "owner lost: its attributes become unowned");
      size_t const ma3 = a.events.size();
      ra.attributeOwnershipAcquisitionIfAvailable(v2, onlyName);
      check(pump(ac, [&] { return a.find("acquired", ma3, &onlyName) >= 0; }),
            "the attribute of the lost owner is acquired again");

      // ------------------------------------- MOM HLAmodifyAttributeState
      {
         InteractionClassHandle const modify =
            ra.getInteractionClassHandle(L"HLAmanager.HLAfederate.HLAadjust.HLAmodifyAttributeState");
         InteractionClassHandle const momExc =
            ra.getInteractionClassHandle(L"HLAmanager.HLAfederate.HLAreport.HLAreportMOMexception");
         ra.publishInteractionClass(modify);
         ra.subscribeInteractionClass(momExc);
         settleAll(ac, 0.3);
         auto sendModify = [&](FederateHandle target, AttributeHandle attribute, int state) {
            ParameterHandleValueMap pv;
            pv[ra.getParameterHandle(modify, L"HLAfederate")] = encHandle(target);
            pv[ra.getParameterHandle(modify, L"HLAobjectInstance")] = encHandle(v2);
            pv[ra.getParameterHandle(modify, L"HLAattribute")] = encHandle(attribute);
            pv[ra.getParameterHandle(modify, L"HLAattributeState")] = HLAinteger32BE(state).encode();
            ra.sendInteraction(modify, pv, text(""));
         };
         size_t const ea = a.events.size(), ec = c.events.size();
         sendModify(hc, pos, 1);
         check(pump(ac, [&] { return rc.isAttributeOwnedByFederate(v2, pos); }),
               "HLAmodifyAttributeState owned: C takes Position from its owner A");
         check(!ra.isAttributeOwnedByFederate(v2, pos), "A no longer owns Position");
         settleAll(ac, 0.3);
         check(a.events.size() == ea && c.events.size() == ec, "no ownership callback at A or C");
         check(ownerBecomes(a, ac, v2, pos, "owner", hc), "A sees C as owner of Position");
         sendModify(hc, pos, 0);
         check(pump(ac, [&] { return !rc.isAttributeOwnedByFederate(v2, pos); }),
               "HLAmodifyAttributeState unowned: C releases Position");
         check(ownerBecomes(a, ac, v2, pos, "notowned"), "Position unowned");
         size_t const ia = a.interactions.size();
         sendModify(hc, priv, 1); // not published by C
         check(pump(ac, [&] {
                  for (size_t i = ia; i < a.interactions.size(); ++i)
                     if (a.interactions[i] == momExc)
                        return true;
                  return false;
               }),
               "attribute not published by the target -> HLAreportMOMexception");
         check(ra.isAttributeOwnedByFederate(v2, priv) && !rc.isAttributeOwnedByFederate(v2, priv),
               "the failed modification has no effect");
      }

      rc.resignFederationExecution(CANCEL_THEN_DELETE_THEN_DIVEST);
      ra.resignFederationExecution(CANCEL_THEN_DELETE_THEN_DIVEST);
      ra.destroyFederationExecution(fed);
   }
   catch (Exception const& e)
   {
      check(false, "unexpected exception: " + narrow(e.what()));
   }

   std::cout << (checks - failures) << "/" << checks << " checks passed" << std::endl;
   return failures == 0 ? 0 : 1;
}
