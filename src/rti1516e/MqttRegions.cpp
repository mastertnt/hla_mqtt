// Data distribution management (IEEE 1516.1-2010, chapter 9): regions, update
// region associations and subscriptions with regions; 10.28 to 10.30.
//
// Regions are local to the federate that creates them. A region handle is
//   (upper 32 bits of the federate handle) << 32 | per-federate counter
// so that a federate recognizes a region created by another live federate
// (RegionNotCreatedByThisFederate) without any exchange.
//
// A new region spans [0, upper bound) on each of its dimensions. setRangeBounds
// changes the pending extents; commitRegionModifications makes them effective.
//
// Update regions travel in the retained info record of the object, as the
// committed extents (not handles), so that a receiver needs no region record:
//   "regions": {"<attribute>": [{"<dimension>": [lower, upper], ...}, ...]}
// The record is republished when an association or a committed region changes.
//
// A subscription with regions makes the attribute (or interaction class)
// subscribed, like a default-region subscription; the subscription stays as
// long as one of the two remains.
//
// Filtering (receiver side, see Extents.h for overlap): an attribute value or
// an interaction is delivered when the receiver subscribes with the default
// region, or when one of its subscription regions overlaps one of the update
// regions (default region if none). Interactions sent with regions carry their
// extents in the message (9.12). Subscriptions with regions only are published
// in F/decl/<federate> ("or") so that owners compute attribute relevance.
// Discovery is not filtered: subscribing to the class is enough.

#include "MqttRTIambassador.h"

#include "Fdd.h"
#include "HandleFriends.h"
#include "Json.h"
#include "MqttTransport.h"
#include "Utf.h"
#include "Wire.h"

#include <RTI/RangeBounds.h>

