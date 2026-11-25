// Declaration management (IEEE 1516.1-2010, chapter 5) and object management
// (chapter 6) over MQTT.
//
// Topics under the joined federation F (class path = qualified name, one
// topic level per component, e.g. HLAobjectRoot/Vehicle/Car):
//   F/objects/<class path>/@<instance>/info     retained JSON {"instance","class","name","registrant","clientId"}
//                                               (immutable; ownership: F/owners/..., see MqttOwnership.cpp)
//   F/objects/<class path>/@<instance>/update   binary Update
//   F/objects/<class path>/@<instance>/delete   binary Delete (then info is cleared)
//   F/interactions/<class path>/@               binary Interaction
//   F/requests/<owner federate handle>          binary Request for one instance (one per owner)
//   F/classrequests/<class path>/@              binary Request for a class
//
// Subscribing to a class subscribes to "<class path>/#" on the broker, which
// also covers the subclasses: the broker filters the traffic. Only a minimal
// cover is subscribed (no class together with one of its ancestors), so each
// message arrives once; a (sender, sequence) guard drops the duplicates that
// overlapping subscriptions can still produce while the cover changes.
//
// Discovery comes from the retained info record (objects registered before the
// subscription included). An object whose delete privilege holder is no longer
// present (Last Will) is removed, as the automatic resign of a crashed federate
// would do.
//
// Time-stamped variants are accepted and delivered in receive order with their
// time (the sender cannot be time-regulating before time management exists).
// Ownership: the registering federate owns the attributes it publishes at
// registration, and HLAprivilegeToDeleteObject; transfers in MqttOwnership.cpp.

#include "MqttRTIambassador.h"

#include "Fdd.h"
#include "HandleFriends.h"
#include "Json.h"
#include "Log.h"
#include "MqttTransport.h"
#include "Utf.h"
#include "Wire.h"

#include <algorithm>
#include <utility>

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

std::string tagBytes(VariableLengthData const& tag)
{
   return tag.data() ? std::string(static_cast<char const*>(tag.data()), tag.size()) : std::string();
}

VariableLengthData toVld(std::string const& s)
{
   return VariableLengthData(s.data(), s.size());
}

int const kReservationAttempts = 8;
} // namespace

// ================================================================ helpers
std::uint64_t MqttRTIambassador::newUniqueValue(char const* kind)
{
   return fnv1a64(std::string(kind) + "|" + _transport->settings().clientId + "|" +
                  std::to_string(++_uniqueCounter));
}

bool MqttRTIambassador::isSubclassOf(std::uint64_t objectClass, std::uint64_t ancestor) const
{
   for (Fdd::ObjectClass const* c = _fdd->objectClass(objectClass); c;
        c = c->parent ? _fdd->objectClass(c->parent) : nullptr)
      if (c->handle == ancestor)
         return true;
   return false;
}

std::uint64_t MqttRTIambassador::knownObjectClass(std::uint64_t objectClass) const
{
   for (Fdd::ObjectClass const* c = _fdd->objectClass(objectClass); c;
        c = c->parent ? _fdd->objectClass(c->parent) : nullptr)
   {
      auto it = _subscribedAttributes.find(c->handle);
      if (it != _subscribedAttributes.end() && !it->second.empty())
         return c->handle;
   }
   return 0;
}

std::uint64_t MqttRTIambassador::receivedInteractionClass(std::uint64_t interaction) const
{
   for (Fdd::InteractionClass const* c = _fdd->interactionClass(interaction); c;
        c = c->parent ? _fdd->interactionClass(c->parent) : nullptr)
      if (_subscribedInteractions.count(c->handle))
         return c->handle;
   return 0;
}

bool MqttRTIambassador::seenBefore(std::uint64_t sender, std::uint64_t seq)
{
   auto key = std::make_pair(sender, seq);
   if (!_recent.insert(key).second)
      return true;
   _recentOrder.push_back(key);
   if (_recentOrder.size() > 4096)
   {
      _recent.erase(_recentOrder.front());
      _recentOrder.pop_front();
   }
   return false;
}

std::string MqttRTIambassador::encodeTime(LogicalTime const& theTime) const
{
   if (theTime.implementationName() != _timeImplementation)
      throw InvalidLogicalTime(L"time of type " + theTime.implementationName() + L", the federation uses " +
                               _timeImplementation);
   VariableLengthData d = theTime.encode();
   return tagBytes(d);
}

std::shared_ptr<LogicalTime> MqttRTIambassador::decodeTime(std::string const& encoded) const
{
   RTI_UNIQUE_PTR<LogicalTimeFactory> f = HLAlogicalTimeFactoryFactory::makeLogicalTimeFactory(_timeImplementation);
   if (!f.get())
      return std::shared_ptr<LogicalTime>();
   return std::shared_ptr<LogicalTime>(f->decodeLogicalTime(toVld(encoded)).release());
}

void MqttRTIambassador::subscribeJoinedTopics()
{
   std::string const ft = _fedTopic;
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      _syncPoints.clear();
      resetTime();
      _save = FederationSave();
      _restore = FederationRestore();
      _restoring = false;
      _saveBusy = false;
      _restoreBusy = false;
   }
   {
      // Advisory switches: FOM <switches>, disabled when absent.
      std::lock_guard<std::mutex> v(_viewMutex);
      _advisorySwitches[0] = _fdd->switchEnabled("objectClassRelevanceAdvisory", false);
      _advisorySwitches[1] = _fdd->switchEnabled("attributeRelevanceAdvisory", false);
      _advisorySwitches[2] = _fdd->switchEnabled("attributeScopeAdvisory", false);
      _advisorySwitches[3] = _fdd->switchEnabled("interactionRelevanceAdvisory", false);
   }
   _transport->subscribe(ft + "/decl/+");
   _transport->subscribe(ft + "/sync/#");
   _transport->subscribe(ft + "/time/+");
   _transport->subscribe(ft + "/execution");
   _transport->subscribe(ft + "/fom/+");
   _transport->subscribe(ft + "/federates/+");
   _transport->subscribe(ft + "/requests/" + hex64(_federateHandle));
   _transport->subscribe(ft + "/classrequests/#");
   _transport->subscribe(ft + "/owners/#");
   _transport->subscribe(ft + "/ownership/" + hex64(_federateHandle));
   _transport->subscribe(ft + "/ownership/all");
   _transport->subscribe(ft + "/saves/#");
   _transport->subscribe(ft + "/save/#");
   _transport->subscribe(ft + "/restore/#");
}

void MqttRTIambassador::unsubscribeJoinedTopics() noexcept
{
   std::string const ft = _fedTopic;
   for (char const* t : {"/execution", "/fom/+", "/federates/+", "/classrequests/#", "/sync/#", "/time/+", "/decl/+",
                         "/owners/#", "/ownership/all", "/saves/#", "/save/#", "/restore/#"})
      _transport->unsubscribe(ft + t);
   _transport->unsubscribe(ft + "/requests/" + hex64(_federateHandle));
   _transport->unsubscribe(ft + "/ownership/" + hex64(_federateHandle));
   for (std::string const& t : _coverTopics)
      _transport->unsubscribe(t);
   _coverTopics.clear();
}

