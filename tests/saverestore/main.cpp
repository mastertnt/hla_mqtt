// hla-saverestore-test: federation save and restore (IEEE 1516.1-2010,
// 4.16 to 4.32) between ambassadors in one process: save and restore with
// their callbacks and status queries, services refused while saving or
// restoring, the saved RTI state (objects, ownership, declarations, time),
// failures reported by a federate, aborts, a timed save, a federate resigning
// and a federate lost during a save. Needs a broker.
//
//   hla-saverestore-test [--broker mqtt://host:1883]
//
// Standard API only. Exit code 0 = all checks passed.

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS // std::getenv
#endif

#include <RTI/NullFederateAmbassador.h>
#include <RTI/RTI1516.h>
#include <RTI/time/HLAfloat64Time.h>

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

struct Federate : NullFederateAmbassador
{
   std::unique_ptr<RTIambassador> rti;
   std::map<ObjectInstanceHandle, ObjectClassHandle> discovered;
   std::map<ObjectInstanceHandle, int> reflections;
   std::vector<std::string> events; // save and restore callbacks, in order
   std::wstring saveLabel, restoreLabel, restoreName;
   FederateHandle restoreHandle;
   double saveTime = -1;
   bool timedSave = false;
   int notSavedReason = -1, notRestoredReason = -1;
   FederateHandleSaveStatusPairVector saveStatus;
   FederateRestoreStatusVector restoreStatus;
   bool saveStatusIn = false, restoreStatusIn = false;
   bool constrained = false;
   double granted = -1;

