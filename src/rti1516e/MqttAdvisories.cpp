// Advisories (IEEE 1516.1-2010, 5.10 to 5.17 and 6.x callbacks).
//
// Every joined federate publishes its subscriptions, retained:
//   F/decl/<federate handle>   {"o":{"<class>":["<attribute>",...]},"i":["<interaction class>",...]}
// From the subscriptions of the OTHER live federates, each federate computes:
//   - object class relevance: startRegistrationForObjectClass for a class it
//     publishes when someone subscribes to that class or to a superclass
//     (an instance is discovered at its closest subscribed superclass);
//   - interaction relevance: turnInteractionsOn likewise for its interactions;
//   - attribute relevance: turnUpdatesOnForObjectInstance for the attributes it
//     owns that a subscriber receives, i.e. that the subscriber subscribed at
//     the class it knows the object by; turnUpdatesOff when that stops;
//   - attribute scope: attributesInScope at discovery, for the subscribed
//     attributes owned by another federate (no DDM: in scope until removed).
// The callbacks follow the advisory switches (initial values from the FOM
// <switches>, else disabled). Disabling a switch stops the callbacks without
// "off" notifications; enabling it again notifies the current state.

#include "MqttRTIambassador.h"

#include "Fdd.h"
#include "HandleFriends.h"
#include "Json.h"
#include "MqttTransport.h"
#include "Wire.h"

