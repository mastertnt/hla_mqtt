// hla-time-test: synchronization points and time management between several
// ambassadors (several MQTT connections) in one process. Needs a broker.
//
//   hla-time-test [--broker mqtt://host:1883]
//
// Standard API only. Exit code 0 = all checks passed.

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS // std::getenv
#endif

#include <RTI/NullFederateAmbassador.h>
#include <RTI/RTI1516.h>
#include <RTI/time/HLAfloat64Time.h>
#include <RTI/time/HLAinteger64Interval.h>
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

std::int64_t itime(LogicalTime const& t)
{
   return dynamic_cast<HLAinteger64Time const&>(t).getTime();
}

struct Event // a received message
{
   std::string what;
   std::int64_t time = -1;
   OrderType received = RECEIVE;
};

struct Federate : NullFederateAmbassador
{
   std::unique_ptr<RTIambassador> rti;
   // sync
   std::map<std::wstring, bool> registration;
   std::map<std::wstring, std::string> announced;
   std::map<std::wstring, FederateHandleSet> synchronized;
   // time
   std::int64_t regulationEnabled = -1, constrainedEnabled = -1;
   std::vector<std::int64_t> grants;
   std::vector<Event> events;
   std::vector<MessageRetractionHandle> retractions;

   explicit Federate(std::string const& clientId)
   {
      rti = RTIambassadorFactory().createRTIambassador();
      rti->connect(*this, HLA_EVOKED, broker + L";clientId=" + widen(clientId));
   }

   bool pump(std::function<bool()> done, double seconds = 5.0)
   {
      auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
      while (!done())
      {
         if (std::chrono::steady_clock::now() > deadline)
            return false;
         rti->evokeMultipleCallbacks(0.0, 0.05);
         if (!done())
            rti->evokeCallback(0.005);
      }
      return true;
   }
   void settle(double seconds) { rti->evokeMultipleCallbacks(seconds, seconds + 0.2); }
   std::int64_t lastGrant() const { return grants.empty() ? -1 : grants.back(); }

   // ---------------------------------------------------------- sync
   void synchronizationPointRegistrationSucceeded(std::wstring const& l) RTI_THROW((FederateInternalError)) override
   {
      registration[l] = true;
   }
   void synchronizationPointRegistrationFailed(std::wstring const& l, SynchronizationPointFailureReason)
      RTI_THROW((FederateInternalError)) override
   {
      registration[l] = false;
   }
   void announceSynchronizationPoint(std::wstring const& l, VariableLengthData const& tag)
      RTI_THROW((FederateInternalError)) override
   {
      announced[l] = str(tag);
   }
   void federationSynchronized(std::wstring const& l, FederateHandleSet const& failed)
      RTI_THROW((FederateInternalError)) override
   {
      synchronized[l] = failed;
   }
   // ---------------------------------------------------------- time
   void timeRegulationEnabled(LogicalTime const& t) RTI_THROW((FederateInternalError)) override
   {
      regulationEnabled = itime(t);
   }
   void timeConstrainedEnabled(LogicalTime const& t) RTI_THROW((FederateInternalError)) override
   {
      constrainedEnabled = itime(t);
   }
   void timeAdvanceGrant(LogicalTime const& t) RTI_THROW((FederateInternalError)) override
   {
      grants.push_back(itime(t));
   }
   void receiveInteraction(InteractionClassHandle, ParameterHandleValueMap const& v, VariableLengthData const& tag,
                           OrderType, TransportationType, SupplementalReceiveInfo) RTI_THROW((FederateInternalError)) override
   {
      events.push_back(Event{"i:" + str(tag), -1, RECEIVE});
   }
   void receiveInteraction(InteractionClassHandle, ParameterHandleValueMap const&, VariableLengthData const& tag,
                           OrderType, TransportationType, LogicalTime const& t, OrderType received,
                           SupplementalReceiveInfo) RTI_THROW((FederateInternalError)) override
   {
      events.push_back(Event{"i:" + str(tag), itime(t), received});
   }
   void receiveInteraction(InteractionClassHandle, ParameterHandleValueMap const&, VariableLengthData const& tag,
                           OrderType, TransportationType, LogicalTime const& t, OrderType received,
                           MessageRetractionHandle, SupplementalReceiveInfo) RTI_THROW((FederateInternalError)) override
   {
      events.push_back(Event{"i:" + str(tag), itime(t), received});
   }
   void reflectAttributeValues(ObjectInstanceHandle, AttributeHandleValueMap const&, VariableLengthData const& tag,
                               OrderType, TransportationType, LogicalTime const& t, OrderType received,
                               MessageRetractionHandle, SupplementalReflectInfo) RTI_THROW((FederateInternalError)) override
   {
      events.push_back(Event{"u:" + str(tag), itime(t), received});
   }
   void reflectAttributeValues(ObjectInstanceHandle, AttributeHandleValueMap const&, VariableLengthData const& tag,
                               OrderType, TransportationType, LogicalTime const& t, OrderType received,
                               SupplementalReflectInfo) RTI_THROW((FederateInternalError)) override
   {
      events.push_back(Event{"u:" + str(tag), itime(t), received});
   }
   void discoverObjectInstance(ObjectInstanceHandle, ObjectClassHandle, std::wstring const&)
      RTI_THROW((FederateInternalError)) override
   {
      ++discovered;
   }
   void requestRetraction(MessageRetractionHandle h) RTI_THROW((FederateInternalError)) override
   {
      retractions.push_back(h);
   }
   int discovered = 0;
};