   explicit Federate(std::string const& clientId)
   {
      rti = RTIambassadorFactory().createRTIambassador();
      rti->connect(*this, HLA_EVOKED, broker + L";clientId=" + widen(clientId));
   }
   bool has(std::string const& e, size_t from = 0) const
   {
      for (size_t i = from; i < events.size(); ++i)
         if (events[i] == e)
            return true;
      return false;
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
   void timeConstrainedEnabled(LogicalTime const&) RTI_THROW((FederateInternalError)) override
   {
      constrained = true;
   }
   void timeAdvanceGrant(LogicalTime const& t) RTI_THROW((FederateInternalError)) override
   {
      granted = dynamic_cast<HLAfloat64Time const&>(t).getTime();
   }

   // ------------------------------------------------------------- save
   void initiateFederateSave(std::wstring const& label) RTI_THROW((FederateInternalError)) override
   {
      saveLabel = label;
      timedSave = false;
      events.push_back("initiateSave");
   }
   void initiateFederateSave(std::wstring const& label, LogicalTime const& t) RTI_THROW((FederateInternalError)) override
   {
      saveLabel = label;
      timedSave = true;
      saveTime = dynamic_cast<HLAfloat64Time const&>(t).getTime();
      events.push_back("initiateSave");
   }
   void federationSaved() RTI_THROW((FederateInternalError)) override
   {
      events.push_back("saved");
   }
   void federationNotSaved(SaveFailureReason r) RTI_THROW((FederateInternalError)) override
   {
      notSavedReason = r;
      events.push_back("notSaved");
   }
   void federationSaveStatusResponse(FederateHandleSaveStatusPairVector const& v)
      RTI_THROW((FederateInternalError)) override
   {
      saveStatus = v;
      saveStatusIn = true;
   }

   // ---------------------------------------------------------- restore
   void requestFederationRestoreSucceeded(std::wstring const& label) RTI_THROW((FederateInternalError)) override
   {
      restoreLabel = label;
      events.push_back("restoreSucceeded");
   }
   void requestFederationRestoreFailed(std::wstring const& label) RTI_THROW((FederateInternalError)) override
   {
      restoreLabel = label;
      events.push_back("restoreFailed");
   }
   void federationRestoreBegun() RTI_THROW((FederateInternalError)) override
   {
      events.push_back("restoreBegun");
   }
   void initiateFederateRestore(std::wstring const& label, std::wstring const& name, FederateHandle h)
      RTI_THROW((FederateInternalError)) override
   {
      restoreLabel = label;
      restoreName = name;
      restoreHandle = h;
      events.push_back("initiateRestore");
   }
   void federationRestored() RTI_THROW((FederateInternalError)) override
   {
      events.push_back("restored");
   }
   void federationNotRestored(RestoreFailureReason r) RTI_THROW((FederateInternalError)) override
   {
      notRestoredReason = r;
      events.push_back("notRestored");
   }
   void federationRestoreStatusResponse(FederateRestoreStatusVector const& v) RTI_THROW((FederateInternalError)) override
   {
      restoreStatus = v;
      restoreStatusIn = true;
   }
};

bool pump(std::vector<Federate*> const& feds, std::function<bool()> done, double seconds = 5.0)
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

bool allHave(std::vector<Federate*> const& feds, std::string const& e, std::vector<size_t> const& from)
{
   for (size_t i = 0; i < feds.size(); ++i)
      if (!feds[i]->has(e, from[i]))
         return false;
   return true;
}

std::vector<size_t> marks(std::vector<Federate*> const& feds)
{
   std::vector<size_t> m;
   for (Federate* f : feds)
      m.push_back(f->events.size());
   return m;
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

// Saves `label` with every federate of `feds` completing (or `failing` not completing).
bool saveAll(std::vector<Federate*> const& feds, Federate& requester, std::wstring const& label,
             Federate* failing = nullptr)
{
   std::vector<size_t> const m = marks(feds);
   requester.rti->requestFederationSave(label);
   if (!pump(feds, [&] { return allHave(feds, "initiateSave", m); }))
      return false;
   for (Federate* f : feds)
   {
      f->rti->federateSaveBegun();
      if (f == failing)
         f->rti->federateSaveNotComplete();
      else
         f->rti->federateSaveComplete();
   }
   return pump(feds, [&] { return allHave(feds, failing ? "notSaved" : "saved", m); });
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
   std::wstring const fed = widen("srtest-" + run);
   std::cout << "federation: " << narrow(fed) << std::endl;

   try
   {
      Federate a("srtest-a-" + run), b("srtest-b-" + run);
      std::vector<Federate*> const ab{&a, &b};
      a.rti->createFederationExecution(fed, widen(std::string(HLA_TEST_FOM_DIR) + "/base.xml"));
      FederateHandle const ha = a.rti->joinFederationExecution(L"a", L"t", fed);
      FederateHandle const hb = b.rti->joinFederationExecution(L"b", L"t", fed);
      RTIambassador& ra = *a.rti;
      RTIambassador& rb = *b.rti;

      ObjectClassHandle const vehicle = ra.getObjectClassHandle(L"Vehicle");
      AttributeHandle const pos = ra.getAttributeHandle(vehicle, L"Position");
      AttributeHandle const name = ra.getAttributeHandle(vehicle, L"Name");
      ra.publishObjectClassAttributes(vehicle, AttributeHandleSet{pos, name});
      rb.subscribeObjectClassAttributes(vehicle, AttributeHandleSet{pos, name});
      ObjectInstanceHandle const v = ra.registerObjectInstance(vehicle);
      check(pump(ab, [&] { return b.discovered.count(v) != 0; }), "B discovers A's vehicle");

      // ------------------------------------------------ errors, no save
      check(throws<SaveNotInitiated>([&] { rb.federateSaveBegun(); }), "federateSaveBegun without save -> SaveNotInitiated");
      check(throws<FederateHasNotBegunSave>([&] { rb.federateSaveComplete(); }),
            "federateSaveComplete without save -> FederateHasNotBegunSave");
      check(throws<SaveNotInProgress>([&] { ra.abortFederationSave(); }), "abortFederationSave -> SaveNotInProgress");
      check(throws<RestoreNotInProgress>([&] { ra.abortFederationRestore(); }),
            "abortFederationRestore -> RestoreNotInProgress");
      check(throws<RestoreNotRequested>([&] { ra.federateRestoreComplete(); }),
            "federateRestoreComplete without restore -> RestoreNotRequested");
      a.saveStatusIn = false;
      ra.queryFederationSaveStatus();
      check(pump(ab, [&] { return a.saveStatusIn; }) && a.saveStatus.size() == 2 &&
               a.saveStatus[0].second == NO_SAVE_IN_PROGRESS && a.saveStatus[1].second == NO_SAVE_IN_PROGRESS,
            "queryFederationSaveStatus: no save in progress for A and B");

      // ------------------------------------------------------- #28 save
      {
         std::vector<size_t> const m = marks(ab);
         ra.requestFederationSave(L"s1");
         check(pump(ab, [&] { return allHave(ab, "initiateSave", m); }), "initiateFederateSave at A and B");
         check(a.saveLabel == L"s1" && b.saveLabel == L"s1" && !a.timedSave, "label s1, untimed");
         check(throws<SaveInProgress>([&] { ra.requestFederationSave(L"other"); }),
               "requestFederationSave during a save -> SaveInProgress");
         check(throws<SaveInProgress>([&] { rb.subscribeObjectClassAttributes(vehicle, AttributeHandleSet{pos}); }),
               "service refused while saving -> SaveInProgress");
         check(throws<FederateHasNotBegunSave>([&] { rb.federateSaveComplete(); }),
               "federateSaveComplete before federateSaveBegun -> FederateHasNotBegunSave");
         rb.federateSaveBegun();
         b.saveStatusIn = false;
         rb.queryFederationSaveStatus(); // #29
         bool okStatus = pump(ab, [&] { return b.saveStatusIn; });
         SaveStatus sa = NO_SAVE_IN_PROGRESS, sb = NO_SAVE_IN_PROGRESS;
         for (auto const& p : b.saveStatus)
            (p.first == ha ? sa : sb) = p.second;
         check(okStatus && sa == FEDERATE_INSTRUCTED_TO_SAVE && sb == FEDERATE_SAVING,
               "queryFederationSaveStatus: A instructed, B saving");
         rb.federateSaveComplete();
         ra.federateSaveBegun();
         settleAll(ab, 0.2);
         check(!a.has("saved", m[0]) && !b.has("saved", m[1]), "not saved while A has not completed");
         ra.federateSaveComplete();
         check(pump(ab, [&] { return allHave(ab, "saved", m); }), "federationSaved at A and B");
         rb.subscribeObjectClassAttributes(vehicle, AttributeHandleSet{pos, name}); // allowed again
         check(true, "services allowed again after the save");
      }

      // State changed after the save: a second object, A no longer owns Position of v.
      ObjectInstanceHandle const w = ra.registerObjectInstance(vehicle);
      check(pump(ab, [&] { return b.discovered.count(w) != 0; }), "B discovers a second vehicle (after the save)");
      ra.unconditionalAttributeOwnershipDivestiture(v, AttributeHandleSet{pos});
      check(!ra.isAttributeOwnedByFederate(v, pos), "A divests Position of v (after the save)");

      // ---------------------------------------------------- #30 restore
      {
         std::vector<size_t> const m = marks(ab);
         ra.requestFederationRestore(L"unknown");
         check(pump(ab, [&] { return a.has("restoreFailed", m[0]); }) && a.restoreLabel == L"unknown",
               "requestFederationRestore(unknown label) -> requestFederationRestoreFailed");
      }
      {
         std::vector<size_t> const m = marks(ab);
         ra.requestFederationRestore(L"s1");
         check(pump(ab, [&] { return a.has("restoreSucceeded", m[0]); }), "requestFederationRestoreSucceeded at A");
         check(pump(ab, [&] { return allHave(ab, "restoreBegun", m); }), "federationRestoreBegun at A and B");
         check(pump(ab, [&] { return allHave(ab, "initiateRestore", m); }), "initiateFederateRestore at A and B");
         check(a.restoreLabel == L"s1" && a.restoreName == L"a" && a.restoreHandle == ha && b.restoreName == L"b" &&
                  b.restoreHandle == hb,
               "initiateFederateRestore: label, saved name and handle");
         check(throws<RestoreInProgress>([&] { ra.updateAttributeValues(v, AttributeHandleValueMap(), text("")); }),
               "service refused while restoring -> RestoreInProgress");
         check(throws<RestoreInProgress>([&] { ra.requestFederationSave(L"x"); }),
               "requestFederationSave while restoring -> RestoreInProgress");
         b.restoreStatusIn = false;
         rb.queryFederationRestoreStatus(); // #31
         bool okStatus = pump(ab, [&] { return b.restoreStatusIn; });
         RestoreStatus st = NO_RESTORE_IN_PROGRESS;
         for (auto const& s : b.restoreStatus)
            if (s.preRestoreHandle == hb)
               st = s.status;
         check(okStatus && b.restoreStatus.size() == 2 && st == FEDERATE_RESTORING,
               "queryFederationRestoreStatus: B restoring");
         ra.federateRestoreComplete();
         settleAll(ab, 0.2);
         check(!a.has("restored", m[0]), "not restored while B has not completed");
         rb.federateRestoreComplete();
         check(pump(ab, [&] { return allHave(ab, "restored", m); }), "federationRestored at A and B");
         check(a.events.size() >= m[0] + 4 && a.events[m[0]] == "restoreSucceeded" && a.events[m[0] + 1] == "restoreBegun" &&
                  a.events[m[0] + 2] == "initiateRestore" && a.events[m[0] + 3] == "restored",
               "callback order at A: succeeded, begun, initiate, restored");
      }

      // ------------------------------------------------ #32 restored state
      check(ra.isAttributeOwnedByFederate(v, pos), "A owns Position of v again (saved ownership)");
      check(throws<ObjectInstanceNotKnown>([&] { ra.isAttributeOwnedByFederate(w, pos); }),
            "the vehicle registered after the save is not known to A");
      {
         int const before = b.reflections[v];
         AttributeHandleValueMap values;
         values[pos] = text("p1");
         ra.updateAttributeValues(v, values, text("")); // saved publication, ownership and subscription
         check(pump(ab, [&] { return b.reflections[v] > before; }), "B reflects A's update of v (saved declarations)");
      }
      check(throws<ObjectInstanceNotKnown>([&] { rb.isAttributeOwnedByFederate(w, pos); }),
            "the second vehicle is not known to B either");

      // ------------------------------------------ failure, abort (save)
      {
         check(saveAll(ab, a, L"s2", &b) && a.notSavedReason == FEDERATE_REPORTED_FAILURE_DURING_SAVE &&
                  b.notSavedReason == FEDERATE_REPORTED_FAILURE_DURING_SAVE,
               "federateSaveNotComplete -> federationNotSaved(FEDERATE_REPORTED_FAILURE_DURING_SAVE)");
         std::vector<size_t> const m = marks(ab);
         ra.requestFederationRestore(L"s2");
         check(pump(ab, [&] { return a.has("restoreFailed", m[0]); }), "a failed save cannot be restored");
      }
      {
         std::vector<size_t> const m = marks(ab);
         rb.requestFederationSave(L"s3");
         check(pump(ab, [&] { return allHave(ab, "initiateSave", m); }), "save s3 initiated");
         rb.federateSaveBegun();
         ra.abortFederationSave();
         check(pump(ab, [&] { return allHave(ab, "notSaved", m); }) && a.notSavedReason == SAVE_ABORTED &&
                  b.notSavedReason == SAVE_ABORTED,
               "abortFederationSave -> federationNotSaved(SAVE_ABORTED)");
         check(throws<SaveNotInitiated>([&] { ra.federateSaveBegun(); }), "aborted save: federateSaveBegun refused");
      }

      // --------------------------------------- failure, abort (restore)
      {
         std::vector<size_t> const m = marks(ab);
         ra.requestFederationRestore(L"s1");
         check(pump(ab, [&] { return allHave(ab, "initiateRestore", m); }), "restore s1 initiated again");
         ra.federateRestoreComplete();
         rb.federateRestoreNotComplete();
         check(pump(ab, [&] { return allHave(ab, "notRestored", m); }) &&
                  a.notRestoredReason == FEDERATE_REPORTED_FAILURE_DURING_RESTORE,
               "federateRestoreNotComplete -> federationNotRestored(FEDERATE_REPORTED_FAILURE_DURING_RESTORE)");
      }
      {
         std::vector<size_t> const m = marks(ab);
         ra.requestFederationRestore(L"s1");
         check(pump(ab, [&] { return allHave(ab, "initiateRestore", m); }), "restore s1 initiated a third time");
         rb.abortFederationRestore();
         check(pump(ab, [&] { return allHave(ab, "notRestored", m); }) && a.notRestoredReason == RESTORE_ABORTED &&
                  b.notRestoredReason == RESTORE_ABORTED,
               "abortFederationRestore -> federationNotRestored(RESTORE_ABORTED)");
         rb.subscribeObjectClassAttributes(vehicle, AttributeHandleSet{pos, name});
         check(true, "services allowed again after the aborted restore");
      }

      // ------------------------------------------------------ timed save
      {
         rb.enableTimeConstrained();
         check(pump(ab, [&] { return b.constrained; }), "B time-constrained");
         std::vector<size_t> const m = marks(ab);
         ra.requestFederationSave(L"s4", HLAfloat64Time(10.0));
         check(pump(ab, [&] { return a.has("initiateSave", m[0]); }) && a.timedSave && a.saveTime == 10.0,
               "timed save: A (unconstrained) instructed at once, with the save time");
         settleAll(ab, 0.3);
         check(!b.has("initiateSave", m[1]), "constrained B not instructed before reaching the save time");
         rb.timeAdvanceRequest(HLAfloat64Time(10.0));
         check(pump(ab, [&] { return b.granted == 10.0 && b.has("initiateSave", m[1]); }) && b.saveTime == 10.0,
               "B instructed once granted at the save time");
         for (Federate* f : ab)
         {
            f->rti->federateSaveBegun();
            f->rti->federateSaveComplete();
         }
         check(pump(ab, [&] { return allHave(ab, "saved", m); }), "timed save s4 saved");
         rb.timeAdvanceRequest(HLAfloat64Time(20.0));
         check(pump(ab, [&] { return b.granted == 20.0; }), "B advances to 20 after the save");
         std::vector<size_t> const r = marks(ab);
         ra.requestFederationRestore(L"s4");
         check(pump(ab, [&] { return allHave(ab, "initiateRestore", r); }), "restore s4 initiated");
         for (Federate* f : ab)
            f->rti->federateRestoreComplete();
         check(pump(ab, [&] { return allHave(ab, "restored", r); }), "s4 restored");
         std::unique_ptr<LogicalTime> t = rb.getTimeFactory()->makeInitial();
         rb.queryLogicalTime(*t);
         check(dynamic_cast<HLAfloat64Time const&>(*t).getTime() == 10.0, "B's logical time restored to 10");
         check(throws<TimeConstrainedAlreadyEnabled>([&] { rb.enableTimeConstrained(); }),
               "B still time-constrained after the restore");
         rb.timeAdvanceRequest(HLAfloat64Time(12.0));
         check(pump(ab, [&] { return b.granted == 12.0; }), "B advances from the restored time");
         rb.disableTimeConstrained();
      }

      // ------------------------------- members resigning or lost (#33)
      {
         std::unique_ptr<Federate> c(new Federate("srtest-c-" + run));
         c->rti->joinFederationExecution(L"c", L"t", fed);
         std::vector<Federate*> const abc{&a, &b, c.get()};
         std::vector<size_t> const m = marks(abc);
         ra.requestFederationSave(L"s5");
         check(pump(abc, [&] { return allHave(abc, "initiateSave", m); }), "save s5 initiated at A, B and C");
         c->rti->resignFederationExecution(NO_ACTION);
         check(pump(ab, [&] { return allHave(ab, "notSaved", m); }) && a.notSavedReason == FEDERATE_RESIGNED_DURING_SAVE &&
                  b.notSavedReason == FEDERATE_RESIGNED_DURING_SAVE,
               "member resigned -> federationNotSaved(FEDERATE_RESIGNED_DURING_SAVE)");
      }
      {
         std::unique_ptr<Federate> d(new Federate("srtest-d-" + run));
         d->rti->joinFederationExecution(L"d", L"t", fed);
         std::vector<Federate*> const abd{&a, &b, d.get()};
         std::vector<size_t> const m = marks(abd);
         rb.requestFederationSave(L"s6");
         check(pump(abd, [&] { return allHave(abd, "initiateSave", m); }), "save s6 initiated at A, B and D");
         d.reset(); // lost without resigning
         check(pump(ab, [&] { return allHave(ab, "notSaved", m); }, 10.0) &&
                  a.notSavedReason == RTI_DETECTED_FAILURE_DURING_SAVE,
               "member lost -> federationNotSaved(RTI_DETECTED_FAILURE_DURING_SAVE)");
      }
      {
         std::unique_ptr<Federate> e(new Federate("srtest-e-" + run));
         e->rti->joinFederationExecution(L"e", L"t", fed);
         std::vector<Federate*> const abe{&a, &b, e.get()};
         settleAll(abe, 0.3);
         std::vector<size_t> const m = marks(abe);
         ra.requestFederationRestore(L"s1"); // three federates, two saved
         check(pump(abe, [&] { return a.has("restoreFailed", m[0]); }),
               "restore with a different set of federates -> requestFederationRestoreFailed");
         e->rti->resignFederationExecution(NO_ACTION);
      }

      // A save of the remaining federates still works.
      check(saveAll(ab, b, L"s7"), "save s7 after the failures");

      // ----------------- restore into another federate: saved identity taken
      rb.resignFederationExecution(CANCEL_THEN_DELETE_THEN_DIVEST);
      {
         Federate b2("srtest-b2-" + run);
         FederateHandle const hb2 = b2.rti->joinFederationExecution(L"b2", L"t", fed);
         std::vector<Federate*> const ab2{&a, &b2};
         settleAll(ab2, 0.3);
         check(hb2 != hb, "the new federate has another handle");
         std::vector<size_t> const m = marks(ab2);
         ra.requestFederationRestore(L"s7");
         check(pump(ab2, [&] { return allHave(ab2, "initiateRestore", m); }), "restore s7 initiated at A and B2");
         check(b2.restoreName == L"b" && b2.restoreHandle == hb, "B2 takes B's saved name and handle");
         b2.restoreStatusIn = false;
         b2.rti->queryFederationRestoreStatus();
         bool mapped = false;
         if (pump(ab2, [&] { return b2.restoreStatusIn; }))
            for (auto const& s : b2.restoreStatus)
               mapped = mapped || (s.preRestoreHandle == hb2 && s.postRestoreHandle == hb);
         check(mapped, "queryFederationRestoreStatus: pre-restore B2, post-restore B");
         for (Federate* f : ab2)
            f->rti->federateRestoreComplete();
         check(pump(ab2, [&] { return allHave(ab2, "restored", m); }), "s7 restored into A and B2");
         AttributeHandleValueMap values;
         values[name] = text("n");
         ra.updateAttributeValues(v, values, text(""));
         check(pump(ab2, [&] { return b2.reflections[v] > 0; }), "B2 reflects v (B's saved subscription)");
         a.saveStatusIn = false;
         ra.queryFederationSaveStatus();
         bool listed = false;
         if (pump(ab2, [&] { return a.saveStatusIn; }))
            for (auto const& p : a.saveStatus)
               listed = listed || p.first == hb;
         check(listed && a.saveStatus.size() == 2, "the federation lists B2 under B's handle");
         check(saveAll(ab2, b2, L"s8"), "save after the restore, requested by B2");
         b2.rti->resignFederationExecution(CANCEL_THEN_DELETE_THEN_DIVEST);
      }
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
