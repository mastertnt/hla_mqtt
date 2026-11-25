// Federation save and restore (IEEE 1516.1-2010, 4.16 to 4.32) without a
// central RTI.
//
// Topics under the joined federation F (label percent-encoded):
//   F/save/claims/<client>        claim of the requester (transient, as the sync labels)
//   F/save/state                  retained {"id","label","time"?,"requester","members":[{"h","c"}],"aborted"?}
//   F/save/status/<federate>      retained {"id","status":"begun"|"complete"|"notcomplete"}
//   F/saves/<label>/<id>/<fed>    retained {"name","type","handle","state":{...}}: saved RTI state of a federate
//   F/saves/<label>/info          retained {"id","federates":[{"h","n","t"}],"time"?}: the last successful save
//   F/restore/claims/<client>     claim of the requester (transient)
//   F/restore/state               retained {"id","label","save","requester","members":[{"h","c","s","n"}],"aborted"?}
//   F/restore/status/<federate>   retained {"id","status":"cleared"|"complete"|"notcomplete"|"failed"|"resigned"}
//
// Save: the members are the federates joined at the request. Each one is
// instructed (at once, or at the save time once time-constrained and granted
// there), writes its RTI state with federateSaveComplete, and every member
// decides the result by itself: all complete -> federationSaved; a member
// reported failure, resigned or vanished -> federationNotSaved. The requester
// (or the smallest live member) then publishes the info record of the label
// and clears the save records.
//
// Restore: the requester checks the saved federates against the joined ones
// (same types; a federate keeps its own saved identity when it was saved,
// the others take the remaining ones of their type) and publishes the
// mapping. Each member clears the retained records it wrote (owner records,
// info records, declarations, time state, names, federate record if its
// handle changes), then reports "cleared". Once every member has cleared,
// each one loads its saved state, takes its saved handle and name, and
// publishes its records again: no record cleared by one federate can reach
// the others after a record published from a saved state. Then
// initiateFederateRestore; federationRestored when every member is complete.
// While restoring, the object, ownership, time and advisory reactions are
// suspended (the traffic of the other members is their own restoration).
//
// Saved state of a federate (#32): declarations (publications, subscriptions
// with and without regions, rates, order and transportation changes), regions,
// object instances known or registered (class, name, owned attributes, update
// regions, order, transportation, advisories notified), reserved names,
// advisory switches, time state (logical time, lookahead, regulation,
// constraint, asynchronous delivery). Not saved: pending ownership
// operations, queued messages, synchronization points, MOM objects.

#include "MqttRTIambassador.h"

#include "Fdd.h"
#include "HandleFriends.h"
#include "MqttTransport.h"
#include "Utf.h"
#include "Wire.h"

#include <algorithm>
#include <cstdlib>