namespace hla_mqtt
{
namespace
{
template <class Fn>
void forEachAttribute(AttributeHandleSet const& attrs, Fn fn)
{
   for (AttributeHandle const& a : attrs)
      fn(AttributeHandleFriend::value(a), a);
}

std::uint64_t const kUpper32 = 0xFFFFFFFF00000000ull;
} // namespace

// ================================================================ helpers
void MqttRTIambassador::recomputeSubscriptions()
{
   _subscribedAttributes = _plainSubscribedAttributes;
   for (auto const& cls : _regionSubscribedAttributes)
      for (auto const& attr : cls.second)
         if (!attr.second.empty())
            _subscribedAttributes[cls.first].insert(attr.first);
   for (auto c = _subscribedRates.begin(); c != _subscribedRates.end();) // rates of subscribed attributes only
   {
      auto sub = _subscribedAttributes.find(c->first);
      for (auto a = c->second.begin(); a != c->second.end();)
         a = sub != _subscribedAttributes.end() && sub->second.count(a->first) ? std::next(a) : c->second.erase(a);
      c = c->second.empty() ? _subscribedRates.erase(c) : std::next(c);
   }
   _subscribedInteractions = _plainSubscribedInteractions;
   for (auto const& kv : _regionSubscribedInteractions)
      if (!kv.second.empty())
         _subscribedInteractions.insert(kv.first);
}

std::uint64_t MqttRTIambassador::ownRegion(RegionHandle const& h) const
{
   std::uint64_t const v = RegionHandleFriend::value(h);
   if (v != 0 && _regions.count(v))
      return v;
   if (v != 0 && (v & kUpper32) != (_federateHandle & kUpper32))
      for (std::uint64_t f : liveFederateHandles())
         if ((f & kUpper32) == (v & kUpper32))
            throw RegionNotCreatedByThisFederate(h.toString());
   throw InvalidRegion(h.toString());
}

void MqttRTIambassador::checkRegionContext(std::uint64_t region, std::vector<std::string> const& dimensionNames) const
{
   std::set<std::uint64_t> available;
   for (std::string const& n : dimensionNames)
      if (Fdd::Dimension const* d = _fdd->dimensionByName(n))
         available.insert(d->handle);
   for (auto const& kv : _regions.at(region).pending)
      if (!available.count(kv.first))
         throw InvalidRegionContext(RegionHandleFriend::create(region).toString() + L" spans dimension " +
                                    fromUtf8(_fdd->dimension(kv.first)->name) +
                                    L", not available for this class attribute or interaction class");
}

void MqttRTIambassador::checkRegionPairs(std::uint64_t cls, AttributeHandleSetRegionHandleSetPairVector const& pairs,
                                         bool context) const
{
   for (auto const& pair : pairs)
      forEachAttribute(pair.first, [&](std::uint64_t av, AttributeHandle const& ah) {
         if (!_fdd->classHasAttribute(cls, av))
            throw AttributeNotDefined(ah.toString());
      });
   for (auto const& pair : pairs)
      for (RegionHandle const& r : pair.second)
         ownRegion(r);
   if (!context)
      return;
   for (auto const& pair : pairs)
      for (RegionHandle const& r : pair.second)
         forEachAttribute(pair.first, [&](std::uint64_t av, AttributeHandle const&) {
            checkRegionContext(RegionHandleFriend::value(r), _fdd->attribute(av)->dimensions);
         });
}

bool MqttRTIambassador::regionInUse(std::uint64_t region) const
{
   for (auto const& kv : _objects)
      for (auto const& attr : kv.second.updateRegions)
         if (attr.second.count(region))
            return true;
   for (auto const& cls : _regionSubscribedAttributes)
      for (auto const& attr : cls.second)
         if (attr.second.count(region))
            return true;
   for (auto const& kv : _regionSubscribedInteractions)
      if (kv.second.count(region))
         return true;
   return false;
}

std::string MqttRTIambassador::objectInfo(ObjectInstance const& o) const
{
   Json info = Json::object();
   info.set("instance", hex64(o.handle));
   info.set("class", hex64(o.objectClass));
   info.set("name", o.name);
   info.set("registrant", hex64(o.registrant));
   info.set("clientId", _transport->settings().clientId); // of the publisher
   return info.dump();
}

std::vector<Extents> MqttRTIambassador::ownExtents(std::set<std::uint64_t> const& regions) const
{
   std::vector<Extents> out;
   for (std::uint64_t r : regions)
   {
      auto it = _regions.find(r);
      if (it != _regions.end())
         out.push_back(it->second.committed);
   }
   return out;
}

std::vector<Extents> MqttRTIambassador::updateExtents(ObjectInstance const& o, std::uint64_t attribute) const
{
   if (o.owned.count(attribute))
   {
      auto it = o.updateRegions.find(attribute);
      return it == o.updateRegions.end() ? std::vector<Extents>() : ownExtents(it->second);
   }
   std::uint64_t const owner = ownerOf(o, attribute); // the owner's record
   OwnerRecord const* r = owner != 0 ? liveRecord(o.handle, owner) : nullptr;
   if (!r)
      return std::vector<Extents>();
   auto it = r->regions.find(attribute);
   return it == r->regions.end() ? std::vector<Extents>() : it->second;
}

bool MqttRTIambassador::subscriptionCovers(std::uint64_t cls, std::uint64_t attribute,
                                           std::vector<Extents> const& sent) const
{
   auto plain = _plainSubscribedAttributes.find(cls);
   if (plain != _plainSubscribedAttributes.end() && plain->second.count(attribute))
      return true;
   auto c = _regionSubscribedAttributes.find(cls);
   if (c == _regionSubscribedAttributes.end())
      return false;
   auto a = c->second.find(attribute);
   if (a == c->second.end() || a->second.empty())
      return false;
   return anyOverlap(sent, ownExtents(a->second));
}

bool MqttRTIambassador::interactionCovers(std::uint64_t cls, std::vector<Extents> const& sent) const
{
   if (_plainSubscribedInteractions.count(cls))
      return true;
   auto it = _regionSubscribedInteractions.find(cls);
   if (it == _regionSubscribedInteractions.end() || it->second.empty())
      return false;
   return anyOverlap(sent, ownExtents(it->second));
}

std::vector<std::pair<std::string, std::string>> MqttRTIambassador::infoRecordsUsing(
   std::set<std::uint64_t> const& regions)
{
   std::vector<std::pair<std::string, std::string>> records;
   for (auto& kv : _objects)
   {
      ObjectInstance& o = kv.second;
      if (o.owned.empty() || o.mom)
         continue;
      bool uses = false;
      for (auto const& attr : o.updateRegions)
         for (std::uint64_t r : attr.second)
            uses = uses || (regions.count(r) != 0 && o.owned.count(attr.first) != 0);
      if (uses)
      {
         records.push_back(ownerRecordEntry(o));
         ++o.recordsInFlight;
      }
   }
   return records;
}

void MqttRTIambassador::publishInfoRecords(std::vector<std::pair<std::string, std::string>> const& records)
{
   try
   {
      for (auto const& r : records)
         _transport->publish(r.first, r.second, 1, true);
   }
   catch (std::runtime_error const& e)
   {
      throw RTIinternalError(fromUtf8(e.what()));
   }
}

// ================================================================ 9.2 to 9.4
RegionHandle MqttRTIambassador::createRegion(DimensionHandleSet const& theDimensions)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   Region region;
   for (DimensionHandle const& d : theDimensions)
   {
      Fdd::Dimension const* dim = _fdd->dimension(DimensionHandleFriend::value(d));
      if (!dim)
         throw InvalidDimensionHandle(d.toString());
      region.pending[dim->handle] = std::make_pair(0ul, dim->upperBound);
   }
   region.committed = region.pending;
   std::uint64_t const handle = (_federateHandle & kUpper32) | ++_regionCounter;
   _regions[handle] = region;
   return RegionHandleFriend::create(handle);
}