void MqttRTIambassador::resetObjectState()
{
   _objects.clear();
   _ownerRecords.clear();
   _pendingMarks.clear();
   _publishedAttributes.clear();
   _subscribedAttributes.clear();
   _publishedInteractions.clear();
   _subscribedInteractions.clear();
   _reservedNames.clear();
   _recent.clear();
   _recentOrder.clear();
   _remoteDecl.clear();
   _classRelevanceOn.clear();
   _interactionRelevanceOn.clear();
   _lastDeclarations.clear();
   _regions.clear();
   _plainSubscribedAttributes.clear();
   _regionSubscribedAttributes.clear();
   _plainSubscribedInteractions.clear();
   _regionSubscribedInteractions.clear();
   _subscribedRates.clear();
   _interactionOrder.clear();
   _interactionTransport.clear();
}

void MqttRTIambassador::updateBrokerSubscriptions()
{
   std::set<std::string> wanted;
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      for (auto const& kv : _subscribedAttributes)
      {
         if (kv.second.empty())
            continue;
         bool covered = false;
         Fdd::ObjectClass const* c = _fdd->objectClass(kv.first);
         for (std::uint64_t p = c->parent; p != 0 && !covered; p = _fdd->objectClass(p)->parent)
         {
            auto it = _subscribedAttributes.find(p);
            covered = it != _subscribedAttributes.end() && !it->second.empty();
         }
         if (!covered)
            wanted.insert(_fedTopic + "/objects/" + classTopicPath(c->qualified) + "/#");
      }
      for (std::uint64_t h : _subscribedInteractions)
      {
         bool covered = false;
         Fdd::InteractionClass const* c = _fdd->interactionClass(h);
         for (std::uint64_t p = c->parent; p != 0 && !covered; p = _fdd->interactionClass(p)->parent)
            covered = _subscribedInteractions.count(p) != 0;
         if (!covered)
            wanted.insert(_fedTopic + "/interactions/" + classTopicPath(c->qualified) + "/#");
      }
      for (std::string const& t : momTopics()) // MOM requests, handled whatever the subscriptions
         wanted.insert(t);
   }
   try
   {
      for (std::string const& t : wanted) // new ones first: no gap in the traffic
         if (!_coverTopics.count(t))
            _transport->subscribe(t);
   }
   catch (std::runtime_error const& e)
   {
      throw RTIinternalError(fromUtf8(e.what()));
   }
   for (std::string const& t : _coverTopics)
      if (!wanted.count(t))
         _transport->unsubscribe(t);
   _coverTopics = wanted;
   std::lock_guard<std::mutex> v(_viewMutex);
   discoverPending();
   evaluateScopes();
   publishDeclarations();
}

// ============================================================ incoming traffic
void MqttRTIambassador::tryDiscover(ObjectInstance& o)
{
   if (o.knownClass != 0 || _restoring || staleObject(o.handle))
      return;
   std::uint64_t const k = knownObjectClass(o.objectClass);
   if (k == 0)
      return;
   o.knownClass = k;
   o.locallyDeleted = false;
   ObjectInstanceHandle const instance = ObjectInstanceHandleFriend::create(o.handle);
   ObjectClassHandle const known = ObjectClassHandleFriend::create(k);
   std::wstring const name = fromUtf8(o.name);
   enqueue([instance, known, name](FederateAmbassador& fa) { fa.discoverObjectInstance(instance, known, name); });
   evaluateScope(o);
}

void MqttRTIambassador::discoverPending()
{
   for (auto& kv : _objects)
      if (!kv.second.locallyDeleted)
         tryDiscover(kv.second);
}

bool MqttRTIambassador::handlePresence(std::string const& clientId, std::string const& payload)
{
   bool online = false;
   if (!payload.empty())
   {
      try
      {
         online = Json::parse(payload)["state"].str() == "online";
      }
      catch (std::exception const&)
      {
      }
   }
   // Only the transition online -> offline/absent counts (a Last Will, then the
   // removal of the stale presence, must not report the loss twice).
   if (online || !clientAlive(clientId) || clientId == _transport->settings().clientId)
      return false;
   HLA_MQTT_LOG(Info, _transport->settings().clientId, "presence lost: " << clientId);
   momFederateLost(clientId); // HLAreportFederateLost, before the records go
   return true; // then ownershipPresenceLost, once the presence is recorded offline
}