namespace hla_mqtt
{
namespace
{
bool startsWith(std::string const& s, std::string const& prefix)
{
   return s.compare(0, prefix.size(), prefix) == 0;
}

std::uint64_t parseHex(std::string const& s)
{
   return std::strtoull(s.c_str(), nullptr, 16);
}

Json hexList(std::set<std::uint64_t> const& values)
{
   Json a = Json::array();
   for (std::uint64_t v : values)
      a.push(hex64(v));
   return a;
}

std::set<std::uint64_t> hexSet(Json const& j)
{
   std::set<std::uint64_t> out;
   for (std::string const& s : j.strings())
      out.insert(parseHex(s));
   return out;
}

Json hexSetMap(std::map<std::uint64_t, std::set<std::uint64_t>> const& m)
{
   Json j = Json::object();
   for (auto const& kv : m)
      j.set(hex64(kv.first), hexList(kv.second));
   return j;
}

std::map<std::uint64_t, std::set<std::uint64_t>> hexSetMapOf(Json const& j)
{
   std::map<std::uint64_t, std::set<std::uint64_t>> out;
   for (auto const& kv : j.members())
      out[parseHex(kv.first)] = hexSet(kv.second);
   return out;
}

Json extentsList(std::vector<Extents> const& list)
{
   Json a = Json::array();
   for (Extents const& e : list)
      a.push(extentsToJson(e));
   return a;
}

int const kExclusiveAttempts = 8;
} // namespace

// ================================================================ helpers
void MqttRTIambassador::checkNotSavingOrRestoring() const
{
   if (_saveBusy)
      throw SaveInProgress(L"federation save in progress");
   if (_restoreBusy)
      throw RestoreInProgress(L"federation restore in progress");
}

bool MqttRTIambassador::momClassObject(ObjectInstance const& o) const
{
   if (o.mom)
      return true;
   Fdd::ObjectClass const* c = _fdd ? _fdd->objectClass(o.objectClass) : nullptr;
   return c && startsWith(c->qualified, "HLAobjectRoot.HLAmanager");
}

// Claim + barrier, as the synchronization labels: true when this federate may
// publish the state record (then withdraws its claim).
bool MqttRTIambassador::claimExclusive(std::string const& base, std::string const& stateTopic)
{
   std::string const myId = _transport->settings().clientId;
   std::string const claimTopic = base + "/claims/" + encodeTopicLevel(myId);
   auto stateExists = [&] {
      std::lock_guard<std::mutex> v(_viewMutex);
      return _view.count(stateTopic) != 0;
   };
   auto contended = [&] {
      std::lock_guard<std::mutex> v(_viewMutex);
      for (auto const& kv : recordsUnder(base + "/claims/"))
      {
         if (kv.first == claimTopic)
            continue;
         try
         {
            if (clientAlive(Json::parse(kv.second)["clientId"].str()))
               return true;
         }
         catch (std::exception const&)
         {
         }
      }
      return _view.count(stateTopic) != 0;
   };
   for (int attempt = 0; attempt < kExclusiveAttempts; ++attempt)
   {
      _transport->barrier();
      if (stateExists())
         return false;
      Json claim = Json::object();
      claim.set("clientId", myId);
      _transport->publish(claimTopic, claim.dump(), 1, true);
      _transport->barrier();
      if (!contended())
         return true;
      _transport->publish(claimTopic, std::string(), 1, true);
      if (stateExists())
         return false;
      backoff(attempt);
   }
   return false;
}

// ============================================================ saved state
Json MqttRTIambassador::federateSnapshot() const
{
   Json s = Json::object();
   s.set("pub", hexSetMap(_publishedAttributes));
   s.set("pubI", hexList(_publishedInteractions));
   s.set("sub", hexSetMap(_plainSubscribedAttributes));
   Json subR = Json::object();
   for (auto const& cls : _regionSubscribedAttributes)
      subR.set(hex64(cls.first), hexSetMap(cls.second));
   s.set("subR", subR);
   s.set("subI", hexList(_plainSubscribedInteractions));
   s.set("subIR", hexSetMap(_regionSubscribedInteractions));
   Json rates = Json::object();
   for (auto const& cls : _subscribedRates)
   {
      Json r = Json::object();
      for (auto const& a : cls.second)
         r.set(hex64(a.first), std::to_string(a.second));
      rates.set(hex64(cls.first), r);
   }
   s.set("rates", rates);
   Json iorder = Json::object();
   for (auto const& kv : _interactionOrder)
      iorder.set(hex64(kv.first), kv.second == TIMESTAMP ? "ts" : "ro");
   s.set("iorder", iorder);
   Json itransport = Json::object();
   for (auto const& kv : _interactionTransport)
      itransport.set(hex64(kv.first), kv.second);
   s.set("itransport", itransport);

   Json regions = Json::object();
   for (auto const& kv : _regions)
   {
      Json r = Json::object();
      r.set("p", extentsToJson(kv.second.pending));
      r.set("c", extentsToJson(kv.second.committed));
      regions.set(hex64(kv.first), r);
   }
   s.set("regions", regions);
   s.set("regionCounter", std::to_string(_regionCounter.load()));

   Json names = Json::array();
   for (std::wstring const& n : _reservedNames)
      names.push(toUtf8(n));
   s.set("names", names);
   Json switches = Json::array();
   for (auto const& sw : _advisorySwitches)
      switches.push(sw.load());
   s.set("switches", switches);

   Json t = Json::object();
   t.set("reg", _time.regulating);
   t.set("con", _time.constrained);
   t.set("async", _time.asyncDelivery);
   t.set("t", _time.time.str());
   t.set("la", _time.lookahead.str());
   s.set("time", t);

   Json objects = Json::array();
   for (auto const& kv : _objects)
   {
      ObjectInstance const& o = kv.second;
      if (momClassObject(o))
         continue;
      Json j = Json::object();
      j.set("h", hex64(o.handle));
      j.set("cls", hex64(o.objectClass));
      j.set("name", o.name);
      j.set("topic", o.topic);
      j.set("known", hex64(o.knownClass));
      j.set("reg", o.registered);
      j.set("registrant", hex64(o.registrant));
      j.set("ldel", o.locallyDeleted);
      j.set("owned", hexList(o.owned));
      j.set("relevant", hexList(o.relevant));
      j.set("scope", hexList(o.scope));
      j.set("uregions", hexSetMap(o.updateRegions));
      Json order = Json::object();
      for (auto const& a : o.order)
         order.set(hex64(a.first), a.second == TIMESTAMP ? "ts" : "ro");
      j.set("order", order);
      Json transport = Json::object();
      for (auto const& a : o.transport)
         transport.set(hex64(a.first), a.second);
      j.set("transport", transport);
      objects.push(j);
   }
   s.set("objects", objects);
   return s;
}

void MqttRTIambassador::loadSnapshot(Json const& s)
{
   _publishedAttributes = hexSetMapOf(s["pub"]);
   _publishedInteractions = hexSet(s["pubI"]);
   _plainSubscribedAttributes = hexSetMapOf(s["sub"]);
   _regionSubscribedAttributes.clear();
   for (auto const& kv : s["subR"].members())
      _regionSubscribedAttributes[parseHex(kv.first)] = hexSetMapOf(kv.second);
   _plainSubscribedInteractions = hexSet(s["subI"]);
   _regionSubscribedInteractions = hexSetMapOf(s["subIR"]);
   _subscribedRates.clear();
   for (auto const& cls : s["rates"].members())
      for (auto const& a : cls.second.members())
         _subscribedRates[parseHex(cls.first)][parseHex(a.first)] = std::strtod(a.second.str().c_str(), nullptr);
   _interactionOrder.clear();
   for (auto const& kv : s["iorder"].members())
      _interactionOrder[parseHex(kv.first)] = kv.second.str() == "ts" ? TIMESTAMP : RECEIVE;
   _interactionTransport.clear();
   for (auto const& kv : s["itransport"].members())
      _interactionTransport[parseHex(kv.first)] = kv.second.str();
   recomputeSubscriptions();

   _regions.clear();
   for (auto const& kv : s["regions"].members())
   {
      Region r;
      r.pending = extentsFromJson(kv.second["p"]);
      r.committed = extentsFromJson(kv.second["c"]);
      _regions[parseHex(kv.first)] = r;
   }
   _regionCounter = static_cast<std::uint32_t>(std::strtoul(s["regionCounter"].str().c_str(), nullptr, 10));

   _reservedNames.clear();
   for (std::string const& n : s["names"].strings())
      _reservedNames.insert(fromUtf8(n));
   size_t k = 0;
   for (Json const& sw : s["switches"].items())
      if (k < 4)
         _advisorySwitches[k++] = sw.boolean();

   // Objects: the saved ones; the MOM objects of the federation stay.
   for (auto it = _objects.begin(); it != _objects.end();)
      it = momClassObject(it->second) ? std::next(it) : _objects.erase(it);
   for (Json const& j : s["objects"].items())
   {
      ObjectInstance o;
      o.handle = parseHex(j["h"].str());
      o.objectClass = parseHex(j["cls"].str());
      o.name = j["name"].str();
      o.topic = j["topic"].str();
      o.knownClass = parseHex(j["known"].str());
      o.registered = j["reg"].boolean();
      o.registrant = parseHex(j["registrant"].str());
      o.locallyDeleted = j["ldel"].boolean();
      o.owned = hexSet(j["owned"]);
      o.relevant = hexSet(j["relevant"]);
      o.scope = hexSet(j["scope"]);
      o.updateRegions = hexSetMapOf(j["uregions"]);
      for (auto const& a : j["order"].members())
         o.order[parseHex(a.first)] = a.second.str() == "ts" ? TIMESTAMP : RECEIVE;
      for (auto const& a : j["transport"].members())
         o.transport[parseHex(a.first)] = a.second.str();
      if (_fdd->objectClass(o.objectClass))
         _objects[o.handle] = o;
   }

   // Time: the saved state, no advance in progress, no queued message.
   bool const isInt = timeIsInteger();
   Json const t = s["time"];
   std::map<std::uint64_t, TimeState> const others = _timeStates;
   std::uint64_t const version = _time.version;
   resetTime();
   _timeStates = others;
   _time.version = version + 1;
   _time.regulating = t["reg"].boolean();
   _time.constrained = t["con"].boolean();
   _time.asyncDelivery = t["async"].boolean();
   _time.time = TimeValue::parse(t["t"].str(), isInt);
   _time.lookahead = TimeValue::parse(t["la"].str(), isInt);
   _time.requested = _time.time;
   _time.regBarrierDone = true;
   _time.conBarrierDone = true;
}

// ================================================ own retained records
void MqttRTIambassador::clearOwnRecords(bool handleChanges)
{
   std::string const ft = _fedTopic;
   std::uint64_t const priv = privilegeAttribute();
   for (auto& kv : _objects)
   {
      ObjectInstance& o = kv.second;
      if (o.mom)
         continue;
      if (!o.owned.empty() || !o.claims.empty() || !o.given.empty())
      {
         ++o.recordsInFlight;
         _transport->publishAsync(ownerRecordTopic(o.handle, _federateHandle), std::string(), 1, true);
      }
      if ((o.owned.count(priv) || o.registered) && !momClassObject(o) && !o.topic.empty())
         _transport->publishAsync(o.topic + "/info", std::string(), 1, true);
   }
   _transport->publishAsync(ft + "/decl/" + hex64(_federateHandle), std::string(), 1, true);
   _lastDeclarations.clear();
   _transport->publishAsync(ft + "/time/" + hex64(_federateHandle), std::string(), 1, true);
   _time.lastPublished.clear();
   std::string const myId = encodeTopicLevel(_transport->settings().clientId);
   for (std::wstring const& n : _reservedNames)
      _transport->publishAsync(ft + "/names/" + encodeTopicLevel(toUtf8(n)) + "/" + myId, std::string(), 1, true);
   if (handleChanges)
   {
      _transport->publishAsync(ft + "/federates/" + hex64(_federateHandle), std::string(), 1, true);
      if (!_momRecordTopic.empty())
         _transport->publishAsync(_momRecordTopic, std::string(), 1, true);
   }
}

void MqttRTIambassador::republishOwnRecords()
{
   std::string const ft = _fedTopic;
   if (!_restore.federateRecord.empty())
   {
      Json record = Json::parse(_restore.federateRecord);
      record.set("name", toUtf8(_federateName));
      record.set("handle", hex64(_federateHandle));
      record.set("state", "joined");
      _transport->publishAsync(ft + "/federates/" + hex64(_federateHandle), record.dump(), 1, true);
   }
   publishDeclarations();
   _time.lastMeaning.clear();
   publishTimeState(true);
   std::uint64_t const priv = privilegeAttribute();
   for (auto& kv : _objects)
   {
      ObjectInstance& o = kv.second;
      if (o.mom)
         continue;
      if (!o.owned.empty())
         publishOwnerRecord(o);
      if ((o.owned.count(priv) || o.registered) && !momClassObject(o) && !o.topic.empty())
         _transport->publishAsync(o.topic + "/info", objectInfo(o), 1, true);
   }
   std::string const myId = _transport->settings().clientId;
   for (std::wstring const& n : _reservedNames)
   {
      Json j = Json::object();
      j.set("clientId", myId);
      j.set("federate", hex64(_federateHandle));
      j.set("state", "reserved");
      _transport->publishAsync(ft + "/names/" + encodeTopicLevel(toUtf8(n)) + "/" + encodeTopicLevel(myId), j.dump(),
                               1, true);
   }
   if (_momFederateObject != 0 && !_momRecordTopic.empty())
   {
      _momRecordTopic = ownerRecordTopic(_momFederateObject, _federateHandle);
      _transport->publishAsync(_momRecordTopic, _momRecordPayload, 1, true);
   }
}

// ================================================================= save
// 4.16
void MqttRTIambassador::requestFederationSave(std::wstring const& label)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      if (_save.active && !_save.done)
         throw SaveInProgress(label);
      if (_restore.active && !_restore.done)
         throw RestoreInProgress(label);
   }
   // Common part with the timed variant below.
   std::string const base = _fedTopic + "/save";
   try
   {
      if (!claimExclusive(base, base + "/state"))
         throw SaveInProgress(label);
      Json state = Json::object();
      {
         std::lock_guard<std::mutex> v(_viewMutex);
         state.set("id", hex64(newUniqueValue("S")));
         state.set("label", toUtf8(label));
         state.set("requester", hex64(_federateHandle));
         Json members = Json::array();
         for (FederateRecord const& r : liveFederates(_fedTopic))
         {
            Json m = Json::object();
            m.set("h", hex64(r.handle));
            m.set("c", r.clientId);
            members.push(m);
         }
         state.set("members", members);
      }
      _transport->publish(base + "/state", state.dump(), 1, true);
      _transport->publish(base + "/claims/" + encodeTopicLevel(_transport->settings().clientId), std::string(), 1, true);
   }
   catch (std::runtime_error const& e)
   {
      throw RTIinternalError(fromUtf8(e.what()));
   }
}