void MqttRTIambassador::commitRegionModifications(RegionHandleSet const& theRegionHandleSet)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   std::vector<std::pair<std::string, std::string>> records;
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      std::set<std::uint64_t> changed;
      for (RegionHandle const& r : theRegionHandleSet)
         changed.insert(ownRegion(r));
      for (std::uint64_t r : changed)
         _regions[r].committed = _regions[r].pending;
      records = infoRecordsUsing(changed);
      publishDeclarations(); // subscription regions
      evaluateAdvisories();  // relevance of our updates, scope of what we receive
   }
   publishInfoRecords(records);
}

void MqttRTIambassador::deleteRegion(RegionHandle const& theRegion)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   std::uint64_t const r = ownRegion(theRegion);
   if (regionInUse(r))
      throw RegionInUseForUpdateOrSubscription(theRegion.toString());
   _regions.erase(r);
}

// ================================================================ 9.5 to 9.7
ObjectInstanceHandle MqttRTIambassador::registerObjectInstanceWithRegions(
   ObjectClassHandle theClass, AttributeHandleSetRegionHandleSetPairVector const& theAttributeHandleSetRegionHandleSetPairVector)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   return registerObject(theClass, std::wstring(), &theAttributeHandleSetRegionHandleSetPairVector);
}

ObjectInstanceHandle MqttRTIambassador::registerObjectInstanceWithRegions(
   ObjectClassHandle theClass, AttributeHandleSetRegionHandleSetPairVector const& theAttributeHandleSetRegionHandleSetPairVector,
   std::wstring const& theObjectInstanceName)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   return registerObject(theClass, theObjectInstanceName, &theAttributeHandleSetRegionHandleSetPairVector);
}

namespace
{
// The class the federate knows the object by; 0 if the object is unknown to it.
template <class Objects>
std::uint64_t knownClassOf(Objects const& objects, std::uint64_t instance, std::uint64_t self)
{
   auto it = objects.find(instance);
   if (it == objects.end())
      return 0;
   (void)self;
   return it->second.knownClass;
}
} // namespace

void MqttRTIambassador::associateRegionsForUpdates(
   ObjectInstanceHandle theObject, AttributeHandleSetRegionHandleSetPairVector const& theAttributeHandleSetRegionHandleSetPairVector)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   std::vector<std::pair<std::string, std::string>> records;
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      std::uint64_t const instance = ObjectInstanceHandleFriend::value(theObject);
      std::uint64_t const cls = knownClassOf(_objects, instance, _federateHandle);
      if (cls == 0)
         throw ObjectInstanceNotKnown(theObject.toString());
      checkRegionPairs(cls, theAttributeHandleSetRegionHandleSetPairVector);
      ObjectInstance& o = _objects[instance];
      for (auto const& pair : theAttributeHandleSetRegionHandleSetPairVector)
         forEachAttribute(pair.first, [&](std::uint64_t av, AttributeHandle const&) {
            for (RegionHandle const& r : pair.second)
               o.updateRegions[av].insert(RegionHandleFriend::value(r));
         });
      if (!o.owned.empty() && !o.mom)
      {
         records.push_back(ownerRecordEntry(o));
         ++o.recordsInFlight;
      }
      evaluateAdvisories();
   }
   publishInfoRecords(records);
}