namespace hla_mqtt
{
namespace
{
std::uint64_t parseHex(std::string const& s)
{
   return std::strtoull(s.c_str(), nullptr, 16);
}
} // namespace

std::string MqttRTIambassador::declarationsPayload() const
{
   Json o = Json::object();
   for (auto const& kv : _subscribedAttributes)
   {
      if (kv.second.empty())
         continue;
      Json attrs = Json::array();
      for (std::uint64_t a : kv.second)
         attrs.push(hex64(a));
      o.set(hex64(kv.first), attrs);
   }
   Json i = Json::array();
   for (std::uint64_t c : _subscribedInteractions)
      i.push(hex64(c));
   Json it = Json::object(); // 6.27 transportation changes, needed by queries and receivers
   for (auto const& kv : _interactionTransport)
      it.set(hex64(kv.first), kv.second);
   if (o.members().empty() && i.items().empty() && it.members().empty())
      return std::string();
   Json j = Json::object();
   j.set("o", o);
   j.set("i", i);
   if (!it.members().empty())
      j.set("it", it);
   return j.dump();
}

void MqttRTIambassador::publishDeclarations()
{
   if (!_transport || _fedTopic.empty())
      return;
   std::string const payload = declarationsPayload();
   if (payload == _lastDeclarations)
      return;
   _lastDeclarations = payload;
   try
   {
      _transport->publishAsync(_fedTopic + "/decl/" + hex64(_federateHandle), payload, 1, true);
   }
   catch (std::runtime_error const&)
   {
   }
}

void MqttRTIambassador::handleDeclarations(std::string const& hexHandle, std::string const& payload)
{
   std::uint64_t const h = parseHex(hexHandle);
   if (h == _federateHandle)
      return;
   if (payload.empty())
      _remoteDecl.erase(h);
   else
   {
      try
      {
         Json j = Json::parse(payload);
         RemoteDeclarations d;
         for (auto const& kv : j["o"].members())
         {
            std::set<std::uint64_t>& attrs = d.objects[parseHex(kv.first)];
            for (std::string const& a : kv.second.strings())
               attrs.insert(parseHex(a));
         }
         for (std::string const& c : j["i"].strings())
            d.interactions.insert(parseHex(c));
         for (auto const& kv : j["it"].members())
            d.interactionTransport[parseHex(kv.first)] = kv.second.str();
         _remoteDecl[h] = d;
      }
      catch (std::exception const&)
      {
         return;
      }
   }
   evaluateAdvisories();
}

void MqttRTIambassador::evaluateAdvisories()
{
   if (!_fdd || !_joined)
      return;
   std::set<std::uint64_t> const live = liveFederateHandles();

   // Closest class of the chain (self included) subscribed by `d`, 0 if none.
   auto subscribedAncestor = [this](RemoteDeclarations const& d, std::uint64_t cls) -> std::uint64_t {
      for (Fdd::ObjectClass const* c = _fdd->objectClass(cls); c; c = c->parent ? _fdd->objectClass(c->parent) : nullptr)
      {
         auto it = d.objects.find(c->handle);
         if (it != d.objects.end() && !it->second.empty())
            return c->handle;
      }
      return 0;
   };
   auto interactionSubscribed = [this](RemoteDeclarations const& d, std::uint64_t cls) {
      for (Fdd::InteractionClass const* c = _fdd->interactionClass(cls); c;
           c = c->parent ? _fdd->interactionClass(c->parent) : nullptr)
         if (d.interactions.count(c->handle))
            return true;
      return false;
   };

   // ------------------------------------------- object class relevance
   if (_advisorySwitches[0])
   {
      std::set<std::uint64_t> relevant;
      for (auto const& pub : _publishedAttributes)
      {
         if (pub.second.empty())
            continue;
         for (auto const& kv : _remoteDecl)
            if (live.count(kv.first) && subscribedAncestor(kv.second, pub.first) != 0)
            {
               relevant.insert(pub.first);
               break;
            }
      }
      for (std::uint64_t c : relevant)
         if (!_classRelevanceOn.count(c))
         {
            ObjectClassHandle const h = ObjectClassHandleFriend::create(c);
            enqueue([h](FederateAmbassador& fa) { fa.startRegistrationForObjectClass(h); });
         }
      for (std::uint64_t c : _classRelevanceOn)
         if (!relevant.count(c))
         {
            ObjectClassHandle const h = ObjectClassHandleFriend::create(c);
            enqueue([h](FederateAmbassador& fa) { fa.stopRegistrationForObjectClass(h); });
         }
      _classRelevanceOn = relevant;
   }
   else
      _classRelevanceOn.clear();

   // -------------------------------------------- interaction relevance
   if (_advisorySwitches[3])
   {
      std::set<std::uint64_t> relevant;
      for (std::uint64_t ic : _publishedInteractions)
         for (auto const& kv : _remoteDecl)
            if (live.count(kv.first) && interactionSubscribed(kv.second, ic))
            {
               relevant.insert(ic);
               break;
            }
      for (std::uint64_t c : relevant)
         if (!_interactionRelevanceOn.count(c))
         {
            InteractionClassHandle const h = InteractionClassHandleFriend::create(c);
            enqueue([h](FederateAmbassador& fa) { fa.turnInteractionsOn(h); });
         }
      for (std::uint64_t c : _interactionRelevanceOn)
         if (!relevant.count(c))
         {
            InteractionClassHandle const h = InteractionClassHandleFriend::create(c);
            enqueue([h](FederateAmbassador& fa) { fa.turnInteractionsOff(h); });
         }
      _interactionRelevanceOn = relevant;
   }
   else
      _interactionRelevanceOn.clear();

   // ---------------------------------------------- attribute relevance
   for (auto& kv : _objects)
   {
      ObjectInstance& o = kv.second;
      if (o.owner != _federateHandle)
         continue;
      if (!_advisorySwitches[1])
      {
         o.relevant.clear();
         continue;
      }
      std::set<std::uint64_t> relevant;
      for (auto const& d : _remoteDecl)
      {
         if (!live.count(d.first))
            continue;
         std::uint64_t const k = subscribedAncestor(d.second, o.objectClass);
         if (k == 0)
            continue;
         for (std::uint64_t a : d.second.objects.at(k))
            if (o.owned.count(a))
               relevant.insert(a);
      }
      AttributeHandleSet on, off;
      for (std::uint64_t a : relevant)
         if (!o.relevant.count(a))
            on.insert(AttributeHandleFriend::create(a));
      for (std::uint64_t a : o.relevant)
         if (!relevant.count(a))
            off.insert(AttributeHandleFriend::create(a));
      o.relevant = relevant;
      ObjectInstanceHandle const h = ObjectInstanceHandleFriend::create(o.handle);
      if (!on.empty())
         enqueue([h, on](FederateAmbassador& fa) { fa.turnUpdatesOnForObjectInstance(h, on); });
      if (!off.empty())
         enqueue([h, off](FederateAmbassador& fa) { fa.turnUpdatesOffForObjectInstance(h, off); });
   }
}

void MqttRTIambassador::notifyScope(ObjectInstance const& o)
{
   if (!_advisorySwitches[2] || o.knownClass == 0)
      return;
   auto it = _subscribedAttributes.find(o.knownClass);
   if (it == _subscribedAttributes.end())
      return;
   AttributeHandleSet inScope;
   for (std::uint64_t a : it->second)
      if (o.ownedRemote.count(a))
         inScope.insert(AttributeHandleFriend::create(a));
   if (inScope.empty())
      return;
   ObjectInstanceHandle const h = ObjectInstanceHandleFriend::create(o.handle);
   enqueue([h, inScope](FederateAmbassador& fa) { fa.attributesInScope(h, inScope); });
}

void MqttRTIambassador::setAdvisorySwitch(int index, bool value)
{
   std::lock_guard<std::mutex> v(_viewMutex);
   _advisorySwitches[index] = value;
   evaluateAdvisories();
}
} // namespace hla_mqtt