void MqttRTIambassador::handleObjectTraffic(std::string const& rest, std::string const& payload)
{
   try
   {
      // ------------------------------------------------ info (retained)
      if (startsWith(rest, "objects/") && rest.size() > 5 && rest.compare(rest.size() - 5, 5, "/info") == 0)
      {
         size_t const at = rest.rfind("/@");
         std::uint64_t const instance = at == std::string::npos ? 0 : parseHex(rest.substr(at + 2));
         if (payload.empty()) // info cleared: deleted (normally after a delete message)
         {
            if (_restoring)
               return; // cleared by its holder for the restore (see MqttSaveRestore.cpp)
            auto it = _objects.find(instance);
            if (it != _objects.end() && !it->second.mom && !it->second.owned.count(privilegeAttribute()))
            {
               if (it->second.knownClass != 0)
               {
                  ++_momRemoved;
                  ObjectInstanceHandle const h = ObjectInstanceHandleFriend::create(instance);
                  enqueue([h](FederateAmbassador& fa) {
                     fa.removeObjectInstance(h, VariableLengthData(), RECEIVE, SupplementalRemoveInfo());
                  });
               }
               _objects.erase(it);
            }
            return;
         }
         Json j = Json::parse(payload);
         ObjectInstance o;
         o.handle = parseHex(j["instance"].str());
         o.objectClass = parseHex(j["class"].str());
         o.name = j["name"].str();
         o.registrant = parseHex(j["registrant"].str());
         o.topic = _fedTopic + "/" + rest.substr(0, rest.size() - 5);
         if (!_fdd->objectClass(o.objectClass))
            return;
         auto it = _objects.find(o.handle);
         if (it != _objects.end() && it->second.mom)
            return; // our own HLAfederate
         if (it == _objects.end())
         {
            if (o.registrant == _federateHandle || staleObject(o.handle))
               return; // ours (already deleted), or stale record of a vanished federate
            it = _objects.emplace(o.handle, o).first;
         }
         if (!it->second.locallyDeleted)
            tryDiscover(it->second);
         return;
      }

      if (_restoring)
         return; // updates, interactions, requests and deletions of the restoring federation
      WireReader r(payload);
      std::uint64_t const sender = r.u64();
      std::uint64_t const seq = r.u64();
      if (sender == _federateHandle || seenBefore(sender, seq))
         return;

      switch (r.kind())
      {
      case WireKind::Update:
      {
         std::uint64_t const instance = r.u64();
         std::string const tag = r.bytes();
         unsigned const timeFlag = r.u8(); // 0 none, 1 timestamp (RO), 2 TSO
         std::string const time = timeFlag ? r.bytes() : std::string();
         std::uint64_t const retraction = r.u64();
         std::uint32_t const n = r.u32();
         auto it = _objects.find(instance);
         if (it == _objects.end())
            return;
         ObjectInstance& o = it->second;
         if (o.knownClass == 0)
            tryDiscover(o); // locally deleted object: rediscovered by its next update
         if (o.knownClass == 0)
            return;
         std::set<std::uint64_t> const& wanted = _subscribedAttributes[o.knownClass];
         AttributeHandleValueMap values;
         bool allBestEffort = true;
         for (std::uint32_t k = 0; k < n; ++k)
         {
            std::uint64_t const a = r.u64();
            std::string const value = r.bytes();
            if (!wanted.count(a) || !subscriptionCovers(o.knownClass, a, updateExtents(o, a)))
               continue; // not subscribed, or no overlap of the regions
            values[AttributeHandleFriend::create(a)] = toVld(value);
            allBestEffort = allBestEffort && bestEffort(o, a);
         }
         if (values.empty())
            return;
         momCountReflection(o.knownClass, instance, allBestEffort);
         ObjectInstanceHandle const h = ObjectInstanceHandleFriend::create(instance);
         VariableLengthData const t = toVld(tag);
         TransportationType const tt = allBestEffort ? BEST_EFFORT : RELIABLE;
         SupplementalReflectInfo const info = reflectInfo(sender);
         std::shared_ptr<LogicalTime> lt = timeFlag ? decodeTime(time) : nullptr;
         if (lt && timeFlag == 2)
         {
            MessageRetractionHandle const mrh = MessageRetractionHandleFriend::create(retraction);
            deliverTimed(TimeValue::of(*lt, timeIsInteger()), true,
                         [h, values, t, tt, lt, mrh, info](FederateAmbassador& fa, OrderType received) {
                            fa.reflectAttributeValues(h, values, t, TIMESTAMP, tt, *lt, received, mrh, info);
                         },
                         retraction);
         }
         else if (lt)
            deliverRO([h, values, t, tt, lt, info](FederateAmbassador& fa) {
               fa.reflectAttributeValues(h, values, t, RECEIVE, tt, *lt, RECEIVE, info);
            });
         else
            deliverRO([h, values, t, tt, info](FederateAmbassador& fa) {
               fa.reflectAttributeValues(h, values, t, RECEIVE, tt, info);
            });
         return;
      }
      case WireKind::Interaction:
      {
         std::uint64_t const cls = r.u64();
         std::string const tag = r.bytes();
         unsigned const timeFlag = r.u8();
         std::string const time = timeFlag ? r.bytes() : std::string();
         std::uint64_t const retraction = r.u64();
         std::uint32_t const n = r.u32();
         std::map<std::uint64_t, std::string> raw;
         for (std::uint32_t k = 0; k < n; ++k)
         {
            std::uint64_t const p = r.u64();
            raw[p] = r.bytes();
         }
         if (momHandles(cls))
            handleMomInteraction(cls, raw); // MOM request, adjustment or service
         std::uint64_t const received = receivedInteractionClass(cls);
         if (received == 0)
            return;
         ParameterHandleValueMap values;
         for (auto const& kv : raw)
            if (_fdd->classHasParameter(received, kv.first))
               values[ParameterHandleFriend::create(kv.first)] = toVld(kv.second);
         std::vector<Extents> const sentRegions = r.atEnd() ? std::vector<Extents>() : readExtents(r);
         if (!interactionCovers(received, sentRegions))
            return; // no overlap with the subscription regions
         if (!_fdd->interactionClass(cls))
            return;
         TransportationType const tt = interactionBestEffort(sender, cls) ? BEST_EFFORT : RELIABLE;
         momCountInteraction(false, received, tt == BEST_EFFORT);
         InteractionClassHandle const h = InteractionClassHandleFriend::create(received);
         VariableLengthData const t = toVld(tag);
         SupplementalReceiveInfo const info = receiveInfo(sender);
         std::shared_ptr<LogicalTime> lt = timeFlag ? decodeTime(time) : nullptr;
         if (lt && timeFlag == 2)
         {
            MessageRetractionHandle const mrh = MessageRetractionHandleFriend::create(retraction);
            deliverTimed(TimeValue::of(*lt, timeIsInteger()), true,
                         [h, values, t, tt, lt, mrh, info](FederateAmbassador& fa, OrderType receivedOrder) {
                            fa.receiveInteraction(h, values, t, TIMESTAMP, tt, *lt, receivedOrder, mrh, info);
                         },
                         retraction);
         }
         else if (lt)
            deliverRO([h, values, t, tt, lt, info](FederateAmbassador& fa) {
               fa.receiveInteraction(h, values, t, RECEIVE, tt, *lt, RECEIVE, info);
            });
         else
            deliverRO([h, values, t, tt, info](FederateAmbassador& fa) {
               fa.receiveInteraction(h, values, t, RECEIVE, tt, info);
            });
         return;
      }
      case WireKind::Delete:
      {
         std::uint64_t const instance = r.u64();
         std::string const tag = r.bytes();
         unsigned const timeFlag = r.u8();
         std::string const time = timeFlag ? r.bytes() : std::string();
         auto it = _objects.find(instance);
         if (it == _objects.end())
            return;
         bool const discovered = it->second.knownClass != 0;
         _objects.erase(it);
         if (!discovered)
            return;
         ++_momRemoved;
         ObjectInstanceHandle const h = ObjectInstanceHandleFriend::create(instance);
         VariableLengthData const t = toVld(tag);
         std::shared_ptr<LogicalTime> lt = timeFlag ? decodeTime(time) : nullptr;
         if (lt && timeFlag == 2)
            deliverTimed(TimeValue::of(*lt, timeIsInteger()), true,
                         [h, t, lt](FederateAmbassador& fa, OrderType receivedOrder) {
                            fa.removeObjectInstance(h, t, TIMESTAMP, *lt, receivedOrder, SupplementalRemoveInfo());
                         });
         else if (lt)
            deliverRO([h, t, lt](FederateAmbassador& fa) {
               fa.removeObjectInstance(h, t, RECEIVE, *lt, RECEIVE, SupplementalRemoveInfo());
            });
         else
            deliverRO([h, t](FederateAmbassador& fa) { fa.removeObjectInstance(h, t, RECEIVE, SupplementalRemoveInfo()); });
         return;
      }
      case WireKind::Retract:
         handleRetraction(r.u64());
         return;
      case WireKind::Request:
      {
         std::uint64_t const instance = r.u64();
         std::uint64_t const cls = r.u64();
         std::string const tag = r.bytes();
         std::uint32_t const n = r.u32();
         std::set<std::uint64_t> attrs;
         for (std::uint32_t k = 0; k < n; ++k)
            attrs.insert(r.u64());
         // 9.13: (attributes, extents) pairs; an attribute is provided when its
         // update regions overlap the regions of a pair naming it.
         std::vector<std::pair<std::set<std::uint64_t>, std::vector<Extents>>> pairs;
         if (!r.atEnd())
         {
            std::uint32_t const count = r.u32();
            for (std::uint32_t k = 0; k < count && k < 4096; ++k)
            {
               std::set<std::uint64_t> pa;
               std::uint32_t const m = r.u32();
               for (std::uint32_t q = 0; q < m; ++q)
                  pa.insert(r.u64());
               pairs.emplace_back(pa, readExtents(r));
            }
         }
         VariableLengthData const t = toVld(tag);
         auto mom = _objects.find(_momFederateObject); // our HLAfederate: values provided by the RTI
         if (mom != _objects.end() &&
             (instance != 0 ? instance == mom->first : isSubclassOf(mom->second.objectClass, cls)))
            momSendFederateUpdate(attrs);
         for (auto const& kv : _objects)
         {
            ObjectInstance const& o = kv.second;
            if (o.owned.empty())
               continue;
            if (instance != 0 ? o.handle != instance : !isSubclassOf(o.objectClass, cls))
               continue;
            AttributeHandleSet provide;
            for (std::uint64_t a : attrs)
            {
               if (!o.owned.count(a))
                  continue;
               bool inRegion = pairs.empty();
               for (auto const& pair : pairs)
                  inRegion = inRegion || (pair.first.count(a) && anyOverlap(updateExtents(o, a), pair.second));
               if (inRegion)
                  provide.insert(AttributeHandleFriend::create(a));
            }
            if (provide.empty())
               continue;
            ObjectInstanceHandle const h = ObjectInstanceHandleFriend::create(o.handle);
            enqueue([h, provide, t](FederateAmbassador& fa) { fa.provideAttributeValueUpdate(h, provide, t); });
         }
         return;
      }
      }
   }
   catch (std::exception const&)
   {
      // Malformed message from another federate: ignored.
   }
   catch (Exception const&)
   {
      // Undecodable time: ignored.
   }
}

