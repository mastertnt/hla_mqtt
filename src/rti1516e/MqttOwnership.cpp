// Ownership management (IEEE 1516.1-2010, chapter 7) over MQTT.
//
// Records (retained), under the joined federation F:
//   F/owners/<instance>/<federate>   JSON {"clientId","owned":[..],"claims":[..],"given":{attr:fed},
//                                          "transport":{attr:name},"regions":{attr:[extents]}}
//     written by that federate only; empty when it owns, claims and gives nothing.
//     Every joined federate subscribes to F/owners/#: the owner of an attribute
//     is the live federate whose record lists it in "owned" (or the target of a
//     "given" entry, during a transfer). No record lists it: unowned.
//   F/objects/<class path>/@<instance>/info   immutable {"instance","class","name","registrant","clientId"}
//
// Messages (JSON, QoS 1, not retained):
//   F/ownership/<federate>   directed: request, cancel, grant, unavailable, notowned, cancelled, mark
//   F/ownership/all          offer (negotiated divestiture), to every federate
//
// The owner of an attribute is the authority for it: acquisitions are requests
// sent to it, queued per attribute (FIFO), granted by unconditional or
// negotiated divestiture (confirmDivestiture) or attributeOwnershipDivestitureIfWanted.
// A grant moves the attribute to "given" in the owner's record, then the new
// owner lists it in "owned" and the old owner drops the "given" entry.
//
// An unowned attribute has no authority: it is claimed. The claimant lists it
// in "claims", then sends itself a marker (round trip through the broker, as
// the barrier of the other exclusive operations): when the marker comes back,
// every record published before it has been seen. No other record owns,
// gives or claims the attribute: the claim wins. Otherwise it is withdrawn;
// the attribute is then requested from its owner, or, if it is only claimed,
// claimed again by the claimant of smallest handle while the others wait.
// Two claimants may withdraw together, never win together.
//
// A federate whose presence goes offline loses its attributes (they become
// unowned); objects whose delete privilege it held are removed (automatic
// resign). Pending requests from it and grants to it are dropped.

#include "MqttRTIambassador.h"

#include "Fdd.h"
#include "HandleFriends.h"
#include "MqttTransport.h"
#include "Wire.h"

#include <algorithm>
#include <cstdlib>
#include <tuple>