bool hasEvent(std::vector<Event> const& events, std::string const& what)
{
   for (Event const& e : events)
      if (e.what == what)
         return true;
   return false;
}

void syncTests(std::string const& run)
{
   std::wstring const fed = widen("synctest-" + run);
   Federate a("sync-a-" + run), b("sync-b-" + run), c("sync-c-" + run);
   a.rti->createFederationExecution(fed, fom("base.xml"));
   FederateHandle const ha = a.rti->joinFederationExecution(L"a", L"t", fed);
   FederateHandle const hb = b.rti->joinFederationExecution(L"b", L"t", fed);
   FederateHandle const hc = c.rti->joinFederationExecution(L"c", L"t", fed);
   // Let everybody see everybody (federate records).
   a.settle(0.2);

   a.rti->registerFederationSynchronizationPoint(L"ready", text("go"));
   check(a.pump([&] { return a.registration.count(L"ready") != 0; }) && a.registration[L"ready"],
         "registration succeeded");
   check(a.pump([&] { return a.announced.count(L"ready") != 0; }) &&
            b.pump([&] { return b.announced.count(L"ready") != 0; }) &&
            c.pump([&] { return c.announced.count(L"ready") != 0; }) && b.announced[L"ready"] == "go",
         "announced to every joined federate, with the tag");
   b.rti->registerFederationSynchronizationPoint(L"ready", text(""));
   check(b.pump([&] { return b.registration.count(L"ready") != 0; }) && !b.registration[L"ready"],
         "same label again -> registration failed");
   expect<SynchronizationPointLabelNotAnnounced>("achieve an unknown label -> SynchronizationPointLabelNotAnnounced",
                                                 [&] { a.rti->synchronizationPointAchieved(L"nope", true); });
   a.rti->synchronizationPointAchieved(L"ready", true);
   b.rti->synchronizationPointAchieved(L"ready", true);
   a.settle(0.3);
   check(a.synchronized.count(L"ready") == 0, "not synchronized while C has not achieved");
   c.rti->synchronizationPointAchieved(L"ready", false);
   check(a.pump([&] { return a.synchronized.count(L"ready") != 0; }) &&
            b.pump([&] { return b.synchronized.count(L"ready") != 0; }) &&
            c.pump([&] { return c.synchronized.count(L"ready") != 0; }) &&
            a.synchronized[L"ready"] == FederateHandleSet{hc},
         "federationSynchronized for everybody, C in the failed set");

   // Explicit set {A, B}: C is not announced.
   a.rti->registerFederationSynchronizationPoint(L"pair", text(""), FederateHandleSet{ha, hb});
   check(b.pump([&] { return b.announced.count(L"pair") != 0; }), "explicit set: B announced");
   c.settle(0.3);
   check(c.announced.count(L"pair") == 0, "explicit set: C not announced");
   a.rti->synchronizationPointAchieved(L"pair", true);
   b.rti->synchronizationPointAchieved(L"pair", true);
   check(a.pump([&] { return a.synchronized.count(L"pair") != 0; }) && a.synchronized[L"pair"].empty(),
         "explicit set synchronized");

   unsigned char bogus[8] = {0, 0, 0, 0, 0, 0, 0x30, 0x39};
   FederateHandle const nobody = a.rti->decodeFederateHandle(VariableLengthData(bogus, 8));
   a.rti->registerFederationSynchronizationPoint(L"ghost", text(""), FederateHandleSet{ha, nobody});
   check(a.pump([&] { return a.registration.count(L"ghost") != 0; }) && !a.registration[L"ghost"],
         "set with a federate not joined -> registration failed");

   // A member vanishes: removed from the set.
   {
      Federate d("sync-d-" + run);
      d.rti->joinFederationExecution(L"d", L"t", fed);
      a.settle(0.2);
      a.rti->registerFederationSynchronizationPoint(L"crash", text(""));
      check(d.pump([&] { return d.announced.count(L"crash") != 0; }), "D announced");
      d.rti.reset();
   }
   for (Federate* f : {&a, &b, &c})
   {
      f->pump([&] { return f->announced.count(L"crash") != 0; });
      f->rti->synchronizationPointAchieved(L"crash", true);
   }
   check(a.pump([&] { return a.synchronized.count(L"crash") != 0; }), "synchronized without the vanished member");

   // The label can be used again once synchronized.
   a.registration.erase(L"ready");
   bool reused = false;
   for (int k = 0; k < 20 && !reused; ++k)
   {
      a.registration.erase(L"ready");
      a.rti->registerFederationSynchronizationPoint(L"ready", text(""));
      a.pump([&] { return a.registration.count(L"ready") != 0; });
      reused = a.registration[L"ready"];
      if (!reused)
         a.settle(0.1);
   }
   check(reused, "label reusable after synchronization");
   for (Federate* f : {&a, &b, &c})
   {
      f->pump([&] { return f->announced.count(L"ready") != 0; });
      try
      {
         f->rti->synchronizationPointAchieved(L"ready", true);
      }
      catch (Exception const&)
      {
      }
   }
   a.pump([&] { return a.synchronized.count(L"ready") != 0; });
   a.settle(0.3);
   for (Federate* f : {&a, &b, &c})
      f->rti->resignFederationExecution(NO_ACTION);
   a.rti->destroyFederationExecution(fed);
}