void MqttRTIambassador::requestFederationSave(std::wstring const& label, LogicalTime const& theTime)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   TimeValue const t = TimeValue::of(theTime, timeIsInteger());
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      if (_save.active && !_save.done)
         throw SaveInProgress(label);
      if (_restore.active && !_restore.done)
         throw RestoreInProgress(label);
      if (t < _time.time)
         throw LogicalTimeAlreadyPassed(theTime.toString());
   }
   std::string const base = _fedTopic + "/save";
   try
   {
      if (!claimExclusive(base, base + "/state"))
         throw SaveInProgress(label);
      Json state = Json::object();
      {
         std::lock_guard<std::mutex> v(_viewMutex);
         state.set("id", hex64(newUniqueValue("S")));
         state.set("label", toUtf8(label));
         state.set("time", t.str());
         state.set("requester", hex64(_federateHandle));
         Json members = Json::array();
         for (FederateRecord const& r : liveFederates(_fedTopic))
         {
            Json m = Json::object();
            m.set("h", hex64(r.handle));
            m.set("c", r.clientId);
            members.push(m);
         }
         state.set("members", members);
      }
      _transport->publish(base + "/state", state.dump(), 1, true);
      _transport->publish(base + "/claims/" + encodeTopicLevel(_transport->settings().clientId), std::string(), 1, true);
   }
   catch (std::runtime_error const& e)
   {
      throw RTIinternalError(fromUtf8(e.what()));
   }
}