// ================================================= 5 Declaration management
namespace
{
template <class Fn>
void forEachAttribute(AttributeHandleSet const& attrs, Fn fn)
{
   for (AttributeHandle const& a : attrs)
      fn(AttributeHandleFriend::value(a), a);
}
} // namespace

#define HLA_MQTT_CHECK_OBJECT_CLASS(cls)                                                     \
   std::uint64_t const cls##Value = ObjectClassHandleFriend::value(cls);                     \
   if (!_fdd->objectClass(cls##Value))                                                       \
      throw ObjectClassNotDefined(cls.toString());

#define HLA_MQTT_CHECK_ATTRIBUTES(clsValue, attrs)                                           \
   forEachAttribute(attrs, [&](std::uint64_t av, AttributeHandle const& ah) {                \
      if (!_fdd->classHasAttribute(clsValue, av))                                            \
         throw AttributeNotDefined(ah.toString());                                           \
   });

// 5.2
void MqttRTIambassador::publishObjectClassAttributes(ObjectClassHandle theClass, AttributeHandleSet const& attributeList)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   std::lock_guard<std::mutex> v(_viewMutex);
   HLA_MQTT_CHECK_OBJECT_CLASS(theClass)
   HLA_MQTT_CHECK_ATTRIBUTES(theClassValue, attributeList)
   forEachAttribute(attributeList,
                    [&](std::uint64_t av, AttributeHandle const&) { _publishedAttributes[theClassValue].insert(av); });
   evaluateAdvisories();
}

// 5.3
void MqttRTIambassador::unpublishObjectClass(ObjectClassHandle theClass)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   std::lock_guard<std::mutex> v(_viewMutex);
   HLA_MQTT_CHECK_OBJECT_CLASS(theClass)
   _publishedAttributes.erase(theClassValue);
   evaluateAdvisories();
}

void MqttRTIambassador::unpublishObjectClassAttributes(ObjectClassHandle theClass,
                                                       AttributeHandleSet const& attributeList)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   std::lock_guard<std::mutex> v(_viewMutex);
   HLA_MQTT_CHECK_OBJECT_CLASS(theClass)
   HLA_MQTT_CHECK_ATTRIBUTES(theClassValue, attributeList)
   auto it = _publishedAttributes.find(theClassValue);
   if (it == _publishedAttributes.end())
      return;
   forEachAttribute(attributeList, [&](std::uint64_t av, AttributeHandle const&) { it->second.erase(av); });
   if (it->second.empty())
      _publishedAttributes.erase(it);
   evaluateAdvisories();
}

// 5.4
void MqttRTIambassador::publishInteractionClass(InteractionClassHandle theInteraction)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   std::lock_guard<std::mutex> v(_viewMutex);
   std::uint64_t const c = InteractionClassHandleFriend::value(theInteraction);
   if (!_fdd->interactionClass(c))
      throw InteractionClassNotDefined(theInteraction.toString());
   _publishedInteractions.insert(c);
   evaluateAdvisories();
}

// 5.5
void MqttRTIambassador::unpublishInteractionClass(InteractionClassHandle theInteraction)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   std::lock_guard<std::mutex> v(_viewMutex);
   std::uint64_t const c = InteractionClassHandleFriend::value(theInteraction);
   if (!_fdd->interactionClass(c))
      throw InteractionClassNotDefined(theInteraction.toString());
   _publishedInteractions.erase(c);
   evaluateAdvisories();
}

// 5.6
void MqttRTIambassador::subscribeObjectClassAttributes(ObjectClassHandle theClass,
                                                       AttributeHandleSet const& attributeList, bool /*active*/,
                                                       std::wstring const& updateRateDesignator)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      HLA_MQTT_CHECK_OBJECT_CLASS(theClass)
      HLA_MQTT_CHECK_ATTRIBUTES(theClassValue, attributeList)
      double rate = 0;
      if (!updateRateDesignator.empty() && !_fdd->updateRate(toUtf8(updateRateDesignator), rate))
         throw InvalidUpdateRateDesignator(updateRateDesignator);
      forEachAttribute(attributeList, [&](std::uint64_t av, AttributeHandle const&) {
         _plainSubscribedAttributes[theClassValue].insert(av);
      });
      setSubscribedRates(theClassValue, attributeList, rate);
      recomputeSubscriptions();
   }
   updateBrokerSubscriptions();
}

// 5.7
void MqttRTIambassador::unsubscribeObjectClass(ObjectClassHandle theClass)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      HLA_MQTT_CHECK_OBJECT_CLASS(theClass)
      _plainSubscribedAttributes.erase(theClassValue); // subscriptions with regions stay (9.9)
      recomputeSubscriptions();
   }
   updateBrokerSubscriptions();
}

void MqttRTIambassador::unsubscribeObjectClassAttributes(ObjectClassHandle theClass,
                                                         AttributeHandleSet const& attributeList)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      HLA_MQTT_CHECK_OBJECT_CLASS(theClass)
      HLA_MQTT_CHECK_ATTRIBUTES(theClassValue, attributeList)
      auto it = _plainSubscribedAttributes.find(theClassValue);
      if (it != _plainSubscribedAttributes.end())
      {
         forEachAttribute(attributeList, [&](std::uint64_t av, AttributeHandle const&) { it->second.erase(av); });
         if (it->second.empty())
            _plainSubscribedAttributes.erase(it);
      }
      recomputeSubscriptions();
   }
   updateBrokerSubscriptions();
}

// 5.8
void MqttRTIambassador::subscribeInteractionClass(InteractionClassHandle theClass, bool /*active*/)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      std::uint64_t const c = InteractionClassHandleFriend::value(theClass);
      if (!_fdd->interactionClass(c))
         throw InteractionClassNotDefined(theClass.toString());
      _plainSubscribedInteractions.insert(c);
      recomputeSubscriptions();
   }
   updateBrokerSubscriptions();
}

// 5.9
void MqttRTIambassador::unsubscribeInteractionClass(InteractionClassHandle theClass)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      std::uint64_t const c = InteractionClassHandleFriend::value(theClass);
      if (!_fdd->interactionClass(c))
         throw InteractionClassNotDefined(theClass.toString());
      _plainSubscribedInteractions.erase(c);
      recomputeSubscriptions();
   }
   updateBrokerSubscriptions();
}