void timeTests(std::string const& run)
{
   std::wstring const fed = widen("timetest-" + run);
   Federate a("time-a-" + run), b("time-b-" + run);
   a.rti->createFederationExecution(fed, fom("base.xml"), L"HLAinteger64Time");
   a.rti->joinFederationExecution(L"a", L"t", fed);
   b.rti->joinFederationExecution(L"b", L"t", fed);
   InteractionClassHandle const urgent = a.rti->getInteractionClassHandle(L"Message.Urgent"); // TimeStamp
   InteractionClassHandle const message = a.rti->getInteractionClassHandle(L"Message");       // Receive
   ObjectClassHandle const vehicle = a.rti->getObjectClassHandle(L"Vehicle");
   AttributeHandle const position = a.rti->getAttributeHandle(vehicle, L"Position"); // TimeStamp
   a.rti->publishInteractionClass(urgent);
   a.rti->publishInteractionClass(message);
   a.rti->publishObjectClassAttributes(vehicle, AttributeHandleSet{position});
   b.rti->subscribeInteractionClass(message);
   b.rti->subscribeObjectClassAttributes(vehicle, AttributeHandleSet{position}, true, L"");
   ObjectInstanceHandle const obj = a.rti->registerObjectInstance(vehicle);

   expect<TimeRegulationIsNotEnabled>("queryLookahead while not regulating -> TimeRegulationIsNotEnabled", [&] {
      HLAinteger64Interval i;
      a.rti->queryLookahead(i);
   });
   expect<InvalidLookahead>("negative lookahead -> InvalidLookahead",
                            [&] { a.rti->enableTimeRegulation(HLAinteger64Interval(-1)); });

   b.rti->enableTimeConstrained();
   check(b.pump([&] { return b.constrainedEnabled >= 0; }) && b.constrainedEnabled == 0, "B constrained at 0");
   a.rti->enableTimeRegulation(HLAinteger64Interval(1));
   check(a.pump([&] { return a.regulationEnabled >= 0; }) && a.regulationEnabled == 0, "A regulating at 0, lookahead 1");
   expect<TimeRegulationAlreadyEnabled>("enable regulation twice -> TimeRegulationAlreadyEnabled",
                                        [&] { a.rti->enableTimeRegulation(HLAinteger64Interval(1)); });
   HLAinteger64Interval la;
   a.rti->queryLookahead(la);
   check(la.getInterval() == 1, "queryLookahead");

   a.rti->timeAdvanceRequest(HLAinteger64Time(10));
   check(a.pump([&] { return a.lastGrant() == 10; }), "A (not constrained) granted 10 at once");
   expect<InvalidLogicalTime>("float time in an integer federation -> InvalidLogicalTime",
                              [&] { a.rti->timeAdvanceRequest(HLAfloat64Time(11.0)); });
   expect<LogicalTimeAlreadyPassed>("TAR in the past -> LogicalTimeAlreadyPassed",
                                    [&] { a.rti->timeAdvanceRequest(HLAinteger64Time(5)); });
   expect<InvalidLogicalTime>("TSO send below time + lookahead -> InvalidLogicalTime",
                              [&] { a.rti->sendInteraction(urgent, ParameterHandleValueMap(), text(""), HLAinteger64Time(10)); });

   b.rti->timeAdvanceRequest(HLAinteger64Time(5));
   check(b.pump([&] { return b.lastGrant() == 5; }), "B granted 5 (GALT = 11)");
   HLAinteger64Time galt(0);
   bool const hasGalt = b.rti->queryGALT(galt);
   check(hasGalt && galt.getTime() == 11,
         "queryGALT = A's time + lookahead (" + (hasGalt ? std::to_string(galt.getTime()) : std::string("none")) + ")");

   // TSO messages (out of order) and an RO one, then B asks for 20.
   AttributeHandleValueMap pos;
   pos[position] = text("p");
   a.rti->sendInteraction(urgent, ParameterHandleValueMap(), text("u13"), HLAinteger64Time(13));
   a.rti->updateAttributeValues(obj, pos, text("p11"), HLAinteger64Time(11));
   a.rti->sendInteraction(message, ParameterHandleValueMap(), text("ro"));
   b.settle(0.3);
   check(b.events.empty(), "constrained B, not advancing: RO message held, TSO queued");
   b.rti->timeAdvanceRequest(HLAinteger64Time(20));
   check(b.pump([&] { return !b.events.empty(); }) && b.events[0].what == "i:ro",
         "RO message delivered once B is advancing");
   b.settle(0.3);
   check(b.lastGrant() == 5, "B not granted 20 while A is at 10 (GALT 11)");
   a.rti->timeAdvanceRequest(HLAinteger64Time(25));
   check(b.pump([&] { return b.lastGrant() == 20; }), "B granted 20 after A advanced to 25");
   bool order = b.events.size() == 3 && b.events[1].what == "u:p11" && b.events[1].time == 11 &&
                b.events[1].received == TIMESTAMP && b.events[2].what == "i:u13" && b.events[2].time == 13;
   check(order, "TSO messages delivered before the grant, in timestamp order");

   // Asynchronous delivery: RO messages no longer held.
   b.events.clear();
   b.rti->enableAsynchronousDelivery();
   expect<AsynchronousDeliveryAlreadyEnabled>("enable asynchronous delivery twice",
                                              [&] { b.rti->enableAsynchronousDelivery(); });
   a.rti->sendInteraction(message, ParameterHandleValueMap(), text("ro2"));
   check(b.pump([&] { return !b.events.empty(); }), "asynchronous delivery: RO message delivered while granted");

   // A late regulator starts at the time of the constrained federates.
   {
      Federate e("time-e-" + run);
      e.rti->joinFederationExecution(L"e", L"t", fed);
      e.rti->enableTimeRegulation(HLAinteger64Interval(1));
      bool const enabled = e.pump([&] { return e.regulationEnabled >= 0; });
      check(enabled && e.regulationEnabled >= 20,
            "late regulator enabled at the constrained federate's time (" + std::to_string(e.regulationEnabled) + ")");
      e.rti->resignFederationExecution(NO_ACTION);
   }
   expect<InTimeAdvancingState>("TAR while advancing -> InTimeAdvancingState", [&] {
      b.rti->timeAdvanceRequest(HLAinteger64Time(30));
      b.rti->timeAdvanceRequest(HLAinteger64Time(31));
   });
   a.rti->timeAdvanceRequest(HLAinteger64Time(40));
   check(b.pump([&] { return b.lastGrant() == 30; }), "B granted 30");
   HLAinteger64Time now(0);
   b.rti->queryLogicalTime(now);
   check(now.getTime() == 30, "queryLogicalTime");

   a.rti->deleteObjectInstance(obj, text(""));
   a.rti->resignFederationExecution(DELETE_OBJECTS);
   b.rti->resignFederationExecution(NO_ACTION);
   a.rti->destroyFederationExecution(fed);
}