// 4.18
void MqttRTIambassador::federateSaveBegun()
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   if (_restoreBusy)
      throw RestoreInProgress(L"federation restore in progress");
   std::lock_guard<std::mutex> v(_viewMutex);
   if (!_save.active || _save.done || _save.local != FEDERATE_INSTRUCTED_TO_SAVE)
      throw SaveNotInitiated(L"no federation save initiated");
   _save.local = FEDERATE_SAVING;
   Json st = Json::object();
   st.set("id", _save.id);
   st.set("status", "begun");
   _transport->publishAsync(_fedTopic + "/save/status/" + hex64(_federateHandle), st.dump(), 1, true);
}

// 4.19
void MqttRTIambassador::federateSaveComplete()
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   if (_restoreBusy)
      throw RestoreInProgress(L"federation restore in progress");
   std::lock_guard<std::mutex> v(_viewMutex);
   if (!_save.active || _save.done || _save.local != FEDERATE_SAVING)
      throw FederateHasNotBegunSave(L"federateSaveBegun not invoked");
   _save.local = FEDERATE_WAITING_FOR_FEDERATION_TO_SAVE;
   Json record = Json::object(); // the saved state first: complete implies it is there
   record.set("name", toUtf8(_federateName));
   record.set("type", toUtf8(_federateType));
   record.set("handle", hex64(_federateHandle));
   record.set("state", federateSnapshot());
   _transport->publishAsync(_fedTopic + "/saves/" + encodeTopicLevel(_save.label) + "/" + _save.id + "/" +
                               hex64(_federateHandle),
                            record.dump(), 1, true);
   Json st = Json::object();
   st.set("id", _save.id);
   st.set("status", "complete");
   _transport->publishAsync(_fedTopic + "/save/status/" + hex64(_federateHandle), st.dump(), 1, true);
}

void MqttRTIambassador::federateSaveNotComplete()
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   if (_restoreBusy)
      throw RestoreInProgress(L"federation restore in progress");
   std::lock_guard<std::mutex> v(_viewMutex);
   if (!_save.active || _save.done || _save.local != FEDERATE_SAVING)
      throw FederateHasNotBegunSave(L"federateSaveBegun not invoked");
   _save.local = FEDERATE_WAITING_FOR_FEDERATION_TO_SAVE;
   Json st = Json::object();
   st.set("id", _save.id);
   st.set("status", "notcomplete");
   _transport->publishAsync(_fedTopic + "/save/status/" + hex64(_federateHandle), st.dump(), 1, true);
}

// 4.21
void MqttRTIambassador::abortFederationSave()
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   if (!_save.active || _save.done)
      throw SaveNotInProgress(L"no federation save in progress");
   Json state = Json::parse(_save.raw);
   state.set("aborted", true);
   _transport->publishAsync(_fedTopic + "/save/state", state.dump(), 1, true);
}