// ===================================================== 6 Object management
// 6.2 to 6.7: object instance names, reserved federation-wide with the same
// claim + barrier scheme as federate names (F/names/<name>/<clientId>).
void MqttRTIambassador::reserveNames(std::set<std::wstring> const& names, bool multiple)
{
   for (std::wstring const& n : names)
      if (n.empty() || n.compare(0, 3, L"HLA") == 0)
         throw IllegalName(L"illegal object instance name '" + n + L"'");

   std::string const ft = _fedTopic;
   std::string const myId = _transport->settings().clientId;
   std::vector<std::string> topics;
   for (std::wstring const& n : names)
      topics.push_back(ft + "/names/" + encodeTopicLevel(toUtf8(n)));

   auto claim = [&](std::string const& state) {
      Json j = Json::object();
      j.set("clientId", myId);
      j.set("federate", hex64(_federateHandle));
      j.set("state", state);
      for (std::string const& t : topics)
         _transport->publish(t + "/" + encodeTopicLevel(myId), j.dump(), 1, true);
   };
   auto withdraw = [&] {
      for (std::string const& t : topics)
         _transport->publish(t + "/" + encodeTopicLevel(myId), std::string(), 1, true);
   };
   // Others: 0 = free, 1 = being reserved by someone else, 2 = reserved.
   auto others = [&]() {
      int worst = 0;
      std::lock_guard<std::mutex> v(_viewMutex);
      for (std::string const& t : topics)
         for (auto const& kv : recordsUnder(t + "/"))
         {
            if (kv.first == t + "/" + encodeTopicLevel(myId))
               continue;
            try
            {
               Json j = Json::parse(kv.second);
               if (!clientAlive(j["clientId"].str()))
                  continue;
               worst = std::max(worst, j["state"].str() == "reserved" ? 2 : 1);
            }
            catch (std::exception const&)
            {
            }
         }
      return worst;
   };

   bool success = false;
   try
   {
      for (std::string const& t : topics)
         _transport->subscribe(t + "/+");
      for (int attempt = 0; attempt < kReservationAttempts; ++attempt)
      {
         _transport->barrier();
         if (others() == 2)
            break;
         claim("reserving");
         _transport->barrier();
         if (others() == 0)
         {
            claim("reserved");
            success = true;
            break;
         }
         withdraw();
         backoff(attempt);
      }
   }
   catch (std::runtime_error const& e)
   {
      throw RTIinternalError(fromUtf8(e.what()));
   }
   for (std::string const& t : topics)
   {
      _transport->unsubscribe(t + "/+");
      std::lock_guard<std::mutex> v(_viewMutex);
      forgetUnder(t + "/");
   }
   if (success)
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      _reservedNames.insert(names.begin(), names.end());
   }
   if (multiple)
   {
      if (success)
         enqueue([names](FederateAmbassador& fa) { fa.multipleObjectInstanceNameReservationSucceeded(names); });
      else
         enqueue([names](FederateAmbassador& fa) { fa.multipleObjectInstanceNameReservationFailed(names); });
   }
   else
   {
      std::wstring const n = *names.begin();
      if (success)
         enqueue([n](FederateAmbassador& fa) { fa.objectInstanceNameReservationSucceeded(n); });
      else
         enqueue([n](FederateAmbassador& fa) { fa.objectInstanceNameReservationFailed(n); });
   }
}

void MqttRTIambassador::reserveObjectInstanceName(std::wstring const& theObjectInstanceName)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   reserveNames(std::set<std::wstring>{theObjectInstanceName}, false);
}

void MqttRTIambassador::reserveMultipleObjectInstanceName(std::set<std::wstring> const& theObjectInstanceNames)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   if (theObjectInstanceNames.empty())
      throw IllegalName(L"empty set of names");
   reserveNames(theObjectInstanceNames, true);
}

void MqttRTIambassador::releaseObjectInstanceName(std::wstring const& theObjectInstanceName)
{
   releaseMultipleObjectInstanceName(std::set<std::wstring>{theObjectInstanceName});
}

void MqttRTIambassador::releaseMultipleObjectInstanceName(std::set<std::wstring> const& theObjectInstanceNames)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      for (std::wstring const& n : theObjectInstanceNames)
         if (!_reservedNames.count(n))
            throw ObjectInstanceNameNotReserved(n);
      for (std::wstring const& n : theObjectInstanceNames)
         _reservedNames.erase(n);
   }
   std::string const myId = encodeTopicLevel(_transport->settings().clientId);
   for (std::wstring const& n : theObjectInstanceNames)
      _transport->publishAsync(_fedTopic + "/names/" + encodeTopicLevel(toUtf8(n)) + "/" + myId, std::string(), 1,
                               true);
}

// 6.8
ObjectInstanceHandle MqttRTIambassador::registerObjectInstance(ObjectClassHandle theClass)
{
   return registerObjectInstance(theClass, std::wstring());
}

ObjectInstanceHandle MqttRTIambassador::registerObjectInstance(ObjectClassHandle theClass,
                                                               std::wstring const& theObjectInstanceName)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   return registerObject(theClass, theObjectInstanceName, nullptr);
}

// _stateMutex held. `regions`: 9.5 registerObjectInstanceWithRegions.
ObjectInstanceHandle MqttRTIambassador::registerObject(ObjectClassHandle theClass,
                                                       std::wstring const& theObjectInstanceName,
                                                       AttributeHandleSetRegionHandleSetPairVector const* regions)
{
   ObjectInstance o;
   std::string infoPayload;
   std::pair<std::string, std::string> record;
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      HLA_MQTT_CHECK_OBJECT_CLASS(theClass)
      auto pub = _publishedAttributes.find(theClassValue);
      if (pub == _publishedAttributes.end() || pub->second.empty())
         throw ObjectClassNotPublished(theClass.toString());
      if (regions)
      {
         for (auto const& pair : *regions)
            forEachAttribute(pair.first, [&](std::uint64_t av, AttributeHandle const& ah) {
               if (!_fdd->classHasAttribute(theClassValue, av))
                  throw AttributeNotDefined(ah.toString());
               if (!pub->second.count(av))
                  throw AttributeNotPublished(ah.toString());
            });
         checkRegionPairs(theClassValue, *regions);
      }
      o.handle = newUniqueValue("O");
      if (!theObjectInstanceName.empty())
      {
         if (!_reservedNames.count(theObjectInstanceName))
            throw ObjectInstanceNameNotReserved(theObjectInstanceName);
         for (auto const& kv : _objects)
            if (kv.second.registered && kv.second.name == toUtf8(theObjectInstanceName))
               throw ObjectInstanceNameInUse(theObjectInstanceName);
         o.name = toUtf8(theObjectInstanceName);
      }
      else
         o.name = "HLAobject-" + hex64(o.handle);
      o.objectClass = theClassValue;
      o.knownClass = theClassValue;
      o.registered = true;
      o.registrant = _federateHandle;
      o.topic = _fedTopic + "/objects/" + classTopicPath(_fdd->objectClass(theClassValue)->qualified) + "/@" +
                hex64(o.handle);
      o.owned = pub->second;
      if (Fdd::Attribute const* priv = _fdd->findAttribute(theClassValue, "HLAprivilegeToDeleteObject"))
         o.owned.insert(priv->handle);
      if (regions)
         for (auto const& pair : *regions)
            forEachAttribute(pair.first, [&](std::uint64_t av, AttributeHandle const&) {
               for (RegionHandle const& r : pair.second)
                  o.updateRegions[av].insert(RegionHandleFriend::value(r));
            });
      o.recordsInFlight = 1;
      _objects[o.handle] = o;
      record = ownerRecordEntry(o);
      infoPayload = objectInfo(o);
      evaluateAdvisories();
   }
   try
   {
      _transport->publish(record.first, record.second, 1, true); // owner first: never seen unowned
      _transport->publish(o.topic + "/info", infoPayload, 1, true);
   }
   catch (std::runtime_error const& e)
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      _objects.erase(o.handle);
      throw RTIinternalError(fromUtf8(e.what()));
   }
   return ObjectInstanceHandleFriend::create(o.handle);
}