// 8.21 retract, 8.22 requestRetraction, 8.23 and 8.24 order type changes.
void retractTests(std::string const& run)
{
   std::wstring const fed = widen("retracttest-" + run);
   Federate a("retract-a-" + run), b("retract-b-" + run);
   a.rti->createFederationExecution(fed, fom("base.xml"), L"HLAinteger64Time");
   a.rti->joinFederationExecution(L"a", L"t", fed);
   b.rti->joinFederationExecution(L"b", L"t", fed);
   InteractionClassHandle const urgent = a.rti->getInteractionClassHandle(L"Message.Urgent"); // TimeStamp
   InteractionClassHandle const message = a.rti->getInteractionClassHandle(L"Message");       // Receive
   InteractionClassHandle const root = a.rti->getInteractionClassHandle(L"HLAinteractionRoot");
   ObjectClassHandle const vehicle = a.rti->getObjectClassHandle(L"Vehicle");
   AttributeHandle const position = a.rti->getAttributeHandle(vehicle, L"Position"); // TimeStamp
   a.rti->publishInteractionClass(urgent);
   a.rti->publishInteractionClass(message);
   a.rti->publishObjectClassAttributes(vehicle, AttributeHandleSet{position});
   b.rti->subscribeInteractionClass(message);
   b.rti->subscribeObjectClassAttributes(vehicle, AttributeHandleSet{position}, true, L"");
   ObjectInstanceHandle const obj = a.rti->registerObjectInstance(vehicle);
   ParameterHandleValueMap const none;

   expect<TimeRegulationIsNotEnabled>("retract while not regulating -> TimeRegulationIsNotEnabled",
                                      [&] { a.rti->retract(MessageRetractionHandle()); });
   b.rti->enableTimeConstrained();
   check(b.pump([&] { return b.constrainedEnabled >= 0; }), "retract: B constrained");
   a.rti->enableTimeRegulation(HLAinteger64Interval(10));
   check(a.pump([&] { return a.regulationEnabled >= 0; }), "retract: A regulating, lookahead 10");

   // Retracted before delivery: B never sees it.
   MessageRetractionHandle const h1 = a.rti->sendInteraction(urgent, none, text("r1"), HLAinteger64Time(15));
   bool ok = true;
   try
   {
      a.rti->retract(h1);
   }
   catch (Exception const& e)
   {
      ok = false;
      std::cout << narrow(e.what()) << std::endl;
   }
   check(ok, "retract a queued TSO message");
   expect<InvalidMessageRetractionHandle>("retract twice -> InvalidMessageRetractionHandle", [&] { a.rti->retract(h1); });
   expect<InvalidMessageRetractionHandle>("retract an invalid handle -> InvalidMessageRetractionHandle",
                                          [&] { a.rti->retract(MessageRetractionHandle()); });
   MessageRetractionHandle const hro = a.rti->sendInteraction(message, none, text("ro"), HLAinteger64Time(16));
   expect<InvalidMessageRetractionHandle>("retract a message sent RO -> InvalidMessageRetractionHandle",
                                          [&] { a.rti->retract(hro); });

   // Delivered, then retracted: requestRetraction at B.
   MessageRetractionHandle const h2 = a.rti->sendInteraction(urgent, none, text("r2"), HLAinteger64Time(12));
   a.rti->timeAdvanceRequest(HLAinteger64Time(5));
   check(a.pump([&] { return a.lastGrant() == 5; }), "retract: A granted 5");
   b.rti->timeAdvanceRequest(HLAinteger64Time(13));
   check(b.pump([&] { return b.lastGrant() == 13; }), "retract: B granted 13 (GALT 15)");
   check(hasEvent(b.events, "i:r2") && !hasEvent(b.events, "i:r1"),
         "the retracted message is not delivered, the other TSO message is");
   a.rti->retract(h2);
   check(b.pump([&] { return !b.retractions.empty(); }) && b.retractions.size() == 1 && b.retractions[0] == h2,
         "retract after delivery -> requestRetraction at the receiver");

   // Too late: the sender's time has passed the timestamp.
   MessageRetractionHandle const h3 = a.rti->sendInteraction(urgent, none, text("r3"), HLAinteger64Time(20));
   a.rti->timeAdvanceRequest(HLAinteger64Time(25));
   check(a.pump([&] { return a.lastGrant() == 25; }), "retract: A granted 25");
   expect<MessageCanNoLongerBeRetracted>("retract below the logical time -> MessageCanNoLongerBeRetracted",
                                         [&] { a.rti->retract(h3); });

   // 8.24 changeInteractionOrderType: Urgent sent RO.
   b.rti->enableAsynchronousDelivery();
   expect<InteractionClassNotPublished>("changeInteractionOrderType, not published -> InteractionClassNotPublished",
                                        [&] { a.rti->changeInteractionOrderType(root, RECEIVE); });
   expect<InteractionClassNotDefined>("changeInteractionOrderType, invalid class -> InteractionClassNotDefined",
                                      [&] { a.rti->changeInteractionOrderType(InteractionClassHandle(), RECEIVE); });
   a.rti->changeInteractionOrderType(urgent, RECEIVE);
   a.rti->sendInteraction(urgent, none, text("o1"), HLAinteger64Time(50));
   bool got = b.pump([&] { return hasEvent(b.events, "i:o1"); });
   check(got && b.events.back().received == RECEIVE && b.lastGrant() == 13,
         "changeInteractionOrderType(RECEIVE): TimeStamp interaction delivered RO, before B's time reaches it");

   // 8.23 changeAttributeOrderType: Position sent RO.
   check(b.pump([&] { return b.discovered > 0; }), "retract: B discovers A's object");
   expect<ObjectInstanceNotKnown>("changeAttributeOrderType, unknown object -> ObjectInstanceNotKnown",
                                  [&] { a.rti->changeAttributeOrderType(ObjectInstanceHandle(), AttributeHandleSet{position}, RECEIVE); });
   expect<AttributeNotOwned>("changeAttributeOrderType by a non-owner -> AttributeNotOwned",
                             [&] { b.rti->changeAttributeOrderType(obj, AttributeHandleSet{position}, RECEIVE); });
   a.rti->changeAttributeOrderType(obj, AttributeHandleSet{position}, RECEIVE);
   AttributeHandleValueMap pos;
   pos[position] = text("p");
   a.rti->updateAttributeValues(obj, pos, text("p50"), HLAinteger64Time(50));
   got = b.pump([&] { return hasEvent(b.events, "u:p50"); });
   check(got && b.events.back().received == RECEIVE && b.lastGrant() == 13,
         "changeAttributeOrderType(RECEIVE): TimeStamp attribute reflected RO");

   a.rti->resignFederationExecution(DELETE_OBJECTS);
   b.rti->resignFederationExecution(NO_ACTION);
   a.rti->destroyFederationExecution(fed);
}

