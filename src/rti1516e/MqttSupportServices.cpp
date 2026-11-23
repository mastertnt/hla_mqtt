// Support services (IEEE 1516.1-2010, chapter 10) that only need the FDD
// or the federate records of the joined federation.

#include "MqttRTIambassador.h"

#include "Fdd.h"
#include "HandleFriends.h"
#include "MqttTransport.h"
#include "Utf.h"

#include <algorithm>
#include <cwctype>

namespace hla_mqtt
{
namespace
{
std::wstring lowerW(std::wstring s)
{
   std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return wchar_t(std::towlower(c)); });
   return s;
}

template <class Set, class Friend>
Set dimensionSet(Fdd const& fdd, std::vector<std::string> const& names)
{
   Set out;
   for (std::string const& n : names)
      if (Fdd::Dimension const* d = fdd.dimensionByName(n))
         out.insert(Friend::create(d->handle));
   return out;
}

template <class Handle, class Friend, class Exc>
Handle decodeHandle(VariableLengthData const& encoded, wchar_t const* kind)
{
   if (encoded.size() != 8 || encoded.data() == nullptr)
      throw CouldNotDecode(std::wstring(kind) + L": expected 8 octets");
   unsigned char const* p = static_cast<unsigned char const*>(encoded.data());
   std::uint64_t v = 0;
   for (int i = 0; i < 8; ++i)
      v = (v << 8) | p[i];
   return Friend::create(v);
}
} // namespace

// ------------------------------------------------------------ 10.6 / 10.7
ResignAction MqttRTIambassador::getAutomaticResignDirective()
{
   checkJoined();
   return _automaticResignDirective;
}

void MqttRTIambassador::setAutomaticResignDirective(ResignAction resignAction)
{
   checkJoined();
   if (resignAction < UNCONDITIONALLY_DIVEST_ATTRIBUTES || resignAction > NO_ACTION)
      throw InvalidResignAction(L"unknown resign action");
   _automaticResignDirective = resignAction;
}

// ------------------------------------------------------------ 10.8 / 10.9
FederateHandle MqttRTIambassador::getFederateHandle(std::wstring const& theName)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   if (theName == _federateName)
      return FederateHandleFriend::create(_federateHandle);
   std::string const name = toUtf8(theName);
   std::lock_guard<std::mutex> v(_viewMutex);
   for (FederateRecord const& r : liveFederates(_fedTopic))
      if (r.name == name)
         return FederateHandleFriend::create(r.handle);
   throw NameNotFound(L"no joined federate named " + theName);
}

std::wstring MqttRTIambassador::getFederateName(FederateHandle theHandle)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   if (!theHandle.isValid())
      throw InvalidFederateHandle(L"invalid federate handle");
   std::uint64_t const h = FederateHandleFriend::value(theHandle);
   if (h == _federateHandle)
      return _federateName;
   std::lock_guard<std::mutex> v(_viewMutex);
   for (FederateRecord const& r : liveFederates(_fedTopic))
      if (r.handle == h)
         return fromUtf8(r.name);
   throw FederateHandleNotKnown(theHandle.toString());
}

// --------------------------------------------------- 10.10 to 10.13
ObjectClassHandle MqttRTIambassador::getObjectClassHandle(std::wstring const& theName)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   if (Fdd::ObjectClass const* c = _fdd->objectClassByName(toUtf8(theName)))
      return ObjectClassHandleFriend::create(c->handle);
   throw NameNotFound(L"no object class named " + theName);
}

std::wstring MqttRTIambassador::getObjectClassName(ObjectClassHandle theHandle)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   if (Fdd::ObjectClass const* c = _fdd->objectClass(ObjectClassHandleFriend::value(theHandle)))
      return fromUtf8(c->qualified);
   throw InvalidObjectClassHandle(theHandle.toString());
}

AttributeHandle MqttRTIambassador::getAttributeHandle(ObjectClassHandle whichClass,
                                                      std::wstring const& theAttributeName)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   std::uint64_t const c = ObjectClassHandleFriend::value(whichClass);
   if (!_fdd->objectClass(c))
      throw InvalidObjectClassHandle(whichClass.toString());
   if (Fdd::Attribute const* a = _fdd->findAttribute(c, toUtf8(theAttributeName)))
      return AttributeHandleFriend::create(a->handle);
   throw NameNotFound(L"no attribute named " + theAttributeName + L" in " + whichClass.toString());
}