namespace hla_mqtt
{
namespace
{
std::uint64_t parseHex(std::string const& s)
{
   return std::strtoull(s.c_str(), nullptr, 16);
}

std::string toHexBytes(void const* data, size_t size)
{
   static char const digits[] = "0123456789abcdef";
   std::string out;
   auto const* p = static_cast<unsigned char const*>(data);
   for (size_t i = 0; i < size; ++i)
   {
      out += digits[p[i] >> 4];
      out += digits[p[i] & 15];
   }
   return out;
}

std::string fromHexBytes(std::string const& hex)
{
   std::string out;
   for (size_t i = 0; i + 1 < hex.size(); i += 2)
      out += static_cast<char>(std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16));
   return out;
}

VariableLengthData toVld(std::string const& s)
{
   return VariableLengthData(s.data(), s.size());
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

AttributeHandleSet handleSet(std::set<std::uint64_t> const& values)
{
   AttributeHandleSet out;
   for (std::uint64_t v : values)
      out.insert(AttributeHandleFriend::create(v));
   return out;
}

std::set<std::uint64_t> valueSet(AttributeHandleSet const& handles)
{
   std::set<std::uint64_t> out;
   for (AttributeHandle const& h : handles)
      out.insert(AttributeHandleFriend::value(h));
   return out;
}

bool startsWith(std::string const& s, std::string const& prefix)
{
   return s.compare(0, prefix.size(), prefix) == 0;
}
} // namespace

// ================================================================ helpers
std::uint64_t MqttRTIambassador::privilegeAttribute() const
{
   Fdd::ObjectClass const* root = _fdd ? _fdd->objectClassByName("HLAobjectRoot") : nullptr;
   Fdd::Attribute const* priv = root ? _fdd->findAttribute(root->handle, "HLAprivilegeToDeleteObject") : nullptr;
   return priv ? priv->handle : 0;
}

bool MqttRTIambassador::federateAlive(std::uint64_t federate) const
{
   if (federate == _federateHandle)
      return true;
   auto it = _view.find(_fedTopic + "/federates/" + hex64(federate));
   if (it == _view.end())
      return false;
   try
   {
      return clientAlive(Json::parse(it->second)["clientId"].str());
   }
   catch (std::exception const&)
   {
      return false;
   }
}

MqttRTIambassador::OwnerRecord const* MqttRTIambassador::liveRecord(std::uint64_t instance,
                                                                     std::uint64_t federate) const
{
   auto i = _ownerRecords.find(instance);
   if (i == _ownerRecords.end())
      return nullptr;
   auto f = i->second.find(federate);
   if (f == i->second.end() || !clientAlive(f->second.clientId))
      return nullptr;
   return &f->second;
}

std::uint64_t MqttRTIambassador::ownerOf(ObjectInstance const& o, std::uint64_t attribute) const
{
   if (o.owned.count(attribute))
      return _federateHandle;
   auto i = _ownerRecords.find(o.handle);
   if (i != _ownerRecords.end())
   {
      for (auto const& kv : i->second)
         if (kv.first != _federateHandle && kv.second.owned.count(attribute) && clientAlive(kv.second.clientId))
            return kv.first;
      for (auto const& kv : i->second) // transfer in progress
      {
         if (kv.first == _federateHandle || !clientAlive(kv.second.clientId))
            continue;
         auto g = kv.second.given.find(attribute);
         if (g != kv.second.given.end() && federateAlive(g->second))
            return g->second;
      }
   }
   auto g = o.given.find(attribute);
   if (g != o.given.end() && federateAlive(g->second))
      return g->second;
   return 0;
}

bool MqttRTIambassador::staleObject(std::uint64_t instance) const
{
   std::uint64_t const priv = privilegeAttribute();
   auto i = _ownerRecords.find(instance);
   if (i == _ownerRecords.end())
      return false;
   bool alive = false, dead = false;
   for (auto const& kv : i->second)
      if (kv.second.owned.count(priv))
         (clientAlive(kv.second.clientId) ? alive : dead) = true;
   return dead && !alive;
}

std::string MqttRTIambassador::ownerRecordTopic(std::uint64_t instance, std::uint64_t federate) const
{
   return _fedTopic + "/owners/" + hex64(instance) + "/" + hex64(federate);
}

std::string MqttRTIambassador::ownerRecord(ObjectInstance const& o) const
{
   if (o.owned.empty() && o.claims.empty() && o.given.empty())
      return std::string();
   Json r = Json::object();
   r.set("clientId", _transport->settings().clientId);
   r.set("owned", hexList(o.owned));
   if (!o.claims.empty())
      r.set("claims", hexList(o.claims));
   if (!o.given.empty())
   {
      Json given = Json::object();
      for (auto const& kv : o.given)
         given.set(hex64(kv.first), hex64(kv.second));
      r.set("given", given);
   }
   Json transport = Json::object();
   for (auto const& kv : o.transport)
      if (o.owned.count(kv.first))
         transport.set(hex64(kv.first), kv.second);
   if (!transport.members().empty())
      r.set("transport", transport);
   Json regions = Json::object();
   for (auto const& attr : o.updateRegions)
   {
      if (!o.owned.count(attr.first))
         continue;
      Json list = Json::array();
      for (Extents const& e : ownExtents(attr.second))
         list.push(extentsToJson(e));
      if (!list.items().empty())
         regions.set(hex64(attr.first), list);
   }
   if (!regions.members().empty())
      r.set("regions", regions);
   return r.dump();
}

std::pair<std::string, std::string> MqttRTIambassador::ownerRecordEntry(ObjectInstance const& o) const
{
   return {ownerRecordTopic(o.handle, _federateHandle), ownerRecord(o)};
}

void MqttRTIambassador::publishOwnerRecord(ObjectInstance& o)
{
   ++o.recordsInFlight;
   _transport->publishAsync(ownerRecordTopic(o.handle, _federateHandle), ownerRecord(o), 1, true);
}

std::vector<std::string> MqttRTIambassador::recordTopicsOf(std::uint64_t instance) const
{
   std::vector<std::string> out{ownerRecordTopic(instance, _federateHandle)};
   auto i = _ownerRecords.find(instance);
   if (i != _ownerRecords.end())
      for (auto const& kv : i->second)
         if (kv.first != _federateHandle)
            out.push_back(ownerRecordTopic(instance, kv.first));
   return out;
}

void MqttRTIambassador::sendOwnership(std::uint64_t to, Json const& message)
{
   Json m = message;
   m.set("from", hex64(_federateHandle));
   _transport->publishAsync(_fedTopic + "/ownership/" + (to ? hex64(to) : std::string("all")), m.dump(), 1, false);
}

namespace
{
Json ownershipMessage(char const* kind, std::uint64_t instance, std::set<std::uint64_t> const& attrs)
{
   Json m = Json::object();
   m.set("kind", kind);
   m.set("instance", hex64(instance));
   m.set("attrs", hexList(attrs));
   return m;
}
} // namespace

// ============================================================ incoming traffic
void MqttRTIambassador::handleOwnershipTraffic(std::string const& rest, std::string const& payload)
{
   try
   {
      if (startsWith(rest, "owners/"))
      {
         std::string const tail = rest.substr(7);
         size_t const slash = tail.find('/');
         if (slash == std::string::npos)
            return;
         handleOwnerRecord(parseHex(tail.substr(0, slash)), parseHex(tail.substr(slash + 1)), payload);
      }
      else if (startsWith(rest, "ownership/") && !payload.empty() && !_restoring)
      {
         std::string const to = rest.substr(10);
         handleOwnershipMessage(to == "all" ? 0 : parseHex(to), payload);
      }
   }
   catch (std::exception const&)
   {
      // Malformed record or message from another federate: ignored.
   }
}

void MqttRTIambassador::handleOwnerRecord(std::uint64_t instance, std::uint64_t federate, std::string const& payload)
{
   auto it = _objects.find(instance);
   if (federate == _federateHandle)
   {
      if (it == _objects.end() || _restoring)
         return;
      ObjectInstance& o = it->second;
      if (o.recordsInFlight > 0)
      {
         --o.recordsInFlight; // our own publication
         return;
      }
      if (!payload.empty() || o.owned.empty())
         return;
      // Our record cleared by another federate: the object was deleted by the
      // holder of its delete privilege.
      bool const known = o.knownClass != 0;
      _objects.erase(it);
      if (known)
      {
         ++_momRemoved;
         ObjectInstanceHandle const h = ObjectInstanceHandleFriend::create(instance);
         enqueue([h](FederateAmbassador& fa) {
            fa.removeObjectInstance(h, VariableLengthData(), RECEIVE, SupplementalRemoveInfo());
         });
      }
      evaluateAdvisories();
      return;
   }
   if (payload.empty())
   {
      auto i = _ownerRecords.find(instance);
      if (i != _ownerRecords.end())
      {
         i->second.erase(federate);
         if (i->second.empty())
            _ownerRecords.erase(i);
      }
   }
   else
   {
      Json j = Json::parse(payload);
      OwnerRecord r;
      r.clientId = j["clientId"].str();
      r.owned = hexSet(j["owned"]);
      r.claims = hexSet(j["claims"]);
      for (auto const& kv : j["given"].members())
         r.given[parseHex(kv.first)] = parseHex(kv.second.str());
      for (auto const& kv : j["transport"].members())
         r.transport[parseHex(kv.first)] = kv.second.str();
      for (auto const& kv : j["regions"].members())
         for (Json const& e : kv.second.items())
            r.regions[parseHex(kv.first)].push_back(extentsFromJson(e));
      _ownerRecords[instance][federate] = r;
   }
   if (it == _objects.end() || _restoring)
      return; // restoring: the table only, the objects are reloaded
   if (!it->second.mom && staleObject(instance)) // retained record of a vanished privilege holder
   {
      bool const known = it->second.knownClass != 0;
      _objects.erase(it);
      if (known)
      {
         ++_momRemoved;
         ObjectInstanceHandle const h = ObjectInstanceHandleFriend::create(instance);
         enqueue([h](FederateAmbassador& fa) {
            fa.removeObjectInstance(h, VariableLengthData(), RECEIVE, SupplementalRemoveInfo());
         });
      }
      return;
   }
   ObjectInstance& o = it->second;
   // Transfers we made: done once the new owner lists the attribute.
   bool changed = false;
   if (OwnerRecord const* r = liveRecord(instance, federate))
      for (auto g = o.given.begin(); g != o.given.end();)
      {
         if (g->second == federate && r->owned.count(g->first))
         {
            g = o.given.erase(g);
            changed = true;
         }
         else
            ++g;
      }
   if (changed)
      publishOwnerRecord(o);
   if (o.knownClass != 0)
      evaluateScope(o);
   driveAcquisitions(o);
}

void MqttRTIambassador::handleOwnershipMessage(std::uint64_t to, std::string const& payload)
{
   Json m = Json::parse(payload);
   std::string const kind = m["kind"].str();
   std::uint64_t const from = parseHex(m["from"].str());
   std::uint64_t const instance = parseHex(m["instance"].str());
   std::set<std::uint64_t> const attrs = hexSet(m["attrs"]);
   if (from == _federateHandle && kind != "mark")
      return; // our own broadcast
   if (to != 0 && to != _federateHandle)
      return;
   auto it = _objects.find(instance);
   if (it == _objects.end())
   {
      if (kind == "request") // object deleted here: the requester asks again elsewhere
         sendOwnership(from, ownershipMessage("notowned", instance, attrs));
      return;
   }
   ObjectInstance& o = it->second;
   ObjectInstanceHandle const h = ObjectInstanceHandleFriend::create(instance);

   if (kind == "mark")
   {
      _pendingMarks.erase(parseHex(m["token"].str()));
      std::uint64_t const self = _federateHandle;
      std::set<std::uint64_t> won;
      auto records = _ownerRecords.find(instance);
      for (std::uint64_t a : attrs)
      {
         if (!o.claims.count(a))
            continue;
         bool contested = false;
         if (records != _ownerRecords.end())
            for (auto const& kv : records->second)
            {
               if (kv.first == self || !clientAlive(kv.second.clientId))
                  continue;
               auto g = kv.second.given.find(a);
               contested = contested || kv.second.owned.count(a) || kv.second.claims.count(a) ||
                           (g != kv.second.given.end() && federateAlive(g->second));
            }
         o.claims.erase(a);
         if (!contested)
            won.insert(a);
      }
      std::map<std::string, std::set<std::uint64_t>> byTag;
      std::set<std::uint64_t> cancelled;
      for (std::uint64_t a : attrs)
      {
         auto acq = o.acquiring.find(a);
         if (acq == o.acquiring.end())
            continue;
         if (won.count(a))
         {
            if (!acq->second.silent)
               byTag[acq->second.tag].insert(a);
            o.acquiring.erase(acq);
         }
         else if (acq->second.cancelling)
         {
            cancelled.insert(a);
            o.acquiring.erase(acq);
         }
      }
      o.owned.insert(won.begin(), won.end());
      // Lost claims are claimed again in the same record (never withdrawn
      // then claimed in two publications: the others could slip in between).
      if (!driveAcquisitions(o))
         publishOwnerRecord(o);
      for (auto const& kv : byTag)
      {
         AttributeHandleSet const secured = handleSet(kv.second);
         VariableLengthData const tag = toVld(fromHexBytes(kv.first));
         enqueue([h, secured, tag](FederateAmbassador& fa) { fa.attributeOwnershipAcquisitionNotification(h, secured, tag); });
      }
      if (!cancelled.empty())
      {
         AttributeHandleSet const c = handleSet(cancelled);
         enqueue([h, c](FederateAmbassador& fa) { fa.confirmAttributeOwnershipAcquisitionCancellation(h, c); });
      }
      if (!won.empty())
         evaluateAdvisories();
   }
   else if (kind == "request")
   {
      bool const ifAvailable = m["ifAvailable"].boolean();
      bool const forced = m["force"].boolean();
      std::set<std::uint64_t> notOwned, unavailable, release, confirm, seized;
      for (std::uint64_t a : attrs)
      {
         if (!o.owned.count(a))
         {
            notOwned.insert(a);
            continue;
         }
         if (forced) // HLAmodifyAttributeState: divested at once, without callback
         {
            std::vector<OwnershipRequest>& queue = o.requests[a];
            queue.erase(std::remove_if(queue.begin(), queue.end(),
                                       [from](OwnershipRequest const& r) { return r.federate == from; }),
                        queue.end());
            queue.insert(queue.begin(), OwnershipRequest{from, false});
            seized.insert(a);
            continue;
         }
         if (ifAvailable && !o.offered.count(a))
         {
            unavailable.insert(a);
            continue;
         }
         std::vector<OwnershipRequest>& queue = o.requests[a];
         auto same = std::find_if(queue.begin(), queue.end(), [from](OwnershipRequest const& r) { return r.federate == from; });
         if (same == queue.end())
            queue.push_back(OwnershipRequest{from, ifAvailable});
         (o.offered.count(a) ? confirm : release).insert(a);
      }
      if (!notOwned.empty())
         sendOwnership(from, ownershipMessage("notowned", instance, notOwned));
      if (!seized.empty())
         grant(o, seized);
      if (!unavailable.empty())
         sendOwnership(from, ownershipMessage("unavailable", instance, unavailable));
      if (!confirm.empty())
      {
         AttributeHandleSet const c = handleSet(confirm);
         enqueue([h, c](FederateAmbassador& fa) { fa.requestDivestitureConfirmation(h, c); });
      }
      if (!release.empty())
      {
         AttributeHandleSet const r = handleSet(release);
         VariableLengthData const tag = toVld(fromHexBytes(m["tag"].str()));
         enqueue([h, r, tag](FederateAmbassador& fa) { fa.requestAttributeOwnershipRelease(h, r, tag); });
      }
   }
   else if (kind == "cancel")
   {
      std::set<std::uint64_t> cancelled;
      for (std::uint64_t a : attrs)
      {
         auto g = o.given.find(a);
         if (g != o.given.end() && g->second == from)
            continue; // already granted: the grant is on its way
         auto q = o.requests.find(a);
         if (q != o.requests.end())
         {
            q->second.erase(std::remove_if(q->second.begin(), q->second.end(),
                                           [from](OwnershipRequest const& r) { return r.federate == from; }),
                            q->second.end());
            if (q->second.empty())
               o.requests.erase(q);
         }
         cancelled.insert(a);
      }
      if (!cancelled.empty())
         sendOwnership(from, ownershipMessage("cancelled", instance, cancelled));
   }
   else if (kind == "grant")
   {
      std::map<std::string, std::set<std::uint64_t>> byTag;
      for (std::uint64_t a : attrs)
      {
         auto acq = o.acquiring.find(a);
         std::string const tag = acq == o.acquiring.end() ? std::string() : acq->second.tag;
         bool const silent = acq != o.acquiring.end() && acq->second.silent;
         if (acq != o.acquiring.end())
            o.acquiring.erase(acq);
         o.claims.erase(a);
         o.owned.insert(a);
         if (!silent)
            byTag[tag].insert(a);
      }
      publishOwnerRecord(o);
      for (auto const& kv : byTag)
      {
         AttributeHandleSet const secured = handleSet(kv.second);
         VariableLengthData const tag = toVld(fromHexBytes(kv.first));
         enqueue([h, secured, tag](FederateAmbassador& fa) { fa.attributeOwnershipAcquisitionNotification(h, secured, tag); });
      }
      evaluateAdvisories();
   }
   else if (kind == "unavailable" || kind == "cancelled")
   {
      std::set<std::uint64_t> done;
      for (std::uint64_t a : attrs)
      {
         auto acq = o.acquiring.find(a);
         if (acq == o.acquiring.end() || (kind == "cancelled" && !acq->second.cancelling))
            continue;
         if (kind == "unavailable" && acq->second.cancelling)
            continue; // the cancellation is confirmed by its own answer
         o.acquiring.erase(acq);
         done.insert(a);
      }
      if (done.empty())
         return;
      AttributeHandleSet const d = handleSet(done);
      if (kind == "unavailable")
         enqueue([h, d](FederateAmbassador& fa) { fa.attributeOwnershipUnavailable(h, d); });
      else
         enqueue([h, d](FederateAmbassador& fa) { fa.confirmAttributeOwnershipAcquisitionCancellation(h, d); });
   }
   else if (kind == "notowned")
   {
      std::set<std::uint64_t> cancelled;
      for (std::uint64_t a : attrs)
      {
         auto acq = o.acquiring.find(a);
         if (acq == o.acquiring.end() || acq->second.askedTo != from)
            continue;
         if (acq->second.cancelling)
         {
            cancelled.insert(a);
            o.acquiring.erase(acq);
         }
         else
            acq->second.askedTo = 0; // asked again, from the current owner
      }
      if (!cancelled.empty())
      {
         AttributeHandleSet const c = handleSet(cancelled);
         enqueue([h, c](FederateAmbassador& fa) { fa.confirmAttributeOwnershipAcquisitionCancellation(h, c); });
      }
      driveAcquisitions(o);
   }
   else if (kind == "offer")
   {
      if (o.knownClass == 0)
         return;
      auto pub = _publishedAttributes.find(o.knownClass);
      if (pub == _publishedAttributes.end())
         return;
      std::set<std::uint64_t> offered;
      for (std::uint64_t a : attrs)
         if (pub->second.count(a) && !o.owned.count(a))
            offered.insert(a);
      if (offered.empty())
         return;
      AttributeHandleSet const off = handleSet(offered);
      VariableLengthData const tag = toVld(fromHexBytes(m["tag"].str()));
      enqueue([h, off, tag](FederateAmbassador& fa) { fa.requestAttributeOwnershipAssumption(h, off, tag); });
   }
}

// ============================================================ acquisition engine
bool MqttRTIambassador::driveAcquisitions(ObjectInstance& o)
{
   if (o.acquiring.empty())
      return false;
   // (owner, ifAvailable, tag, forced) -> attributes
   std::map<std::tuple<std::uint64_t, bool, std::string, bool>, std::set<std::uint64_t>> asks;
   std::set<std::uint64_t> claimNow;
   auto records = _ownerRecords.find(o.handle);
   for (auto& kv : o.acquiring)
   {
      std::uint64_t const a = kv.first;
      Acquisition& acq = kv.second;
      if (o.claims.count(a) || acq.cancelling)
         continue;
      std::uint64_t const owner = ownerOf(o, a);
      if (owner == _federateHandle)
         continue;
      if (owner != 0)
      {
         if (acq.askedTo != owner)
         {
            acq.askedTo = owner;
            asks[std::make_tuple(owner, acq.ifAvailable, acq.tag, acq.forced)].insert(a);
         }
         continue;
      }
      acq.askedTo = 0;
      bool lowerClaimant = false;
      if (records != _ownerRecords.end())
         for (auto const& r : records->second)
            lowerClaimant = lowerClaimant || (r.first < _federateHandle && r.second.claims.count(a) &&
                                              clientAlive(r.second.clientId));
      if (!lowerClaimant)
         claimNow.insert(a);
   }
   for (auto const& kv : asks)
   {
      Json m = ownershipMessage("request", o.handle, kv.second);
      m.set("ifAvailable", std::get<1>(kv.first));
      m.set("tag", std::get<2>(kv.first));
      if (std::get<3>(kv.first))
         m.set("force", true);
      sendOwnership(std::get<0>(kv.first), m);
   }
   if (claimNow.empty())
      return false;
   o.claims.insert(claimNow.begin(), claimNow.end());
   publishOwnerRecord(o);
   std::uint64_t const token = ++_ownershipMarks;
   _pendingMarks[token] = o.handle;
   Json m = ownershipMessage("mark", o.handle, claimNow);
   m.set("token", hex64(token));
   sendOwnership(_federateHandle, m);
   return true;
}

void MqttRTIambassador::grant(ObjectInstance& o, std::set<std::uint64_t> const& attrs)
{
   std::map<std::uint64_t, std::set<std::uint64_t>> byFederate;
   for (std::uint64_t a : attrs)
   {
      auto q = o.requests.find(a);
      if (q == o.requests.end() || q->second.empty())
         continue;
      std::uint64_t const to = q->second.front().federate;
      for (size_t k = 1; k < q->second.size(); ++k) // the others ask the new owner
         sendOwnership(q->second[k].federate, ownershipMessage("notowned", o.handle, {a}));
      o.requests.erase(q);
      o.owned.erase(a);
      o.offered.erase(a);
      o.relevant.erase(a);
      o.updateRegions.erase(a);
      o.order.erase(a);
      o.transport.erase(a);
      o.given[a] = to;
      byFederate[to].insert(a);
   }
   if (byFederate.empty())
      return;
   publishOwnerRecord(o); // "given" before the grant: the attribute is never seen unowned
   for (auto const& kv : byFederate)
      sendOwnership(kv.first, ownershipMessage("grant", o.handle, kv.second));
   evaluateAdvisories();
}

void MqttRTIambassador::divest(ObjectInstance& o, std::set<std::uint64_t> const& attrs)
{
   std::set<std::uint64_t> requested, released;
   for (std::uint64_t a : attrs)
   {
      auto q = o.requests.find(a);
      (q != o.requests.end() && !q->second.empty() ? requested : released).insert(a);
   }
   grant(o, requested); // pending acquisitions get the attributes
   for (std::uint64_t a : released)
   {
      o.owned.erase(a);
      o.offered.erase(a);
      o.relevant.erase(a);
      o.updateRegions.erase(a);
      o.order.erase(a);
      o.transport.erase(a);
   }
   if (!released.empty())
   {
      publishOwnerRecord(o);
      evaluateAdvisories();
   }
}

void MqttRTIambassador::cancelAcquisitions(ObjectInstance& o, std::set<std::uint64_t> const& attrs, bool notify)
{
   std::map<std::uint64_t, std::set<std::uint64_t>> byOwner;
   std::set<std::uint64_t> now;
   bool claimsDropped = false;
   for (std::uint64_t a : attrs)
   {
      auto acq = o.acquiring.find(a);
      if (acq == o.acquiring.end())
         continue;
      if (acq->second.askedTo != 0)
         byOwner[acq->second.askedTo].insert(a);
      if (!notify)
      {
         claimsDropped = o.claims.erase(a) != 0 || claimsDropped;
         o.acquiring.erase(acq);
      }
      else if (o.claims.count(a) || acq->second.askedTo != 0)
         acq->second.cancelling = true; // confirmed by the marker or the owner's answer
      else
      {
         now.insert(a); // waiting: nothing sent yet
         o.acquiring.erase(acq);
      }
   }
   for (auto const& kv : byOwner)
      sendOwnership(kv.first, ownershipMessage("cancel", o.handle, kv.second));
   if (claimsDropped)
      publishOwnerRecord(o);
   if (notify && !now.empty())
   {
      ObjectInstanceHandle const h = ObjectInstanceHandleFriend::create(o.handle);
      AttributeHandleSet const c = handleSet(now);
      enqueue([h, c](FederateAmbassador& fa) { fa.confirmAttributeOwnershipAcquisitionCancellation(h, c); });
   }
}

void MqttRTIambassador::ownershipPresenceLost(std::string const& clientId)
{
   auto clientOf = [this](std::uint64_t federate) {
      auto it = _view.find(_fedTopic + "/federates/" + hex64(federate));
      if (it == _view.end())
         return std::string();
      try
      {
         return Json::parse(it->second)["clientId"].str();
      }
      catch (std::exception const&)
      {
         return std::string();
      }
   };
   // Objects whose delete privilege holder is gone: removed (automatic resign).
   for (auto it = _objects.begin(); it != _objects.end();)
   {
      ObjectInstance const& o = it->second;
      if (o.mom || !staleObject(o.handle))
      {
         ++it;
         continue;
      }
      if (o.knownClass != 0)
      {
         ++_momRemoved;
         ObjectInstanceHandle const h = ObjectInstanceHandleFriend::create(o.handle);
         enqueue([h](FederateAmbassador& fa) {
            fa.removeObjectInstance(h, VariableLengthData(), RECEIVE, SupplementalRemoveInfo());
         });
      }
      _ownerRecords.erase(o.handle);
      it = _objects.erase(it);
   }
   for (auto& kv : _objects)
   {
      ObjectInstance& o = kv.second;
      if (o.mom)
         continue;
      bool changed = false;
      for (auto q = o.requests.begin(); q != o.requests.end();)
      {
         q->second.erase(std::remove_if(q->second.begin(), q->second.end(),
                                        [&](OwnershipRequest const& r) { return clientOf(r.federate) == clientId; }),
                         q->second.end());
         q = q->second.empty() ? o.requests.erase(q) : std::next(q);
      }
      for (auto g = o.given.begin(); g != o.given.end();)
      {
         if (clientOf(g->second) == clientId)
         {
            g = o.given.erase(g); // granted to a vanished federate: unowned
            changed = true;
         }
         else
            ++g;
      }
      if (changed)
         publishOwnerRecord(o);
      // Requests sent to a vanished owner: asked again (the attribute is now
      // unowned, or owned by another), or cancelled if a cancellation waited.
      std::set<std::uint64_t> cancelled;
      for (auto acq = o.acquiring.begin(); acq != o.acquiring.end();)
      {
         bool const ownerGone = acq->second.askedTo != 0 && !federateAlive(acq->second.askedTo);
         if (ownerGone)
            acq->second.askedTo = 0;
         if (ownerGone && acq->second.cancelling)
         {
            cancelled.insert(acq->first);
            acq = o.acquiring.erase(acq);
         }
         else
            ++acq;
      }
      if (!cancelled.empty())
      {
         ObjectInstanceHandle const h = ObjectInstanceHandleFriend::create(o.handle);
         AttributeHandleSet const c = handleSet(cancelled);
         enqueue([h, c](FederateAmbassador& fa) { fa.confirmAttributeOwnershipAcquisitionCancellation(h, c); });
      }
      if (o.knownClass != 0)
         evaluateScope(o);
      driveAcquisitions(o);
   }
   evaluateAdvisories();
}

// MOM HLAmodifyAttributeState (11.4): ownership given to or taken from this
// federate, without notification to either federate.
std::wstring MqttRTIambassador::momModifyAttributeState(std::uint64_t instance, std::uint64_t attribute, bool owned)
{
   auto it = _objects.find(instance);
   if (it == _objects.end() || it->second.knownClass == 0)
      return L"object instance not known by the federate";
   ObjectInstance& o = it->second;
   if (!_fdd->classHasAttribute(o.knownClass, attribute))
      return L"attribute not defined for the known class of the object instance";
   Fdd::ObjectClass const* c = _fdd->objectClass(o.objectClass);
   if (o.mom || (c && startsWith(c->qualified, "HLAobjectRoot.HLAmanager")))
      return L"attribute owned by the RTI";
   if (!owned)
   {
      if (!o.owned.count(attribute))
         return std::wstring(); // already not owned by this federate
      o.owned.erase(attribute);
      o.offered.erase(attribute);
      o.relevant.erase(attribute);
      o.updateRegions.erase(attribute);
      o.order.erase(attribute);
      o.transport.erase(attribute);
      o.requests.erase(attribute); // the requesters see it unowned and claim it
      publishOwnerRecord(o);
      evaluateAdvisories();
      return std::wstring();
   }
   auto pub = _publishedAttributes.find(o.knownClass);
   if (pub == _publishedAttributes.end() || !pub->second.count(attribute))
      return L"attribute not published by the federate at the known class";
   if (o.owned.count(attribute))
      return std::wstring();
   bool const existed = o.acquiring.count(attribute) != 0;
   Acquisition& acq = o.acquiring[attribute];
   if (!existed)
      acq.silent = true; // otherwise the federate's own acquisition is notified as usual
   acq.forced = true;
   acq.ifAvailable = false;
   acq.askedTo = 0; // asked again, forced
   driveAcquisitions(o);
   return std::wstring();
}

// ================================================================ services
MqttRTIambassador::ObjectInstance& MqttRTIambassador::ownershipObject(ObjectInstanceHandle const& h,
                                                                      AttributeHandleSet const& attrs,
                                                                      std::set<std::uint64_t>& values)
{
   auto it = _objects.find(ObjectInstanceHandleFriend::value(h));
   if (it == _objects.end() || it->second.knownClass == 0)
      throw ObjectInstanceNotKnown(h.toString());
   for (AttributeHandle const& a : attrs)
      if (!_fdd->classHasAttribute(it->second.knownClass, AttributeHandleFriend::value(a)))
         throw AttributeNotDefined(a.toString());
   values = valueSet(attrs);
   return it->second;
}

namespace
{
template <class Exc>
void requireAll(std::set<std::uint64_t> const& values, std::set<std::uint64_t> const& in)
{
   for (std::uint64_t a : values)
      if (!in.count(a))
         throw Exc(AttributeHandleFriend::create(a).toString());
}
} // namespace

// 7.2
void MqttRTIambassador::unconditionalAttributeOwnershipDivestiture(ObjectInstanceHandle theObject,
                                                                  AttributeHandleSet const& theAttributes)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   std::lock_guard<std::mutex> v(_viewMutex);
   std::set<std::uint64_t> attrs;
   ObjectInstance& o = ownershipObject(theObject, theAttributes, attrs);
   requireAll<AttributeNotOwned>(attrs, o.owned);
   divest(o, attrs);
}