// Two federates, both regulating and constrained, exchanging TSO messages.
void loopTest(std::string const& run, std::int64_t lookahead, bool useNer, int steps)
{
   std::wstring const fed = widen("looptest-" + std::to_string(lookahead) + "-" + run);
   Federate p("loop-p-" + run), q("loop-q-" + run);
   p.rti->createFederationExecution(fed, fom("base.xml"), L"HLAinteger64Time");
   p.rti->joinFederationExecution(L"p", L"t", fed);
   q.rti->joinFederationExecution(L"q", L"t", fed);
   InteractionClassHandle const urgent = p.rti->getInteractionClassHandle(L"Message.Urgent");
   for (Federate* f : {&p, &q})
   {
      f->rti->publishInteractionClass(urgent);
      f->rti->subscribeInteractionClass(urgent);
      f->rti->enableTimeConstrained();
      f->pump([&] { return f->constrainedEnabled >= 0; });
   }
   for (Federate* f : {&p, &q})
   {
      f->rti->enableTimeRegulation(HLAinteger64Interval(lookahead));
      f->pump([&] { return f->regulationEnabled >= 0; });
   }
   check(p.regulationEnabled == 0 && q.regulationEnabled == 0, "loop: both regulating and constrained");

   auto t0 = std::chrono::steady_clock::now();
   bool ok = true;
   std::int64_t now = 0;
   for (int k = 1; k <= steps && ok; ++k)
   {
      std::int64_t const target = now + (lookahead > 0 ? lookahead : 1);
      // Each sends one TSO message at the earliest allowed time, then advances.
      std::int64_t const sendAt = lookahead > 0 ? now + lookahead : now + 1;
      p.rti->sendInteraction(urgent, ParameterHandleValueMap(), text("p" + std::to_string(k)), HLAinteger64Time(sendAt));
      q.rti->sendInteraction(urgent, ParameterHandleValueMap(), text("q" + std::to_string(k)), HLAinteger64Time(sendAt));
      if (useNer)
      {
         p.rti->nextMessageRequest(HLAinteger64Time(target));
         q.rti->nextMessageRequest(HLAinteger64Time(target));
      }
      else
      {
         p.rti->timeAdvanceRequest(HLAinteger64Time(target));
         q.rti->timeAdvanceRequest(HLAinteger64Time(target));
      }
      std::size_t const pg = p.grants.size(), qg = q.grants.size();
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while ((p.grants.size() == pg || q.grants.size() == qg) && std::chrono::steady_clock::now() < deadline)
      {
         p.rti->evokeMultipleCallbacks(0.0, 0.01);
         q.rti->evokeMultipleCallbacks(0.0, 0.01);
      }
      ok = p.grants.size() > pg && q.grants.size() > qg && p.lastGrant() == target && q.lastGrant() == target;
      now = target;
   }
   double const secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
   bool received = true;
   for (int k = 1; k <= steps; ++k)
   {
      bool pk = false, qk = false;
      for (Event const& e : q.events)
         pk = pk || (e.what == "i:p" + std::to_string(k) && e.received == TIMESTAMP);
      for (Event const& e : p.events)
         qk = qk || (e.what == "i:q" + std::to_string(k) && e.received == TIMESTAMP);
      received = received && pk && qk;
   }
   std::string const kind = std::string(useNer ? "NER" : "TAR") + ", lookahead " + std::to_string(lookahead);
   check(ok, "loop (" + kind + "): " + std::to_string(steps) + " steps granted (" +
                std::to_string(int(steps / secs)) + " steps/s)");
   check(received, "loop (" + kind + "): every TSO message received with receivedOrder TIMESTAMP");
   if (useNer)
   {
      // No message at all: only the consistent-cut rule can grant (zero lookahead).
      p.rti->nextMessageRequest(HLAinteger64Time(now + 100));
      q.rti->nextMessageRequest(HLAinteger64Time(now + 100));
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while ((p.lastGrant() != now + 100 || q.lastGrant() != now + 100) && std::chrono::steady_clock::now() < deadline)
      {
         p.rti->evokeMultipleCallbacks(0.0, 0.01);
         q.rti->evokeMultipleCallbacks(0.0, 0.01);
      }
      check(p.lastGrant() == now + 100 && q.lastGrant() == now + 100,
            "zero lookahead, NER without messages: both granted (consistent cut)");
   }
   for (Federate* f : {&p, &q})
      f->rti->resignFederationExecution(NO_ACTION);
   p.rti->destroyFederationExecution(fed);
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

   try
   {
      syncTests(run);
      timeTests(run);
      retractTests(run);
      loopTest(run, 1, false, 50);
      loopTest(run, 0, true, 50);
   }
   catch (Exception const& e)
   {
      check(false, "unexpected exception: " + narrow(e.what()));
   }
   std::cout << (failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED") << " (" << checks << " checks, "
             << failures << " failure(s))" << std::endl;
   return failures == 0 ? 0 : 1;
}