// 4.22
void MqttRTIambassador::queryFederationSaveStatus()
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   if (_restoreBusy)
      throw RestoreInProgress(L"federation restore in progress");
   std::lock_guard<std::mutex> v(_viewMutex);
   FederateHandleSaveStatusPairVector statuses;
   bool const inProgress = _save.active && !_save.done;
   for (std::uint64_t h : liveFederateHandles())
   {
      SaveStatus s = NO_SAVE_IN_PROGRESS;
      bool member = false;
      for (SaveMember const& m : _save.members)
         member = member || m.handle == h;
      if (inProgress && member)
      {
         auto st = _save.status.find(h);
         if (h == _federateHandle)
            s = _save.initiated ? _save.local : NO_SAVE_IN_PROGRESS;
         else if (st == _save.status.end())
            s = FEDERATE_INSTRUCTED_TO_SAVE;
         else
            s = st->second == "begun" ? FEDERATE_SAVING : FEDERATE_WAITING_FOR_FEDERATION_TO_SAVE;
      }
      statuses.emplace_back(FederateHandleFriend::create(h), s);
   }
   enqueue([statuses](FederateAmbassador& fa) { fa.federationSaveStatusResponse(statuses); });
}

void MqttRTIambassador::handleSaveTraffic(std::string const& rest, std::string const& payload)
{
   try
   {
      if (rest == "save/state")
      {
         if (payload.empty())
         {
            if (_save.done || !_save.member)
            {
               _save = FederationSave();
               _saveBusy = false;
            }
            return;
         }
         Json j = Json::parse(payload);
         std::string const id = j["id"].str();
         if (id != _save.id)
         {
            _save = FederationSave();
            _save.active = true;
            _save.id = id;
            _save.label = j["label"].str();
            _save.requester = parseHex(j["requester"].str());
            _save.timed = !j["time"].str().empty();
            if (_save.timed)
               _save.time = TimeValue::parse(j["time"].str(), timeIsInteger());
            for (Json const& m : j["members"].items())
            {
               SaveMember sm;
               sm.handle = parseHex(m["h"].str());
               sm.clientId = m["c"].str();
               _save.members.push_back(sm);
               _save.member = _save.member || sm.handle == _federateHandle;
            }
            // Statuses received before the state (retained, other topics).
            for (auto const& kv : recordsUnder(_fedTopic + "/save/status/"))
            {
               try
               {
                  Json st = Json::parse(kv.second);
                  if (st["id"].str() == id)
                     _save.status[parseHex(kv.first.substr(kv.first.rfind('/') + 1))] = st["status"].str();
               }
               catch (std::exception const&)
               {
               }
            }
         }
         _save.raw = payload;
         _save.aborted = j["aborted"].boolean();
         evaluateSave();
      }
      else if (startsWith(rest, "save/status/"))
      {
         if (payload.empty() || !_save.active)
            return;
         Json st = Json::parse(payload);
         if (st["id"].str() != _save.id)
            return;
         _save.status[parseHex(rest.substr(12))] = st["status"].str();
         evaluateSave();
      }
   }
   catch (std::exception const&)
   {
      // Malformed record: ignored.
   }
}

void MqttRTIambassador::evaluateSave()
{
   if (!_save.active || _save.done || !_transport)
      return;
   // Instruction: at once, or at the save time for a time-constrained federate.
   if (_save.member && !_save.initiated && !_save.aborted &&
       (!_save.timed || !_time.constrained || (_time.advance == Advance::None && _save.time <= _time.time)))
   {
      _save.initiated = true;
      _save.local = FEDERATE_INSTRUCTED_TO_SAVE;
      _saveBusy = true;
      std::wstring const label = fromUtf8(_save.label);
      if (_save.timed)
      {
         std::shared_ptr<LogicalTime> t = _save.time.toTime();
         enqueue([label, t](FederateAmbassador& fa) { fa.initiateFederateSave(label, *t); });
      }
      else
         enqueue([label](FederateAmbassador& fa) { fa.initiateFederateSave(label); });
   }

   // Result.
   std::set<std::uint64_t> const live = liveFederateHandles();
   bool success = true, pending = false;
   SaveFailureReason reason = FEDERATE_REPORTED_FAILURE_DURING_SAVE;
   bool gone = false;
   if (_save.aborted)
   {
      success = false;
      reason = SAVE_ABORTED;
   }
   else
      for (SaveMember const& m : _save.members)
      {
         auto st = _save.status.find(m.handle);
         std::string const s = st == _save.status.end() ? std::string() : st->second;
         if (s == "complete")
            continue;
         if (s == "notcomplete")
         {
            success = false;
            continue;
         }
         if (live.count(m.handle))
         {
            pending = true;
            continue;
         }
         success = false;
         gone = true;
         reason = clientAlive(m.clientId) ? FEDERATE_RESIGNED_DURING_SAVE : RTI_DETECTED_FAILURE_DURING_SAVE;
      }
   if (pending && !gone && !_save.aborted)
      return;
   _save.done = true;
   _save.local = NO_SAVE_IN_PROGRESS;
   _saveBusy = false;
   if (_save.member)
   {
      if (success)
         enqueue([](FederateAmbassador& fa) { fa.federationSaved(); });
      else
         enqueue([reason](FederateAmbassador& fa) { fa.federationNotSaved(reason); });
   }

   // Records: by the requester, or by the smallest live member if it is gone.
   std::uint64_t cleaner = live.count(_save.requester) ? _save.requester : 0;
   for (SaveMember const& m : _save.members)
      if (cleaner == 0 && live.count(m.handle))
         cleaner = m.handle;
   if (cleaner != _federateHandle)
      return;
   std::string const saves = _fedTopic + "/saves/" + encodeTopicLevel(_save.label) + "/";
   try
   {
      if (success)
      {
         Json info = Json::object();
         info.set("id", _save.id);
         if (_save.timed)
            info.set("time", _save.time.str());
         Json federates = Json::array();
         for (SaveMember const& m : _save.members)
         {
            auto rec = _view.find(saves + _save.id + "/" + hex64(m.handle));
            if (rec == _view.end())
               continue;
            Json r = Json::parse(rec->second);
            Json f = Json::object();
            f.set("h", hex64(m.handle));
            f.set("n", r["name"].str());
            f.set("t", r["type"].str());
            federates.push(f);
         }
         info.set("federates", federates);
         _transport->publishAsync(saves + "info", info.dump(), 1, true);
      }
      // States of the previous save of this label (success), or of this one (failure).
      for (auto const& kv : recordsUnder(saves))
      {
         std::string const tail = kv.first.substr(saves.size());
         if (tail == "info")
            continue;
         bool const thisSave = startsWith(tail, _save.id + "/");
         if (thisSave != success)
            _transport->publishAsync(kv.first, std::string(), 1, true);
      }
      for (SaveMember const& m : _save.members)
         _transport->publishAsync(_fedTopic + "/save/status/" + hex64(m.handle), std::string(), 1, true);
      _transport->publishAsync(_fedTopic + "/save/state", std::string(), 1, true);
   }
   catch (std::exception const&)
   {
   }
}