void MqttRTIambassador::unassociateRegionsForUpdates(
   ObjectInstanceHandle theObject, AttributeHandleSetRegionHandleSetPairVector const& theAttributeHandleSetRegionHandleSetPairVector)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   std::vector<std::pair<std::string, std::string>> records;
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      std::uint64_t const instance = ObjectInstanceHandleFriend::value(theObject);
      std::uint64_t const cls = knownClassOf(_objects, instance, _federateHandle);
      if (cls == 0)
         throw ObjectInstanceNotKnown(theObject.toString());
      checkRegionPairs(cls, theAttributeHandleSetRegionHandleSetPairVector, false);
      ObjectInstance& o = _objects[instance];
      for (auto const& pair : theAttributeHandleSetRegionHandleSetPairVector)
         forEachAttribute(pair.first, [&](std::uint64_t av, AttributeHandle const&) {
            auto it = o.updateRegions.find(av);
            if (it == o.updateRegions.end())
               return;
            for (RegionHandle const& r : pair.second)
               it->second.erase(RegionHandleFriend::value(r));
            if (it->second.empty())
               o.updateRegions.erase(it);
         });
      if (!o.owned.empty() && !o.mom)
      {
         records.push_back(ownerRecordEntry(o));
         ++o.recordsInFlight;
      }
      evaluateAdvisories();
   }
   publishInfoRecords(records);
}

// ================================================================ 9.8 to 9.11
void MqttRTIambassador::subscribeObjectClassAttributesWithRegions(
   ObjectClassHandle theClass, AttributeHandleSetRegionHandleSetPairVector const& theAttributeHandleSetRegionHandleSetPairVector,
   bool /*active*/, std::wstring const& updateRateDesignator)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      std::uint64_t const cls = ObjectClassHandleFriend::value(theClass);
      if (!_fdd->objectClass(cls))
         throw ObjectClassNotDefined(theClass.toString());
      checkRegionPairs(cls, theAttributeHandleSetRegionHandleSetPairVector);
      double rate = 0;
      if (!updateRateDesignator.empty() && !_fdd->updateRate(toUtf8(updateRateDesignator), rate))
         throw InvalidUpdateRateDesignator(updateRateDesignator);
      for (auto const& pair : theAttributeHandleSetRegionHandleSetPairVector)
      {
         forEachAttribute(pair.first, [&](std::uint64_t av, AttributeHandle const&) {
            for (RegionHandle const& r : pair.second)
               _regionSubscribedAttributes[cls][av].insert(RegionHandleFriend::value(r));
         });
         setSubscribedRates(cls, pair.first, rate);
      }
      recomputeSubscriptions();
   }
   updateBrokerSubscriptions();
}

void MqttRTIambassador::unsubscribeObjectClassAttributesWithRegions(
   ObjectClassHandle theClass, AttributeHandleSetRegionHandleSetPairVector const& theAttributeHandleSetRegionHandleSetPairVector)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      std::uint64_t const cls = ObjectClassHandleFriend::value(theClass);
      if (!_fdd->objectClass(cls))
         throw ObjectClassNotDefined(theClass.toString());
      checkRegionPairs(cls, theAttributeHandleSetRegionHandleSetPairVector, false);
      auto c = _regionSubscribedAttributes.find(cls);
      if (c != _regionSubscribedAttributes.end())
      {
         for (auto const& pair : theAttributeHandleSetRegionHandleSetPairVector)
            forEachAttribute(pair.first, [&](std::uint64_t av, AttributeHandle const&) {
               auto it = c->second.find(av);
               if (it == c->second.end())
                  return;
               for (RegionHandle const& r : pair.second)
                  it->second.erase(RegionHandleFriend::value(r));
               if (it->second.empty())
                  c->second.erase(it);
            });
         if (c->second.empty())
            _regionSubscribedAttributes.erase(c);
      }
      recomputeSubscriptions();
   }
   updateBrokerSubscriptions();
}

void MqttRTIambassador::subscribeInteractionClassWithRegions(InteractionClassHandle theClass,
                                                             RegionHandleSet const& theRegionHandleSet, bool /*active*/)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      std::uint64_t const cls = InteractionClassHandleFriend::value(theClass);
      Fdd::InteractionClass const* ic = _fdd->interactionClass(cls);
      if (!ic)
         throw InteractionClassNotDefined(theClass.toString());
      for (RegionHandle const& r : theRegionHandleSet)
         ownRegion(r);
      for (RegionHandle const& r : theRegionHandleSet)
         checkRegionContext(RegionHandleFriend::value(r), ic->dimensions);
      for (RegionHandle const& r : theRegionHandleSet)
         _regionSubscribedInteractions[cls].insert(RegionHandleFriend::value(r));
      recomputeSubscriptions();
   }
   updateBrokerSubscriptions();
}