// 7.3
void MqttRTIambassador::negotiatedAttributeOwnershipDivestiture(ObjectInstanceHandle theObject,
                                                               AttributeHandleSet const& theAttributes,
                                                               VariableLengthData const& theUserSuppliedTag)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   std::lock_guard<std::mutex> v(_viewMutex);
   std::set<std::uint64_t> attrs;
   ObjectInstance& o = ownershipObject(theObject, theAttributes, attrs);
   requireAll<AttributeNotOwned>(attrs, o.owned);
   for (std::uint64_t a : attrs)
      if (o.offered.count(a))
         throw AttributeAlreadyBeingDivested(AttributeHandleFriend::create(a).toString());
   o.offered.insert(attrs.begin(), attrs.end());
   Json m = ownershipMessage("offer", o.handle, attrs);
   m.set("tag", toHexBytes(theUserSuppliedTag.data(), theUserSuppliedTag.size()));
   sendOwnership(0, m);
   std::set<std::uint64_t> requested; // acquisitions already pending: confirmation asked at once
   for (std::uint64_t a : attrs)
      if (o.requests.count(a))
         requested.insert(a);
   if (!requested.empty())
   {
      AttributeHandleSet const r = handleSet(requested);
      enqueue([theObject, r](FederateAmbassador& fa) { fa.requestDivestitureConfirmation(theObject, r); });
   }
}

