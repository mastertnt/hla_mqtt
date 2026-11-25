// Per-instance and per-class options of the messages a federate sends:
//   6.23 to 6.30  transportation type: change (confirmed by callback) and query
//   8.23, 8.24    order type of the federate's own updates and interactions
//   10.14         update rate value of an instance attribute
//
// A change only affects the messages sent by the federate that asks for it.
// Receivers learn it from retained records:
//   - attribute transportation: "transport": {"<attribute>": "HLAbestEffort"} in
//     the object's info record (republished on change);
//   - interaction transportation: "it": {"<class>": "HLAbestEffort"} in the
//     F/decl/<federate> record.
// The order type needs no record: each message says whether it is TSO.

#include "MqttRTIambassador.h"

#include "Fdd.h"
#include "HandleFriends.h"
#include "MqttTransport.h"
#include "Utf.h"

#include <tuple>

namespace hla_mqtt
{
namespace
{
char const* transportationName(TransportationType t)
{
   return t == BEST_EFFORT ? "HLAbestEffort" : "HLAreliable";
}

TransportationType transportationOf(bool bestEffort)
{
   return bestEffort ? BEST_EFFORT : RELIABLE;
}
} // namespace

// ================================================================ helpers
bool MqttRTIambassador::timestampOrder(ObjectInstance const& o, std::uint64_t attribute) const
{
   auto it = o.order.find(attribute);
   if (it != o.order.end())
      return it->second == TIMESTAMP;
   Fdd::Attribute const* attr = _fdd->attribute(attribute);
   return attr && attr->order == "TimeStamp";
}

bool MqttRTIambassador::bestEffort(ObjectInstance const& o, std::uint64_t attribute) const
{
   std::map<std::uint64_t, std::string> const* changed = &o.transport; // ours, or the owner's record
   if (!o.owned.count(attribute))
   {
      std::uint64_t const owner = ownerOf(o, attribute);
      OwnerRecord const* r = owner != 0 ? liveRecord(o.handle, owner) : nullptr;
      changed = r ? &r->transport : nullptr;
   }
   if (changed)
   {
      auto it = changed->find(attribute);
      if (it != changed->end())
         return it->second == "HLAbestEffort";
   }
   Fdd::Attribute const* attr = _fdd->attribute(attribute);
   return attr && attr->transportation == "HLAbestEffort";
}

bool MqttRTIambassador::interactionTimestampOrder(std::uint64_t interactionClass) const
{
   auto it = _interactionOrder.find(interactionClass);
   if (it != _interactionOrder.end())
      return it->second == TIMESTAMP;
   Fdd::InteractionClass const* ic = _fdd->interactionClass(interactionClass);
   return ic && ic->order == "TimeStamp";
}

bool MqttRTIambassador::interactionBestEffort(std::uint64_t sender, std::uint64_t interactionClass) const
{
   std::map<std::uint64_t, std::string> const* changes = nullptr;
   if (sender == _federateHandle)
      changes = &_interactionTransport;
   else
   {
      auto d = _remoteDecl.find(sender);
      if (d != _remoteDecl.end())
         changes = &d->second.interactionTransport;
   }
   if (changes)
   {
      auto it = changes->find(interactionClass);
      if (it != changes->end())
         return it->second == "HLAbestEffort";
   }
   Fdd::InteractionClass const* ic = _fdd->interactionClass(interactionClass);
   return ic && ic->transportation == "HLAbestEffort";
}

void MqttRTIambassador::setSubscribedRates(std::uint64_t cls, AttributeHandleSet const& attrs, double rate)
{
   for (AttributeHandle const& a : attrs)
      if (rate > 0)
         _subscribedRates[cls][AttributeHandleFriend::value(a)] = rate;
      else if (_subscribedRates.count(cls))
         _subscribedRates[cls].erase(AttributeHandleFriend::value(a));
}

namespace
{
// Object known to the federate and the class it knows it by (registered class
// for its own objects); throws ObjectInstanceNotKnown.
template <class Objects>
auto knownObject(Objects& objects, ObjectInstanceHandle const& h, std::uint64_t /*self*/, std::uint64_t& cls)
   -> decltype(&objects.begin()->second)
{
   auto it = objects.find(ObjectInstanceHandleFriend::value(h));
   if (it != objects.end())
   {
      cls = it->second.knownClass;
      if (cls != 0)
         return &it->second;
   }
   throw ObjectInstanceNotKnown(h.toString());
}

void checkTransportation(TransportationType t)
{
   if (t != RELIABLE && t != BEST_EFFORT)
      throw InvalidTransportationType(std::to_wstring(int(t)));
}
} // namespace

// ======================================= 6.23 to 6.30 transportation type
void MqttRTIambassador::requestAttributeTransportationTypeChange(ObjectInstanceHandle theObject,
                                                                 AttributeHandleSet const& theAttributes,
                                                                 TransportationType theType)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   std::string topic, payload;
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      std::uint64_t cls = 0;
      ObjectInstance* o = knownObject(_objects, theObject, _federateHandle, cls);
      for (AttributeHandle const& a : theAttributes)
         if (!_fdd->classHasAttribute(cls, AttributeHandleFriend::value(a)))
            throw AttributeNotDefined(a.toString());
      for (AttributeHandle const& a : theAttributes)
         if (!o->owned.count(AttributeHandleFriend::value(a)))
            throw AttributeNotOwned(a.toString());
      checkTransportation(theType);
      // The change is immediate: never AttributeAlreadyBeingChanged.
      for (AttributeHandle const& a : theAttributes)
         o->transport[AttributeHandleFriend::value(a)] = transportationName(theType);
      std::tie(topic, payload) = ownerRecordEntry(*o); // seen by the subscribers
      ++o->recordsInFlight;
   }
   try
   {
      _transport->publish(topic, payload, 1, true);
   }
   catch (std::runtime_error const& e)
   {
      throw RTIinternalError(fromUtf8(e.what()));
   }
   AttributeHandleSet const attrs = theAttributes;
   enqueue([theObject, attrs, theType](FederateAmbassador& fa) {
      fa.confirmAttributeTransportationTypeChange(theObject, attrs, theType);
   });
}