void MqttRTIambassador::unsubscribeInteractionClassWithRegions(InteractionClassHandle theClass,
                                                               RegionHandleSet const& theRegionHandleSet)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      std::uint64_t const cls = InteractionClassHandleFriend::value(theClass);
      if (!_fdd->interactionClass(cls))
         throw InteractionClassNotDefined(theClass.toString());
      for (RegionHandle const& r : theRegionHandleSet)
         ownRegion(r);
      auto it = _regionSubscribedInteractions.find(cls);
      if (it != _regionSubscribedInteractions.end())
      {
         for (RegionHandle const& r : theRegionHandleSet)
            it->second.erase(RegionHandleFriend::value(r));
         if (it->second.empty())
            _regionSubscribedInteractions.erase(it);
      }
      recomputeSubscriptions();
   }
   updateBrokerSubscriptions();
}

// ================================================================ 9.12, 9.13
void MqttRTIambassador::sendInteractionWithRegions(InteractionClassHandle theInteraction,
                                                   ParameterHandleValueMap const& theParameterValues,
                                                   RegionHandleSet const& theRegionHandleSet,
                                                   VariableLengthData const& theUserSuppliedTag)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   sendInteractionMessage(theInteraction, theParameterValues, theUserSuppliedTag, nullptr, &theRegionHandleSet);
}

MessageRetractionHandle MqttRTIambassador::sendInteractionWithRegions(InteractionClassHandle theInteraction,
                                                                      ParameterHandleValueMap const& theParameterValues,
                                                                      RegionHandleSet const& theRegionHandleSet,
                                                                      VariableLengthData const& theUserSuppliedTag,
                                                                      LogicalTime const& theTime)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   return sendInteractionMessage(theInteraction, theParameterValues, theUserSuppliedTag, &theTime, &theRegionHandleSet);
}

void MqttRTIambassador::requestAttributeValueUpdateWithRegions(ObjectClassHandle theClass,
                                                               AttributeHandleSetRegionHandleSetPairVector const& theSet,
                                                               VariableLengthData const& theUserSuppliedTag)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   AttributeHandleSet all;
   for (auto const& pair : theSet)
      all.insert(pair.first.begin(), pair.first.end());
   requestClassUpdate(theClass, all, &theSet, theUserSuppliedTag);
}

// ================================================================ 10.28 to 10.30
DimensionHandleSet MqttRTIambassador::getDimensionHandleSet(RegionHandle theRegionHandle)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   auto it = _regions.find(RegionHandleFriend::value(theRegionHandle));
   if (it == _regions.end())
      throw InvalidRegion(theRegionHandle.toString());
   DimensionHandleSet dims;
   for (auto const& kv : it->second.pending)
      dims.insert(DimensionHandleFriend::create(kv.first));
   return dims;
}

RangeBounds MqttRTIambassador::getRangeBounds(RegionHandle theRegionHandle, DimensionHandle theDimensionHandle)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   auto it = _regions.find(RegionHandleFriend::value(theRegionHandle));
   if (it == _regions.end())
      throw InvalidRegion(theRegionHandle.toString());
   auto d = it->second.pending.find(DimensionHandleFriend::value(theDimensionHandle));
   if (d == it->second.pending.end())
      throw RegionDoesNotContainSpecifiedDimension(theDimensionHandle.toString());
   return RangeBounds(d->second.first, d->second.second);
}

void MqttRTIambassador::setRangeBounds(RegionHandle theRegionHandle, DimensionHandle theDimensionHandle,
                                       RangeBounds const& theRangeBounds)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   Region& region = _regions[ownRegion(theRegionHandle)];
   auto d = region.pending.find(DimensionHandleFriend::value(theDimensionHandle));
   if (d == region.pending.end())
      throw RegionDoesNotContainSpecifiedDimension(theDimensionHandle.toString());
   unsigned long const upperBound = _fdd->dimension(d->first)->upperBound;
   if (theRangeBounds.getLowerBound() >= theRangeBounds.getUpperBound() ||
       theRangeBounds.getUpperBound() > upperBound)
      throw InvalidRangeBound(L"[" + std::to_wstring(theRangeBounds.getLowerBound()) + L", " +
                              std::to_wstring(theRangeBounds.getUpperBound()) + L") is not a valid range within [0, " +
                              std::to_wstring(upperBound) + L")");
   d->second = std::make_pair(theRangeBounds.getLowerBound(), theRangeBounds.getUpperBound());
}
} // namespace hla_mqtt