// 7.6
void MqttRTIambassador::confirmDivestiture(ObjectInstanceHandle theObject, AttributeHandleSet const& confirmedAttributes,
                                           VariableLengthData const& /*theUserSuppliedTag*/)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   std::lock_guard<std::mutex> v(_viewMutex);
   std::set<std::uint64_t> attrs;
   ObjectInstance& o = ownershipObject(theObject, confirmedAttributes, attrs);
   requireAll<AttributeNotOwned>(attrs, o.owned);
   requireAll<AttributeDivestitureWasNotRequested>(attrs, o.offered);
   for (std::uint64_t a : attrs)
      if (!o.requests.count(a))
         throw NoAcquisitionPending(AttributeHandleFriend::create(a).toString());
   grant(o, attrs);
}

// 7.8
void MqttRTIambassador::attributeOwnershipAcquisition(ObjectInstanceHandle theObject,
                                                     AttributeHandleSet const& desiredAttributes,
                                                     VariableLengthData const& theUserSuppliedTag)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   std::lock_guard<std::mutex> v(_viewMutex);
   std::set<std::uint64_t> attrs;
   ObjectInstance& o = ownershipObject(theObject, desiredAttributes, attrs);
   auto pub = _publishedAttributes.find(o.knownClass);
   if (pub == _publishedAttributes.end() || pub->second.empty())
      throw ObjectClassNotPublished(ObjectClassHandleFriend::create(o.knownClass).toString());
   requireAll<AttributeNotPublished>(attrs, pub->second);
   for (std::uint64_t a : attrs)
      if (o.owned.count(a))
         throw FederateOwnsAttributes(AttributeHandleFriend::create(a).toString());
   std::string const tag = toHexBytes(theUserSuppliedTag.data(), theUserSuppliedTag.size());
   for (std::uint64_t a : attrs)
   {
      Acquisition& acq = o.acquiring[a]; // a pending IfAvailable becomes an acquisition (7.8)
      if (acq.ifAvailable && acq.askedTo != 0)
         acq.askedTo = 0; // asked again without "if available"
      acq.ifAvailable = false;
      acq.tag = tag;
      acq.cancelling = false;
      acq.silent = false;
   }
   driveAcquisitions(o);
}