void MqttRTIambassador::queryAttributeTransportationType(ObjectInstanceHandle theObject, AttributeHandle theAttribute)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   std::uint64_t cls = 0;
   ObjectInstance const* o = knownObject(_objects, theObject, _federateHandle, cls);
   std::uint64_t const a = AttributeHandleFriend::value(theAttribute);
   if (!_fdd->classHasAttribute(cls, a))
      throw AttributeNotDefined(theAttribute.toString());
   TransportationType const t = transportationOf(bestEffort(*o, a));
   enqueue([theObject, theAttribute, t](FederateAmbassador& fa) {
      fa.reportAttributeTransportationType(theObject, theAttribute, t);
   });
}

void MqttRTIambassador::requestInteractionTransportationTypeChange(InteractionClassHandle theClass,
                                                                   TransportationType theType)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      std::uint64_t const c = InteractionClassHandleFriend::value(theClass);
      if (!_fdd->interactionClass(c))
         throw InteractionClassNotDefined(theClass.toString());
      if (!_publishedInteractions.count(c))
         throw InteractionClassNotPublished(theClass.toString());
      checkTransportation(theType);
      _interactionTransport[c] = transportationName(theType);
      publishDeclarations();
   }
   enqueue([theClass, theType](FederateAmbassador& fa) { fa.confirmInteractionTransportationTypeChange(theClass, theType); });
}

void MqttRTIambassador::queryInteractionTransportationType(FederateHandle theFederate,
                                                           InteractionClassHandle theInteraction)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   std::uint64_t const c = InteractionClassHandleFriend::value(theInteraction);
   if (!_fdd->interactionClass(c))
      throw InteractionClassNotDefined(theInteraction.toString());
   TransportationType const t = transportationOf(interactionBestEffort(FederateHandleFriend::value(theFederate), c));
   enqueue([theFederate, theInteraction, t](FederateAmbassador& fa) {
      fa.reportInteractionTransportationType(theFederate, theInteraction, t);
   });
}

// =========================================================== 8.23, 8.24
void MqttRTIambassador::changeAttributeOrderType(ObjectInstanceHandle theObject, AttributeHandleSet const& theAttributes,
                                                 OrderType theType)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   std::uint64_t cls = 0;
   ObjectInstance* o = knownObject(_objects, theObject, _federateHandle, cls);
   for (AttributeHandle const& a : theAttributes)
      if (!_fdd->classHasAttribute(cls, AttributeHandleFriend::value(a)))
         throw AttributeNotDefined(a.toString());
   for (AttributeHandle const& a : theAttributes)
      if (!o->owned.count(AttributeHandleFriend::value(a)))
         throw AttributeNotOwned(a.toString());
   if (theType != RECEIVE && theType != TIMESTAMP)
      throw RTIinternalError(L"invalid order type " + std::to_wstring(int(theType)));
   for (AttributeHandle const& a : theAttributes)
      o->order[AttributeHandleFriend::value(a)] = theType;
}

void MqttRTIambassador::changeInteractionOrderType(InteractionClassHandle theClass, OrderType theType)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   std::uint64_t const c = InteractionClassHandleFriend::value(theClass);
   if (!_fdd->interactionClass(c))
      throw InteractionClassNotDefined(theClass.toString());
   if (!_publishedInteractions.count(c))
      throw InteractionClassNotPublished(theClass.toString());
   if (theType != RECEIVE && theType != TIMESTAMP)
      throw RTIinternalError(L"invalid order type " + std::to_wstring(int(theType)));
   _interactionOrder[c] = theType;
}

// ================================================================ 10.14
double MqttRTIambassador::getUpdateRateValueForAttribute(ObjectInstanceHandle theObject, AttributeHandle theAttribute)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   std::uint64_t cls = 0;
   ObjectInstance const* o = knownObject(_objects, theObject, _federateHandle, cls);
   std::uint64_t const a = AttributeHandleFriend::value(theAttribute);
   if (!_fdd->classHasAttribute(cls, a))
      throw AttributeNotDefined(theAttribute.toString());
   if (o->owned.count(a))
      return 0; // not reflected by its owner
   auto c = _subscribedRates.find(cls);
   if (c == _subscribedRates.end())
      return 0;
   auto r = c->second.find(a);
   return r == c->second.end() ? 0 : r->second; // 0: no update rate designator
}
} // namespace hla_mqtt