// 6.10
namespace
{
MessageRetractionHandle sendUpdate(MqttTransport& transport, std::string const& topic, WireWriter const& w, int qos)
{
   try
   {
      transport.publishAsync(topic, w.str(), qos, false);
   }
   catch (std::runtime_error const& e)
   {
      throw RTIinternalError(fromUtf8(e.what()));
   }
   return MessageRetractionHandle();
}
} // namespace

void MqttRTIambassador::updateAttributeValues(ObjectInstanceHandle theObject,
                                              AttributeHandleValueMap const& theAttributeValues,
                                              VariableLengthData const& theUserSuppliedTag)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   std::string topic;
   int qos = 0;
   WireWriter w(WireKind::Update);
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      auto it = _objects.find(ObjectInstanceHandleFriend::value(theObject));
      if (it == _objects.end())
         throw ObjectInstanceNotKnown(theObject.toString());
      ObjectInstance const& o = it->second;
      for (auto const& kv : theAttributeValues)
      {
         std::uint64_t const a = AttributeHandleFriend::value(kv.first);
         if (!_fdd->classHasAttribute(o.objectClass, a))
            throw AttributeNotDefined(kv.first.toString());
         if (!o.owned.count(a))
            throw AttributeNotOwned(kv.first.toString());
         if (!bestEffort(o, a))
            qos = 1;
      }
      topic = o.topic + "/update";
      w.u64(_federateHandle).u64(nextSeq()).u64(o.handle);
      momCountUpdate(o.objectClass, o.handle, qos == 0);
   }
   w.bytes(theUserSuppliedTag.data(), theUserSuppliedTag.size()).u8(0).u64(0);
   w.u32(std::uint32_t(theAttributeValues.size()));
   for (auto const& kv : theAttributeValues)
      w.u64(AttributeHandleFriend::value(kv.first)).bytes(kv.second.data(), kv.second.size());
   sendUpdate(*_transport, topic, w, qos);
}

MessageRetractionHandle MqttRTIambassador::updateAttributeValues(ObjectInstanceHandle theObject,
                                                                 AttributeHandleValueMap const& theAttributeValues,
                                                                 VariableLengthData const& theUserSuppliedTag,
                                                                 LogicalTime const& theTime)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   std::string const time = encodeTime(theTime);
   std::uint64_t const retraction = newUniqueValue("R");
   // Attributes with the TimeStamp order go TSO (if regulating), the others RO
   // with their timestamp: up to two messages. Published under _viewMutex so
   // that the time state publications stay after them (FIFO argument).
   std::lock_guard<std::mutex> v(_viewMutex);
   auto it = _objects.find(ObjectInstanceHandleFriend::value(theObject));
   if (it == _objects.end())
      throw ObjectInstanceNotKnown(theObject.toString());
   ObjectInstance const& o = it->second;
   TimeValue const ts = TimeValue::of(theTime, timeIsInteger());
   AttributeHandleValueMap parts[2]; // 0 RO, 1 TSO
   int qos[2] = {0, 0};
   for (auto const& kv : theAttributeValues)
   {
      std::uint64_t const a = AttributeHandleFriend::value(kv.first);
      if (!_fdd->classHasAttribute(o.objectClass, a))
         throw AttributeNotDefined(kv.first.toString());
      if (!o.owned.count(a))
         throw AttributeNotOwned(kv.first.toString());
      int const k = (_time.regulating && timestampOrder(o, a)) ? 1 : 0;
      parts[k][kv.first] = kv.second;
      if (!bestEffort(o, a) || k == 1)
         qos[k] = 1;
   }
   bool const tso = !parts[1].empty() && sendsTso(true, ts);
   for (int k = 0; k < 2; ++k)
   {
      if (parts[k].empty())
         continue;
      WireWriter w(WireKind::Update);
      w.u64(_federateHandle).u64(nextSeq()).u64(o.handle);
      w.bytes(theUserSuppliedTag.data(), theUserSuppliedTag.size()).u8(k == 1 && tso ? 2 : 1);
      w.bytes(time.data(), time.size()).u64(retraction);
      w.u32(std::uint32_t(parts[k].size()));
      for (auto const& kv : parts[k])
         w.u64(AttributeHandleFriend::value(kv.first)).bytes(kv.second.data(), kv.second.size());
      sendUpdate(*_transport, o.topic + "/update", w, qos[k]);
      momCountUpdate(o.objectClass, o.handle, qos[k] == 0);
      if (k == 1 && tso)
         rememberSentTso(retraction, ts, o.topic + "/update");
   }
   return MessageRetractionHandleFriend::create(retraction);
}

// 6.12
namespace
{
void writeParameters(WireWriter& w, ParameterHandleValueMap const& values)
{
   w.u32(std::uint32_t(values.size()));
   for (auto const& kv : values)
      w.u64(ParameterHandleFriend::value(kv.first)).bytes(kv.second.data(), kv.second.size());
}
} // namespace

void MqttRTIambassador::sendInteraction(InteractionClassHandle theInteraction,
                                        ParameterHandleValueMap const& theParameterValues,
                                        VariableLengthData const& theUserSuppliedTag)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   sendInteractionMessage(theInteraction, theParameterValues, theUserSuppliedTag, nullptr, nullptr);
}

MessageRetractionHandle MqttRTIambassador::sendInteraction(InteractionClassHandle theInteraction,
                                                           ParameterHandleValueMap const& theParameterValues,
                                                           VariableLengthData const& theUserSuppliedTag,
                                                           LogicalTime const& theTime)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   return sendInteractionMessage(theInteraction, theParameterValues, theUserSuppliedTag, &theTime, nullptr);
}

// Wire: sender, seq, class, tag, time flag [, time], retraction, parameters,
// then the extents of the regions (9.12; none = default region).
MessageRetractionHandle MqttRTIambassador::sendInteractionMessage(InteractionClassHandle theInteraction,
                                                                  ParameterHandleValueMap const& values,
                                                                  VariableLengthData const& tag,
                                                                  LogicalTime const* theTime,
                                                                  RegionHandleSet const* regions)
{
   std::string const time = theTime ? encodeTime(*theTime) : std::string();
   std::uint64_t const c = InteractionClassHandleFriend::value(theInteraction);
   std::uint64_t const retraction = theTime ? newUniqueValue("R") : 0;
   std::lock_guard<std::mutex> v(_viewMutex);
   Fdd::InteractionClass const* ic = _fdd->interactionClass(c);
   if (!ic)
      throw InteractionClassNotDefined(theInteraction.toString());
   if (!_publishedInteractions.count(c))
      throw InteractionClassNotPublished(theInteraction.toString());
   for (auto const& kv : values)
      if (!_fdd->classHasParameter(c, ParameterHandleFriend::value(kv.first)))
         throw InteractionParameterNotDefined(kv.first.toString());
   std::vector<Extents> sent;
   if (regions)
   {
      for (RegionHandle const& r : *regions)
         ownRegion(r);
      for (RegionHandle const& r : *regions)
         checkRegionContext(RegionHandleFriend::value(r), ic->dimensions);
      for (RegionHandle const& r : *regions)
         sent.push_back(_regions.at(RegionHandleFriend::value(r)).committed);
   }
   bool tso = false;
   TimeValue ts = TimeValue::zero(timeIsInteger());
   if (theTime)
   {
      ts = TimeValue::of(*theTime, timeIsInteger());
      tso = sendsTso(interactionTimestampOrder(c), ts);
   }
   WireWriter w(WireKind::Interaction);
   w.u64(_federateHandle).u64(nextSeq()).u64(c);
   w.bytes(tag.data(), tag.size()).u8(theTime ? (tso ? 2 : 1) : 0);
   if (theTime)
      w.bytes(time.data(), time.size());
   w.u64(retraction);
   writeParameters(w, values);
   writeExtents(w, sent);
   std::string const topic = _fedTopic + "/interactions/" + classTopicPath(ic->qualified) + "/@";
   bool const bestEffort = !tso && interactionBestEffort(_federateHandle, c);
   sendUpdate(*_transport, topic, w, bestEffort ? 0 : 1);
   momCountInteraction(true, c, bestEffort);
   if (tso)
      rememberSentTso(retraction, ts, topic);
   if (momHandles(c)) // a MOM request about ourselves: our own messages do not come back
   {
      std::map<std::uint64_t, std::string> raw;
      for (auto const& kv : values)
         raw[ParameterHandleFriend::value(kv.first)] = tagBytes(kv.second);
      handleMomInteraction(c, raw);
   }
   return theTime ? MessageRetractionHandleFriend::create(retraction) : MessageRetractionHandle();
}