// 7.9
void MqttRTIambassador::attributeOwnershipAcquisitionIfAvailable(ObjectInstanceHandle theObject,
                                                                AttributeHandleSet const& desiredAttributes)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   std::lock_guard<std::mutex> v(_viewMutex);
   std::set<std::uint64_t> attrs;
   ObjectInstance& o = ownershipObject(theObject, desiredAttributes, attrs);
   auto pub = _publishedAttributes.find(o.knownClass);
   if (pub == _publishedAttributes.end() || pub->second.empty())
      throw ObjectClassNotPublished(ObjectClassHandleFriend::create(o.knownClass).toString());
   requireAll<AttributeNotPublished>(attrs, pub->second);
   for (std::uint64_t a : attrs)
      if (o.owned.count(a))
         throw FederateOwnsAttributes(AttributeHandleFriend::create(a).toString());
   for (std::uint64_t a : attrs)
      if (o.acquiring.count(a))
         throw AttributeAlreadyBeingAcquired(AttributeHandleFriend::create(a).toString());
   for (std::uint64_t a : attrs)
      o.acquiring[a].ifAvailable = true;
   driveAcquisitions(o);
}

// 7.12
void MqttRTIambassador::attributeOwnershipReleaseDenied(ObjectInstanceHandle theObject,
                                                       AttributeHandleSet const& theAttributes)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   std::lock_guard<std::mutex> v(_viewMutex);
   std::set<std::uint64_t> attrs;
   ObjectInstance& o = ownershipObject(theObject, theAttributes, attrs);
   requireAll<AttributeNotOwned>(attrs, o.owned);
   // The requests stay pending (the requesters may cancel them); a later
   // divestiture still satisfies them.
}