std::wstring MqttRTIambassador::getAttributeName(ObjectClassHandle whichClass, AttributeHandle theHandle)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   std::uint64_t const c = ObjectClassHandleFriend::value(whichClass);
   std::uint64_t const a = AttributeHandleFriend::value(theHandle);
   if (!_fdd->objectClass(c))
      throw InvalidObjectClassHandle(whichClass.toString());
   Fdd::Attribute const* attr = _fdd->attribute(a);
   if (!attr)
      throw InvalidAttributeHandle(theHandle.toString());
   if (!_fdd->classHasAttribute(c, a))
      throw AttributeNotDefined(theHandle.toString() + L" is not an attribute of " + whichClass.toString());
   return fromUtf8(attr->name);
}

// ------------------------------------------------------------------ 10.14
double MqttRTIambassador::getUpdateRateValue(std::wstring const& updateRateDesignator)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   double rate = 0;
   if (!_fdd->updateRate(toUtf8(updateRateDesignator), rate))
      throw InvalidUpdateRateDesignator(updateRateDesignator);
   return rate;
}

// --------------------------------------------------- 10.16 to 10.19
InteractionClassHandle MqttRTIambassador::getInteractionClassHandle(std::wstring const& theName)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   if (Fdd::InteractionClass const* c = _fdd->interactionClassByName(toUtf8(theName)))
      return InteractionClassHandleFriend::create(c->handle);
   throw NameNotFound(L"no interaction class named " + theName);
}

std::wstring MqttRTIambassador::getInteractionClassName(InteractionClassHandle theHandle)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   if (Fdd::InteractionClass const* c = _fdd->interactionClass(InteractionClassHandleFriend::value(theHandle)))
      return fromUtf8(c->qualified);
   throw InvalidInteractionClassHandle(theHandle.toString());
}

ParameterHandle MqttRTIambassador::getParameterHandle(InteractionClassHandle whichClass,
                                                      std::wstring const& theName)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   std::uint64_t const c = InteractionClassHandleFriend::value(whichClass);
   if (!_fdd->interactionClass(c))
      throw InvalidInteractionClassHandle(whichClass.toString());
   if (Fdd::Parameter const* p = _fdd->findParameter(c, toUtf8(theName)))
      return ParameterHandleFriend::create(p->handle);
   throw NameNotFound(L"no parameter named " + theName + L" in " + whichClass.toString());
}

std::wstring MqttRTIambassador::getParameterName(InteractionClassHandle whichClass, ParameterHandle theHandle)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   std::uint64_t const c = InteractionClassHandleFriend::value(whichClass);
   std::uint64_t const p = ParameterHandleFriend::value(theHandle);
   if (!_fdd->interactionClass(c))
      throw InvalidInteractionClassHandle(whichClass.toString());
   Fdd::Parameter const* param = _fdd->parameter(p);
   if (!param)
      throw InvalidParameterHandle(theHandle.toString());
   if (!_fdd->classHasParameter(c, p))
      throw InteractionParameterNotDefined(theHandle.toString() + L" is not a parameter of " +
                                           whichClass.toString());
   return fromUtf8(param->name);
}

// --------------------------------------------------- 10.20 to 10.23
OrderType MqttRTIambassador::getOrderType(std::wstring const& orderName)
{
   checkJoined();
   std::wstring const l = lowerW(orderName);
   if (l == L"receive")
      return RECEIVE;
   if (l == L"timestamp")
      return TIMESTAMP;
   throw InvalidOrderName(orderName);
}

std::wstring MqttRTIambassador::getOrderName(OrderType orderType)
{
   checkJoined();
   switch (orderType)
   {
   case RECEIVE: return L"Receive";
   case TIMESTAMP: return L"TimeStamp";
   }
   throw InvalidOrderType(L"unknown order type");
}

TransportationType MqttRTIambassador::getTransportationType(std::wstring const& transportationName)
{
   checkJoined();
   std::wstring const l = lowerW(transportationName);
   if (l == L"hlareliable")
      return RELIABLE;
   if (l == L"hlabesteffort")
      return BEST_EFFORT;
   throw InvalidTransportationName(transportationName);
}

std::wstring MqttRTIambassador::getTransportationName(TransportationType transportationType)
{
   checkJoined();
   switch (transportationType)
   {
   case RELIABLE: return L"HLAreliable";
   case BEST_EFFORT: return L"HLAbestEffort";
   }
   throw InvalidTransportationType(L"unknown transportation type");
}