// 6.14
void MqttRTIambassador::deleteOwnObject(ObjectInstance const& o, std::vector<std::string> const& records,
                                        VariableLengthData const& tag, std::string const& time, bool tso)
{
   WireWriter w(WireKind::Delete);
   w.u64(_federateHandle).u64(nextSeq()).u64(o.handle).bytes(tag.data(), tag.size());
   if (time.empty())
      w.u8(0);
   else
      w.u8(tso ? 2 : 1).bytes(time.data(), time.size());
   _transport->publish(o.topic + "/delete", w.str(), 1, false);
   _transport->publish(o.topic + "/info", std::string(), 1, true);
   for (std::string const& r : records) // co-owners not subscribed to the class learn the deletion here
      _transport->publish(r, std::string(), 1, true);
}

void MqttRTIambassador::deleteObjectInstance(ObjectInstanceHandle theObject, VariableLengthData const& theUserSuppliedTag)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   ObjectInstance o;
   std::vector<std::string> records;
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      auto it = _objects.find(ObjectInstanceHandleFriend::value(theObject));
      if (it == _objects.end() || it->second.knownClass == 0)
         throw ObjectInstanceNotKnown(theObject.toString());
      if (!it->second.owned.count(privilegeAttribute()))
         throw DeletePrivilegeNotHeld(theObject.toString());
      o = it->second;
      records = recordTopicsOf(o.handle);
      _objects.erase(it);
      _ownerRecords.erase(o.handle);
      ++_momDeleted;
   }
   try
   {
      deleteOwnObject(o, records, theUserSuppliedTag, std::string());
   }
   catch (std::runtime_error const& e)
   {
      throw RTIinternalError(fromUtf8(e.what()));
   }
}

MessageRetractionHandle MqttRTIambassador::deleteObjectInstance(ObjectInstanceHandle theObject,
                                                                VariableLengthData const& theUserSuppliedTag,
                                                                LogicalTime const& theTime)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   std::string const time = encodeTime(theTime);
   ObjectInstance o;
   std::vector<std::string> records;
   bool tso = false;
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      auto it = _objects.find(ObjectInstanceHandleFriend::value(theObject));
      if (it == _objects.end() || it->second.knownClass == 0)
         throw ObjectInstanceNotKnown(theObject.toString());
      std::uint64_t const priv = privilegeAttribute();
      if (!it->second.owned.count(priv))
         throw DeletePrivilegeNotHeld(theObject.toString());
      tso = sendsTso(timestampOrder(it->second, priv), TimeValue::of(theTime, timeIsInteger()));
      o = it->second;
      records = recordTopicsOf(o.handle);
      _objects.erase(it);
      _ownerRecords.erase(o.handle);
      ++_momDeleted;
   }
   try
   {
      deleteOwnObject(o, records, theUserSuppliedTag, time, tso);
   }
   catch (std::runtime_error const& e)
   {
      throw RTIinternalError(fromUtf8(e.what()));
   }
   return MessageRetractionHandleFriend::create(newUniqueValue("R"));
}

// 6.16
void MqttRTIambassador::localDeleteObjectInstance(ObjectInstanceHandle theObject)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   std::lock_guard<std::mutex> v(_viewMutex);
   auto it = _objects.find(ObjectInstanceHandleFriend::value(theObject));
   if (it == _objects.end() || it->second.knownClass == 0)
      throw ObjectInstanceNotKnown(theObject.toString());
   if (!it->second.owned.empty())
      throw FederateOwnsAttributes(theObject.toString());
   it->second.knownClass = 0;
   it->second.locallyDeleted = true;
   it->second.scope.clear();
}

// 6.19
void MqttRTIambassador::requestAttributeValueUpdate(ObjectInstanceHandle theObject,
                                                    AttributeHandleSet const& theAttributes,
                                                    VariableLengthData const& theUserSuppliedTag)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   std::map<std::uint64_t, std::vector<std::uint64_t>> byOwner; // the owners other than us
   std::uint64_t cls = 0;
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      auto it = _objects.find(ObjectInstanceHandleFriend::value(theObject));
      if (it == _objects.end() || it->second.knownClass == 0)
         throw ObjectInstanceNotKnown(theObject.toString());
      cls = it->second.objectClass;
      HLA_MQTT_CHECK_ATTRIBUTES(cls, theAttributes)
      if (it->second.mom) // our HLAfederate or the HLAfederation object
      {
         std::set<std::uint64_t> attrs;
         for (AttributeHandle const& a : theAttributes)
            attrs.insert(AttributeHandleFriend::value(a));
         momProvide(it->second, attrs);
         return;
      }
      for (AttributeHandle const& a : theAttributes)
      {
         std::uint64_t const owner = ownerOf(it->second, AttributeHandleFriend::value(a));
         if (owner != 0 && owner != _federateHandle) // unowned, or owned by the requester: nothing to provide
            byOwner[owner].push_back(AttributeHandleFriend::value(a));
      }
   }
   for (auto const& kv : byOwner)
   {
      WireWriter w(WireKind::Request);
      w.u64(_federateHandle).u64(nextSeq()).u64(ObjectInstanceHandleFriend::value(theObject)).u64(cls);
      w.bytes(theUserSuppliedTag.data(), theUserSuppliedTag.size()).u32(std::uint32_t(kv.second.size()));
      for (std::uint64_t a : kv.second)
         w.u64(a);
      sendUpdate(*_transport, _fedTopic + "/requests/" + hex64(kv.first), w, 1);
   }
}

void MqttRTIambassador::requestAttributeValueUpdate(ObjectClassHandle theClass, AttributeHandleSet const& theAttributes,
                                                    VariableLengthData const& theUserSuppliedTag)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   requestClassUpdate(theClass, theAttributes, nullptr, theUserSuppliedTag);
}