// 7.13
void MqttRTIambassador::attributeOwnershipDivestitureIfWanted(ObjectInstanceHandle theObject,
                                                             AttributeHandleSet const& theAttributes,
                                                             AttributeHandleSet& theDivestedAttributes)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   std::lock_guard<std::mutex> v(_viewMutex);
   std::set<std::uint64_t> attrs;
   ObjectInstance& o = ownershipObject(theObject, theAttributes, attrs);
   requireAll<AttributeNotOwned>(attrs, o.owned);
   std::set<std::uint64_t> wanted;
   for (std::uint64_t a : attrs)
   {
      auto q = o.requests.find(a);
      if (q != o.requests.end() && !q->second.empty())
         wanted.insert(a);
   }
   grant(o, wanted);
   theDivestedAttributes = handleSet(wanted);
}

// 7.14
void MqttRTIambassador::cancelNegotiatedAttributeOwnershipDivestiture(ObjectInstanceHandle theObject,
                                                                     AttributeHandleSet const& theAttributes)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   std::set<std::uint64_t> attrs;
   ObjectInstance& o = ownershipObject(theObject, theAttributes, attrs);
   requireAll<AttributeNotOwned>(attrs, o.owned);
   requireAll<AttributeDivestitureWasNotRequested>(attrs, o.offered);
   std::map<std::uint64_t, std::set<std::uint64_t>> unavailable; // "if available" requests end here
   for (std::uint64_t a : attrs)
   {
      o.offered.erase(a);
      auto q = o.requests.find(a);
      if (q == o.requests.end())
         continue;
      for (OwnershipRequest const& r : q->second)
         if (r.ifAvailable)
            unavailable[r.federate].insert(a);
      q->second.erase(std::remove_if(q->second.begin(), q->second.end(),
                                     [](OwnershipRequest const& r) { return r.ifAvailable; }),
                      q->second.end());
      if (q->second.empty())
         o.requests.erase(q);
   }
   for (auto const& kv : unavailable)
      sendOwnership(kv.first, ownershipMessage("unavailable", o.handle, kv.second));
}