// ============================================================== restore
// 4.24
void MqttRTIambassador::requestFederationRestore(std::wstring const& label)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      if (_save.active && !_save.done)
         throw SaveInProgress(label);
      if (_restore.active && !_restore.done)
         throw RestoreInProgress(label);
   }
   std::string const base = _fedTopic + "/restore";
   try
   {
      if (!claimExclusive(base, base + "/state"))
         throw RestoreInProgress(label);
      std::string const claimTopic = base + "/claims/" + encodeTopicLevel(_transport->settings().clientId);
      Json state = Json::object();
      bool possible = false;
      {
         std::lock_guard<std::mutex> v(_viewMutex);
         auto info = _view.find(_fedTopic + "/saves/" + encodeTopicLevel(toUtf8(label)) + "/info");
         if (info != _view.end())
         {
            Json const saved = Json::parse(info->second);
            struct Saved
            {
               std::uint64_t handle;
               std::string name, type;
               bool used;
            };
            std::vector<Saved> savedFederates;
            for (Json const& f : saved["federates"].items())
               savedFederates.push_back(Saved{parseHex(f["h"].str()), f["n"].str(), f["t"].str(), false});
            std::vector<FederateRecord> live = liveFederates(_fedTopic);
            std::sort(live.begin(), live.end(),
                      [](FederateRecord const& a, FederateRecord const& b) { return a.handle < b.handle; });
            std::sort(savedFederates.begin(), savedFederates.end(),
                      [](Saved const& a, Saved const& b) { return a.handle < b.handle; });
            possible = live.size() == savedFederates.size();
            std::map<std::uint64_t, Saved const*> mapping;
            // A federate takes back its own saved identity, then the others
            // take the remaining identities of their type, in handle order.
            for (FederateRecord const& r : live)
               for (Saved& s : savedFederates)
                  if (!s.used && s.handle == r.handle && s.type == r.type)
                  {
                     s.used = true;
                     mapping[r.handle] = &s;
                  }
            for (FederateRecord const& r : live)
            {
               if (mapping.count(r.handle))
                  continue;
               for (Saved& s : savedFederates)
                  if (!s.used && s.type == r.type)
                  {
                     s.used = true;
                     mapping[r.handle] = &s;
                     break;
                  }
            }
            possible = possible && mapping.size() == live.size();
            if (possible)
            {
               state.set("id", hex64(newUniqueValue("R")));
               state.set("label", toUtf8(label));
               state.set("save", saved["id"].str());
               state.set("requester", hex64(_federateHandle));
               Json members = Json::array();
               for (FederateRecord const& r : live)
               {
                  Json m = Json::object();
                  m.set("h", hex64(r.handle));
                  m.set("c", r.clientId);
                  m.set("s", hex64(mapping[r.handle]->handle));
                  m.set("n", mapping[r.handle]->name);
                  members.push(m);
               }
               state.set("members", members);
            }
         }
      }
      if (possible)
      {
         // Succeeded before federationRestoreBegun (delivered by the broker).
         enqueue([label](FederateAmbassador& fa) { fa.requestFederationRestoreSucceeded(label); });
         _transport->publish(base + "/state", state.dump(), 1, true);
      }
      else
         enqueue([label](FederateAmbassador& fa) { fa.requestFederationRestoreFailed(label); });
      _transport->publish(claimTopic, std::string(), 1, true);
   }
   catch (std::runtime_error const& e)
   {
      throw RTIinternalError(fromUtf8(e.what()));
   }
}

// 4.28
void MqttRTIambassador::federateRestoreComplete()
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   if (_saveBusy)
      throw SaveInProgress(L"federation save in progress");
   std::lock_guard<std::mutex> v(_viewMutex);
   if (!_restore.active || _restore.done || _restore.local != FEDERATE_RESTORING)
      throw RestoreNotRequested(L"no federate restore initiated");
   _restore.local = FEDERATE_WAITING_FOR_FEDERATION_TO_RESTORE;
   std::uint64_t pre = 0; // status topics use the pre-restore handle
   for (RestoreMember const& m : _restore.members)
      if (m.saved == _federateHandle)
         pre = m.handle;
   Json st = Json::object();
   st.set("id", _restore.id);
   st.set("status", "complete");
   _transport->publishAsync(_fedTopic + "/restore/status/" + hex64(pre), st.dump(), 1, true);
}