// --------------------------------------------------- 10.24 to 10.28
DimensionHandleSet MqttRTIambassador::getAvailableDimensionsForClassAttribute(ObjectClassHandle theClass,
                                                                             AttributeHandle theHandle)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   std::uint64_t const c = ObjectClassHandleFriend::value(theClass);
   std::uint64_t const a = AttributeHandleFriend::value(theHandle);
   if (!_fdd->objectClass(c))
      throw InvalidObjectClassHandle(theClass.toString());
   Fdd::Attribute const* attr = _fdd->attribute(a);
   if (!attr)
      throw InvalidAttributeHandle(theHandle.toString());
   if (!_fdd->classHasAttribute(c, a))
      throw AttributeNotDefined(theHandle.toString() + L" is not an attribute of " + theClass.toString());
   return dimensionSet<DimensionHandleSet, DimensionHandleFriend>(*_fdd, attr->dimensions);
}

DimensionHandleSet MqttRTIambassador::getAvailableDimensionsForInteractionClass(InteractionClassHandle theClass)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   Fdd::InteractionClass const* c = _fdd->interactionClass(InteractionClassHandleFriend::value(theClass));
   if (!c)
      throw InvalidInteractionClassHandle(theClass.toString());
   return dimensionSet<DimensionHandleSet, DimensionHandleFriend>(*_fdd, c->dimensions);
}

DimensionHandle MqttRTIambassador::getDimensionHandle(std::wstring const& theName)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   if (Fdd::Dimension const* d = _fdd->dimensionByName(toUtf8(theName)))
      return DimensionHandleFriend::create(d->handle);
   throw NameNotFound(L"no dimension named " + theName);
}

std::wstring MqttRTIambassador::getDimensionName(DimensionHandle theHandle)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   if (Fdd::Dimension const* d = _fdd->dimension(DimensionHandleFriend::value(theHandle)))
      return fromUtf8(d->name);
   throw InvalidDimensionHandle(theHandle.toString());
}

unsigned long MqttRTIambassador::getDimensionUpperBound(DimensionHandle theHandle)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   if (Fdd::Dimension const* d = _fdd->dimension(DimensionHandleFriend::value(theHandle)))
      return d->upperBound;
   throw InvalidDimensionHandle(theHandle.toString());
}

// --------------------------------------------------- 10.33 / 10.34
unsigned long MqttRTIambassador::normalizeFederateHandle(FederateHandle theFederateHandle)
{
   checkJoined();
   if (!theFederateHandle.isValid())
      throw InvalidFederateHandle(L"invalid federate handle");
   std::uint64_t const h = FederateHandleFriend::value(theFederateHandle);
   return static_cast<unsigned long>(h ^ (h >> 32));
}

unsigned long MqttRTIambassador::normalizeServiceGroup(ServiceGroup theServiceGroup)
{
   checkJoined();
   if (theServiceGroup < FEDERATION_MANAGEMENT || theServiceGroup > SUPPORT_SERVICES)
      throw InvalidServiceGroup(L"unknown service group");
   return static_cast<unsigned long>(theServiceGroup);
}

// ------------------------------------------------------------------ 10.?
RTI_UNIQUE_PTR<LogicalTimeFactory> MqttRTIambassador::getTimeFactory() const
{
   checkJoined();
   return HLAlogicalTimeFactoryFactory::makeLogicalTimeFactory(_timeImplementation);
}

// ------------------------------------------------- handle decoding (12.x)
#define HLA_MQTT_DECODE(Kind)                                                                \
   Kind MqttRTIambassador::decode##Kind(VariableLengthData const& encodedValue) const          \
   {                                                                                           \
      checkJoined();                                                                           \
      return decodeHandle<Kind, Kind##Friend, CouldNotDecode>(encodedValue, L"" #Kind);        \
   }

HLA_MQTT_DECODE(FederateHandle)
HLA_MQTT_DECODE(ObjectClassHandle)
HLA_MQTT_DECODE(InteractionClassHandle)
HLA_MQTT_DECODE(ObjectInstanceHandle)
HLA_MQTT_DECODE(AttributeHandle)
HLA_MQTT_DECODE(ParameterHandle)
HLA_MQTT_DECODE(DimensionHandle)
HLA_MQTT_DECODE(MessageRetractionHandle)
HLA_MQTT_DECODE(RegionHandle)
} // namespace hla_mqtt