// 7.15
void MqttRTIambassador::cancelAttributeOwnershipAcquisition(ObjectInstanceHandle theObject,
                                                           AttributeHandleSet const& theAttributes)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   std::lock_guard<std::mutex> v(_viewMutex);
   std::set<std::uint64_t> attrs;
   ObjectInstance& o = ownershipObject(theObject, theAttributes, attrs);
   for (std::uint64_t a : attrs)
      if (o.owned.count(a))
         throw AttributeAlreadyOwned(AttributeHandleFriend::create(a).toString());
   for (std::uint64_t a : attrs)
   {
      auto acq = o.acquiring.find(a);
      if (acq == o.acquiring.end() || acq->second.ifAvailable || acq->second.cancelling || acq->second.silent)
         throw AttributeAcquisitionWasNotRequested(AttributeHandleFriend::create(a).toString());
   }
   cancelAcquisitions(o, attrs, true);
}

// 7.17
void MqttRTIambassador::queryAttributeOwnership(ObjectInstanceHandle theObject, AttributeHandle theAttribute)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   std::lock_guard<std::mutex> v(_viewMutex);
   AttributeHandleSet one;
   one.insert(theAttribute);
   std::set<std::uint64_t> attrs;
   ObjectInstance& o = ownershipObject(theObject, one, attrs);
   std::uint64_t const a = AttributeHandleFriend::value(theAttribute);
   Fdd::ObjectClass const* c = _fdd->objectClass(o.objectClass);
   if (o.mom || (c && startsWith(c->qualified, "HLAobjectRoot.HLAmanager")))
   {
      enqueue([theObject, theAttribute](FederateAmbassador& fa) { fa.attributeIsOwnedByRTI(theObject, theAttribute); });
      return;
   }
   std::uint64_t const owner = ownerOf(o, a);
   if (owner == 0)
      enqueue([theObject, theAttribute](FederateAmbassador& fa) { fa.attributeIsNotOwned(theObject, theAttribute); });
   else
   {
      FederateHandle const f = FederateHandleFriend::create(owner);
      enqueue([theObject, theAttribute, f](FederateAmbassador& fa) { fa.informAttributeOwnership(theObject, theAttribute, f); });
   }
}

// 7.19
bool MqttRTIambassador::isAttributeOwnedByFederate(ObjectInstanceHandle theObject, AttributeHandle theAttribute)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   std::lock_guard<std::mutex> v(_viewMutex);
   AttributeHandleSet one;
   one.insert(theAttribute);
   std::set<std::uint64_t> attrs;
   ObjectInstance& o = ownershipObject(theObject, one, attrs);
   return o.owned.count(AttributeHandleFriend::value(theAttribute)) != 0;
}
} // namespace hla_mqtt