// Wire: sender, seq, 0, class, tag, attributes, then (9.13) the pairs:
// u32 count, per pair u32 attributes + handles and the extents of its regions.
void MqttRTIambassador::requestClassUpdate(ObjectClassHandle theClass, AttributeHandleSet const& attributes,
                                           AttributeHandleSetRegionHandleSetPairVector const* pairs,
                                           VariableLengthData const& tag)
{
   std::string topic;
   std::vector<std::vector<Extents>> extents;
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      HLA_MQTT_CHECK_OBJECT_CLASS(theClass)
      HLA_MQTT_CHECK_ATTRIBUTES(theClassValue, attributes)
      if (pairs)
      {
         checkRegionPairs(theClassValue, *pairs);
         for (auto const& pair : *pairs)
         {
            std::set<std::uint64_t> handles;
            for (RegionHandle const& r : pair.second)
               handles.insert(RegionHandleFriend::value(r));
            extents.push_back(ownExtents(handles));
         }
      }
      topic = _fedTopic + "/classrequests/" + classTopicPath(_fdd->objectClass(theClassValue)->qualified) + "/@";
      std::set<std::uint64_t> attrs;
      for (AttributeHandle const& a : attributes)
         attrs.insert(AttributeHandleFriend::value(a));
      for (auto const& kv : _objects) // local MOM entries: our own request does not come back
         if (kv.second.mom && isSubclassOf(kv.second.objectClass, theClassValue))
            momProvide(kv.second, attrs);
   }
   WireWriter w(WireKind::Request);
   w.u64(_federateHandle).u64(nextSeq()).u64(0).u64(ObjectClassHandleFriend::value(theClass));
   w.bytes(tag.data(), tag.size()).u32(std::uint32_t(attributes.size()));
   for (AttributeHandle const& a : attributes)
      w.u64(AttributeHandleFriend::value(a));
   if (pairs)
   {
      w.u32(std::uint32_t(pairs->size()));
      for (size_t k = 0; k < pairs->size(); ++k)
      {
         w.u32(std::uint32_t((*pairs)[k].first.size()));
         for (AttributeHandle const& a : (*pairs)[k].first)
            w.u64(AttributeHandleFriend::value(a));
         writeExtents(w, extents[k]);
      }
   }
   sendUpdate(*_transport, topic, w, 1);
}

// ============================================================= resign
// 4.10 resign actions: cancel the pending acquisitions, delete the objects we
// hold the delete privilege of, divest the remaining attributes (to a pending
// acquisition if any, otherwise unowned). Validated before any effect.
void MqttRTIambassador::resignObjects(ResignAction action)
{
   bool const cancel = action == CANCEL_PENDING_OWNERSHIP_ACQUISITIONS || action == CANCEL_THEN_DELETE_THEN_DIVEST;
   bool const remove =
      action == DELETE_OBJECTS || action == DELETE_OBJECTS_THEN_DIVEST || action == CANCEL_THEN_DELETE_THEN_DIVEST;
   bool const divestAll = action == UNCONDITIONALLY_DIVEST_ATTRIBUTES || action == DELETE_OBJECTS_THEN_DIVEST ||
                          action == CANCEL_THEN_DELETE_THEN_DIVEST;
   std::vector<std::pair<ObjectInstance, std::vector<std::string>>> deleted;
   std::set<std::wstring> names;
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      std::uint64_t const priv = privilegeAttribute();
      size_t owning = 0;
      bool pending = false;
      for (auto const& kv : _objects)
      {
         ObjectInstance const& o = kv.second;
         if (o.mom)
            continue;
         pending = pending || !o.acquiring.empty();
         if (!o.owned.empty() && !(remove && o.owned.count(priv)))
            ++owning;
      }
      if (pending && !cancel)
         throw OwnershipAcquisitionPending(L"the federate has pending attribute ownership acquisitions");
      if (owning != 0 && !divestAll)
         throw FederateOwnsAttributes(L"the federate still owns attributes of " + std::to_wstring(owning) +
                                      L" object instance(s)");
      for (auto it = _objects.begin(); it != _objects.end();)
      {
         ObjectInstance& o = it->second;
         if (o.mom)
         {
            ++it;
            continue;
         }
         if (cancel && !o.acquiring.empty())
         {
            std::set<std::uint64_t> attrs;
            for (auto const& acq : o.acquiring)
               attrs.insert(acq.first);
            cancelAcquisitions(o, attrs, false);
         }
         if (remove && o.owned.count(priv))
         {
            deleted.emplace_back(o, recordTopicsOf(o.handle));
            _ownerRecords.erase(o.handle);
            it = _objects.erase(it);
            ++_momDeleted;
            continue;
         }
         if (divestAll && !o.owned.empty())
         {
            std::set<std::uint64_t> const attrs = o.owned;
            divest(o, attrs);
         }
         ++it;
      }
      names = _reservedNames;
   }
   for (auto const& d : deleted)
      deleteOwnObject(d.first, d.second, VariableLengthData(), std::string());
   std::string const myId = encodeTopicLevel(_transport->settings().clientId);
   for (std::wstring const& n : names)
      _transport->publish(_fedTopic + "/names/" + encodeTopicLevel(toUtf8(n)) + "/" + myId, std::string(), 1, true);
}

// ============================================================ 10 Support
ObjectClassHandle MqttRTIambassador::getKnownObjectClassHandle(ObjectInstanceHandle theObject)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   auto it = _objects.find(ObjectInstanceHandleFriend::value(theObject));
   if (it != _objects.end() && it->second.knownClass != 0)
      return ObjectClassHandleFriend::create(it->second.knownClass);
   throw ObjectInstanceNotKnown(theObject.toString());
}

ObjectInstanceHandle MqttRTIambassador::getObjectInstanceHandle(std::wstring const& theName)
{
   checkJoined();
   std::string const name = toUtf8(theName);
   std::lock_guard<std::mutex> v(_viewMutex);
   for (auto const& kv : _objects)
      if (kv.second.name == name && kv.second.knownClass != 0)
         return ObjectInstanceHandleFriend::create(kv.first);
   throw ObjectInstanceNotKnown(theName);
}

std::wstring MqttRTIambassador::getObjectInstanceName(ObjectInstanceHandle theHandle)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   auto it = _objects.find(ObjectInstanceHandleFriend::value(theHandle));
   if (it != _objects.end() && it->second.knownClass != 0)
      return fromUtf8(it->second.name);
   throw ObjectInstanceNotKnown(theHandle.toString());
}

// 5.10 to 5.17 advisory switches: stored, with the standard IsOn/IsOff
// checks; no advisory callback is generated yet.
#define HLA_MQTT_SWITCH(Name, index, value, AlreadyExc)    void MqttRTIambassador::Name()    {       checkJoined();       checkNotSavingOrRestoring();       if (_advisorySwitches[index] == value)          throw AlreadyExc(L"" #Name);       setAdvisorySwitch(index, value);    }

HLA_MQTT_SWITCH(enableObjectClassRelevanceAdvisorySwitch, 0, true, ObjectClassRelevanceAdvisorySwitchIsOn)
HLA_MQTT_SWITCH(disableObjectClassRelevanceAdvisorySwitch, 0, false, ObjectClassRelevanceAdvisorySwitchIsOff)
HLA_MQTT_SWITCH(enableAttributeRelevanceAdvisorySwitch, 1, true, AttributeRelevanceAdvisorySwitchIsOn)
HLA_MQTT_SWITCH(disableAttributeRelevanceAdvisorySwitch, 1, false, AttributeRelevanceAdvisorySwitchIsOff)
HLA_MQTT_SWITCH(enableAttributeScopeAdvisorySwitch, 2, true, AttributeScopeAdvisorySwitchIsOn)
HLA_MQTT_SWITCH(disableAttributeScopeAdvisorySwitch, 2, false, AttributeScopeAdvisorySwitchIsOff)
HLA_MQTT_SWITCH(enableInteractionRelevanceAdvisorySwitch, 3, true, InteractionRelevanceAdvisorySwitchIsOn)
HLA_MQTT_SWITCH(disableInteractionRelevanceAdvisorySwitch, 3, false, InteractionRelevanceAdvisorySwitchIsOff)
} // namespace hla_mqtt