void MqttRTIambassador::federateRestoreNotComplete()
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   if (_saveBusy)
      throw SaveInProgress(L"federation save in progress");
   std::lock_guard<std::mutex> v(_viewMutex);
   if (!_restore.active || _restore.done || _restore.local != FEDERATE_RESTORING)
      throw RestoreNotRequested(L"no federate restore initiated");
   _restore.local = FEDERATE_WAITING_FOR_FEDERATION_TO_RESTORE;
   std::uint64_t pre = 0;
   for (RestoreMember const& m : _restore.members)
      if (m.saved == _federateHandle)
         pre = m.handle;
   Json st = Json::object();
   st.set("id", _restore.id);
   st.set("status", "notcomplete");
   _transport->publishAsync(_fedTopic + "/restore/status/" + hex64(pre), st.dump(), 1, true);
}

// 4.30
void MqttRTIambassador::abortFederationRestore()
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   if (!_restore.active || _restore.done)
      throw RestoreNotInProgress(L"no federation restore in progress");
   Json state = Json::parse(_restore.raw);
   state.set("aborted", true);
   _transport->publishAsync(_fedTopic + "/restore/state", state.dump(), 1, true);
}

// 4.31
void MqttRTIambassador::queryFederationRestoreStatus()
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   if (_saveBusy)
      throw SaveInProgress(L"federation save in progress");
   std::lock_guard<std::mutex> v(_viewMutex);
   FederateRestoreStatusVector statuses;
   if (_restore.active && !_restore.done)
      for (RestoreMember const& m : _restore.members)
      {
         RestoreStatus s = FEDERATE_WAITING_FOR_RESTORE_TO_BEGIN;
         auto st = _restore.status.find(m.handle);
         std::string const v2 = st == _restore.status.end() ? std::string() : st->second;
         if (m.clientId == _transport->settings().clientId)
            s = _restore.local;
         else if (v2 == "cleared")
            s = FEDERATE_PREPARED_TO_RESTORE;
         else if (v2 == "complete" || v2 == "notcomplete")
            s = FEDERATE_WAITING_FOR_FEDERATION_TO_RESTORE;
         statuses.emplace_back(FederateHandleFriend::create(m.handle), FederateHandleFriend::create(m.saved), s);
      }
   else
      for (std::uint64_t h : liveFederateHandles())
         statuses.emplace_back(FederateHandleFriend::create(h), FederateHandleFriend::create(h), NO_RESTORE_IN_PROGRESS);
   enqueue([statuses](FederateAmbassador& fa) { fa.federationRestoreStatusResponse(statuses); });
}

void MqttRTIambassador::handleRestoreTraffic(std::string const& rest, std::string const& payload)
{
   try
   {
      if (rest == "restore/state")
      {
         if (payload.empty())
         {
            if (_restore.done || !_restore.member)
            {
               _restore = FederationRestore();
               _restoreBusy = false;
            }
            return;
         }
         Json j = Json::parse(payload);
         std::string const id = j["id"].str();
         if (id != _restore.id)
         {
            _restore = FederationRestore();
            _restore.active = true;
            _restore.id = id;
            _restore.label = j["label"].str();
            _restore.saveId = j["save"].str();
            _restore.requester = parseHex(j["requester"].str());
            for (Json const& m : j["members"].items())
            {
               RestoreMember rm;
               rm.handle = parseHex(m["h"].str());
               rm.clientId = m["c"].str();
               rm.saved = parseHex(m["s"].str());
               rm.name = m["n"].str();
               _restore.members.push_back(rm);
               if (rm.handle == _federateHandle)
               {
                  _restore.member = true;
                  _restore.saved = rm.saved;
               }
            }
            for (auto const& kv : recordsUnder(_fedTopic + "/restore/status/"))
            {
               try
               {
                  Json st = Json::parse(kv.second);
                  if (st["id"].str() == id)
                     _restore.status[parseHex(kv.first.substr(kv.first.rfind('/') + 1))] = st["status"].str();
               }
               catch (std::exception const&)
               {
               }
            }
            _restore.raw = payload;
            if (_restore.member && !j["aborted"].boolean())
            {
               // Begun, then our records cleared (see the file header).
               _restoreBusy = true;
               _restoring = true;
               _restore.local = FEDERATE_PREPARED_TO_RESTORE;
               auto rec = _view.find(_fedTopic + "/federates/" + hex64(_federateHandle));
               if (rec != _view.end())
                  _restore.federateRecord = rec->second;
               enqueue([](FederateAmbassador& fa) { fa.federationRestoreBegun(); });
               clearOwnRecords(_restore.saved != _federateHandle);
               Json st = Json::object();
               st.set("id", id);
               st.set("status", "cleared");
               _transport->publishAsync(_fedTopic + "/restore/status/" + hex64(_federateHandle), st.dump(), 1, true);
            }
         }
         _restore.raw = payload;
         _restore.aborted = j["aborted"].boolean();
         evaluateRestore();
      }
      else if (startsWith(rest, "restore/status/"))
      {
         if (payload.empty() || !_restore.active)
            return;
         Json st = Json::parse(payload);
         if (st["id"].str() != _restore.id)
            return;
         _restore.status[parseHex(rest.substr(15))] = st["status"].str();
         evaluateRestore();
      }
   }
   catch (std::exception const&)
   {
   }
}

void MqttRTIambassador::evaluateRestore()
{
   if (!_restore.active || _restore.done || !_transport)
      return;
   if (_restore.aborted)
   {
      finishRestore(false, RESTORE_ABORTED);
      return;
   }
   bool allCleared = true, allFinal = true, success = true, gone = false;
   RestoreFailureReason reason = FEDERATE_REPORTED_FAILURE_DURING_RESTORE;
   for (RestoreMember const& m : _restore.members)
   {
      auto st = _restore.status.find(m.handle);
      std::string const s = st == _restore.status.end() ? std::string() : st->second;
      bool const final = s == "complete" || s == "notcomplete" || s == "failed";
      allCleared = allCleared && (s == "cleared" || final);
      allFinal = allFinal && final;
      if (s == "notcomplete")
         success = false;
      if (s == "failed")
      {
         success = false;
         reason = RTI_UNABLE_TO_RESTORE;
      }
      if (s == "resigned" || (!final && !clientAlive(m.clientId)))
      {
         gone = true;
         reason = s == "resigned" ? FEDERATE_RESIGNED_DURING_RESTORE : RTI_DETECTED_FAILURE_DURING_RESTORE;
      }
   }
   if (gone)
   {
      finishRestore(false, reason);
      return;
   }
   if (_restore.member && allCleared && !_restore.loading)
   {
      _restore.loading = true;
      std::string const id = _restore.id;
      enqueueInternal([this, id] { loadRestoredState(id); });
   }
   if (allFinal)
      finishRestore(success, reason);
}

void MqttRTIambassador::finishRestore(bool success, RestoreFailureReason reason)
{
   if (_restore.done)
      return;
   _restore.done = true;
   if (_restore.member)
   {
      if (!_restore.loaded)
      {
         republishOwnRecords(); // cleared but not restored: the current state again
      }
      _restoring = false;
      _restore.local = NO_RESTORE_IN_PROGRESS;
      _restoreBusy = false;
      if (success)
         enqueue([](FederateAmbassador& fa) { fa.federationRestored(); });
      else
         enqueue([reason](FederateAmbassador& fa) { fa.federationNotRestored(reason); });
      discoverPending();
      evaluateAdvisories();
      evaluateTime();
   }
   // Records: by the requester, or by the smallest live member.
   std::uint64_t cleaner = 0;
   for (RestoreMember const& m : _restore.members)
      if (m.handle == _restore.requester && clientAlive(m.clientId))
         cleaner = m.clientId == _transport->settings().clientId ? 1 : 2;
   if (cleaner == 0)
      for (RestoreMember const& m : _restore.members)
         if (clientAlive(m.clientId))
         {
            cleaner = m.clientId == _transport->settings().clientId ? 1 : 2;
            break;
         }
   if (cleaner != 1)
      return;
   try
   {
      for (RestoreMember const& m : _restore.members)
         _transport->publishAsync(_fedTopic + "/restore/status/" + hex64(m.handle), std::string(), 1, true);
      _transport->publishAsync(_fedTopic + "/restore/state", std::string(), 1, true);
   }
   catch (std::exception const&)
   {
   }
}

// Federate thread: every member has cleared its records.
void MqttRTIambassador::loadRestoredState(std::string const& id)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   std::uint64_t oldHandle = 0, newHandle = 0;
   Json saved;
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      if (!_joined || !_restore.active || _restore.id != id || _restore.done)
         return;
      oldHandle = _federateHandle;
      newHandle = _restore.saved;
      auto rec = _view.find(_fedTopic + "/saves/" + encodeTopicLevel(_restore.label) + "/" + _restore.saveId + "/" +
                            hex64(newHandle));
      bool ok = rec != _view.end();
      if (ok)
      {
         try
         {
            saved = Json::parse(rec->second);
         }
         catch (std::exception const&)
         {
            ok = false;
         }
      }
      if (!ok)
      {
         Json st = Json::object();
         st.set("id", id);
         st.set("status", "failed");
         _transport->publishAsync(_fedTopic + "/restore/status/" + hex64(oldHandle), st.dump(), 1, true);
         return;
      }
   }
   try
   {
      if (newHandle != oldHandle) // directed topics follow the handle
      {
         _transport->subscribe(_fedTopic + "/requests/" + hex64(newHandle));
         _transport->subscribe(_fedTopic + "/ownership/" + hex64(newHandle));
         _transport->unsubscribe(_fedTopic + "/requests/" + hex64(oldHandle));
         _transport->unsubscribe(_fedTopic + "/ownership/" + hex64(oldHandle));
      }
      std::wstring name;
      {
         std::lock_guard<std::mutex> v(_viewMutex);
         if (_restore.id != id || _restore.done)
            return;
         _federateHandle = newHandle;
         _federateName = fromUtf8(saved["name"].str());
         name = _federateName;
         loadSnapshot(saved["state"]);
         _restore.loaded = true;
         republishOwnRecords();
         _restore.local = FEDERATE_RESTORING;
      }
      updateBrokerSubscriptions();
      std::wstring const label = fromUtf8(_restore.label);
      FederateHandle const h = FederateHandleFriend::create(newHandle);
      enqueue([label, name, h](FederateAmbassador& fa) { fa.initiateFederateRestore(label, name, h); });
   }
   catch (std::exception const&)
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      Json st = Json::object();
      st.set("id", id);
      st.set("status", "failed");
      _transport->publishAsync(_fedTopic + "/restore/status/" + hex64(oldHandle), st.dump(), 1, true);
   }
}
} // namespace hla_mqtt
