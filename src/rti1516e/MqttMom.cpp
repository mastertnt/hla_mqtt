// Management Object Model (IEEE 1516.1-2010, chapter 11) without central RTI.
//
// The FDD holds the standard MIM (built into the library, see
// cmake/EmbedFile.cmake) unless the federation was created with another MIM.
// Each federate plays the RTI part of the MOM for itself:
//   - HLAmanager.HLAfederate: every joined federate registers its own instance
//     (info record published like any object, owner = the federate). Its
//     values are computed when needed and sent as ordinary updates, on
//     requestAttributeValueUpdate and every HLAsetTiming period.
//   - HLAmanager.HLAfederation: built locally by each federate from the
//     retained records (execution, FOM modules, federates); discovered when
//     the class is subscribed, its values reflected locally on request.
//   - HLAmanager.HLAfederate.HLArequest/HLAadjust/HLAservice: every federate
//     receives these interactions (internal broker subscriptions) and handles
//     the ones whose HLAfederate parameter is its own handle. HLAservice
//     interactions invoke the service on the federate thread, with the
//     callbacks (an internal queue entry, outside callback scope).
//   - HLAmanager.HLAfederation.HLArequest: answered by the live federate with
//     the smallest handle (all federates see the same records).
//   - HLAreportFederateLost: sent by that same federate when another one
//     vanishes without resigning (Last Will).
// A federate does not need to publish the report classes: the RTI sends them.
// The values follow the MIM data types, encoded with the encoding helpers of
// this library.
//
// Limits: HLAreportServiceInvocation covers the services invoked through
// HLAservice interactions only; HLAmodifyAttributeState and the save and
// restore services answer HLAreportMOMexception (not implemented).

#include "MqttRTIambassador.h"

#include "Fdd.h"
#include "HandleFriends.h"
#include "Json.h"
#include "MqttTransport.h"
#include "Utf.h"
#include "Version.h"
#include "Wire.h"

#include <RTI/encoding/BasicDataElements.h>
#include <RTI/encoding/HLAfixedRecord.h>
#include <RTI/encoding/HLAopaqueData.h>
#include <RTI/encoding/HLAvariableArray.h>
#include <RTI/time/HLAfloat64Interval.h>
#include <RTI/time/HLAinteger64Interval.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace hla_mqtt
{
namespace
{
std::string const kFederateClass = "HLAobjectRoot.HLAmanager.HLAfederate";
std::string const kFederationClass = "HLAobjectRoot.HLAmanager.HLAfederation";
std::string const kFederateI = "HLAinteractionRoot.HLAmanager.HLAfederate";
std::string const kFederationI = "HLAinteractionRoot.HLAmanager.HLAfederation";

bool startsWith(std::string const& s, std::string const& prefix)
{
   return s.compare(0, prefix.size(), prefix) == 0;
}

VariableLengthData vld(std::string const& s)
{
   return VariableLengthData(s.data(), s.size());
}

std::string bytesOf(VariableLengthData const& d)
{
   return d.size() ? std::string(static_cast<char const*>(d.data()), d.size()) : std::string();
}

// ------------------------------------------------------------ encoding
HLAopaqueData handleElement(std::uint64_t v)
{
   Octet b[8];
   for (int i = 7; i >= 0; --i, v >>= 8)
      b[i] = Octet(v & 0xFF);
   return HLAopaqueData(b, 8);
}

VariableLengthData encInt(long long v)
{
   return HLAinteger32BE(Integer32(v)).encode();
}

VariableLengthData encBool(bool b)
{
   return encInt(b ? 1 : 0);
}

VariableLengthData encString(std::wstring const& s)
{
   return HLAunicodeString(s).encode();
}

VariableLengthData encHandle(std::uint64_t h)
{
   return handleElement(h).encode();
}

VariableLengthData encBytes(std::string const& b) // HLAlogicalTime, HLAtimeInterval, HLAopaqueData
{
   return HLAopaqueData(reinterpret_cast<Octet const*>(b.data()), b.size()).encode();
}

VariableLengthData encHandleList(std::vector<std::uint64_t> const& handles)
{
   HLAvariableArray a{HLAopaqueData()};
   for (std::uint64_t h : handles)
      a.addElement(handleElement(h));
   return a.encode();
}

VariableLengthData encStringList(std::vector<std::wstring> const& strings)
{
   HLAvariableArray a{HLAunicodeString()};
   for (std::wstring const& s : strings)
      a.addElement(HLAunicodeString(s));
   return a.encode();
}

// Array of {handle, HLAinteger32BE} records (HLAobjectClassBasedCounts,
// HLAinteractionCounts, HLAinteractionSubList, HLAsynchPointFederateList).
VariableLengthData encHandleIntList(std::vector<std::pair<std::uint64_t, long long>> const& entries)
{
   HLAfixedRecord prototype;
   prototype.appendElement(HLAopaqueData());
   prototype.appendElement(HLAinteger32BE());
   HLAvariableArray a{prototype};
   for (auto const& e : entries)
   {
      HLAfixedRecord r;
      r.appendElement(handleElement(e.first));
      r.appendElement(HLAinteger32BE(Integer32(e.second)));
      a.addElement(r);
   }
   return a.encode();
}

// ------------------------------------------------------------ decoding
struct MomParameterError
{
   std::wstring what;
};

std::uint64_t decHandle(std::string const& b)
{
   HLAopaqueData d;
   try
   {
      d.decode(vld(b));
   }
   catch (EncoderException const&)
   {
      throw MomParameterError{L"undecodable handle"};
   }
   if (d.dataLength() != 8)
      throw MomParameterError{L"handle of " + std::to_wstring(d.dataLength()) + L" octets, 8 expected"};
   std::uint64_t v = 0;
   for (size_t i = 0; i < 8; ++i)
      v = (v << 8) | static_cast<unsigned char>(d.get()[i]);
   return v;
}

long long decInt(std::string const& b)
{
   HLAinteger32BE x;
   try
   {
      x.decode(vld(b));
   }
   catch (EncoderException const&)
   {
      throw MomParameterError{L"undecodable HLAinteger32BE"};
   }
   return x.get();
}

std::wstring decString(std::string const& b)
{
   HLAunicodeString s;
   try
   {
      s.decode(vld(b));
   }
   catch (EncoderException const&)
   {
      throw MomParameterError{L"undecodable HLAunicodeString"};
   }
   return s.get();
}

std::string decBytes(std::string const& b)
{
   HLAopaqueData d;
   try
   {
      d.decode(vld(b));
   }
   catch (EncoderException const&)
   {
      throw MomParameterError{L"undecodable byte array"};
   }
   return std::string(reinterpret_cast<char const*>(d.get()), d.dataLength());
}

std::vector<std::uint64_t> decHandleList(std::string const& b)
{
   HLAvariableArray a{HLAopaqueData()};
   try
   {
      a.decode(vld(b));
   }
   catch (EncoderException const&)
   {
      throw MomParameterError{L"undecodable HLAhandleList"};
   }
   std::vector<std::uint64_t> out;
   for (size_t i = 0; i < a.size(); ++i)
      out.push_back(decHandle(bytesOf(a.get(i).encode())));
   return out;
}

std::wstring hostName()
{
#if defined(_WIN32)
   char* n = nullptr;
   size_t size = 0;
   if (_dupenv_s(&n, &size, "COMPUTERNAME") == 0 && n)
   {
      std::wstring const name = fromUtf8(n);
      std::free(n);
      return name;
   }
#else
   char n[256] = {0};
   if (gethostname(n, sizeof n - 1) == 0)
      return fromUtf8(n);
#endif
   return L"";
}

std::wstring const kRtiVersion = std::wstring(HLA_MQTT_RTI_NAME) + L" " + HLA_MQTT_RTI_VERSION;

void addCount(std::map<std::uint64_t, MomCounts>& m, std::uint64_t cls, bool bestEffort)
{
   ++(bestEffort ? m[cls].bestEffort : m[cls].reliable);
}

long long total(std::map<std::uint64_t, MomCounts> const& m)
{
   long long n = 0;
   for (auto const& kv : m)
      n += kv.second.reliable + kv.second.bestEffort;
   return n;
}
} // namespace

// ================================================================ counters
void MqttRTIambassador::momCountUpdate(std::uint64_t cls, std::uint64_t instance, bool bestEffort)
{
   addCount(_momUpdatesSent, cls, bestEffort);
   _momInstancesUpdated[cls].insert(instance);
}

void MqttRTIambassador::momCountReflection(std::uint64_t cls, std::uint64_t instance, bool bestEffort)
{
   addCount(_momReflections, cls, bestEffort);
   _momInstancesReflected[cls].insert(instance);
}

void MqttRTIambassador::momCountInteraction(bool sent, std::uint64_t cls, bool bestEffort)
{
   addCount(sent ? _momInteractionsSent : _momInteractionsReceived, cls, bestEffort);
}

void MqttRTIambassador::momAccountTime()
{
   auto const now = std::chrono::steady_clock::now();
   double const ms = std::chrono::duration<double, std::milli>(now - _momClock).count();
   _momClock = now;
   (_time.advance == Advance::None ? _momGrantedMs : _momAdvancingMs) += ms;
}

SupplementalReflectInfo MqttRTIambassador::reflectInfo(std::uint64_t sender) const
{
   return _conveyProducingFederate ? SupplementalReflectInfo(FederateHandleFriend::create(sender))
                                   : SupplementalReflectInfo();
}

SupplementalReceiveInfo MqttRTIambassador::receiveInfo(std::uint64_t sender) const
{
   return _conveyProducingFederate ? SupplementalReceiveInfo(FederateHandleFriend::create(sender))
                                   : SupplementalReceiveInfo();
}

// ======================================================= join / resign
void MqttRTIambassador::momJoined(std::vector<std::wstring> const& designators, std::vector<std::string> const& contents)
{
   std::string infoTopic, infoPayload;
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      _momUpdatesSent.clear();
      _momReflections.clear();
      _momInteractionsSent.clear();
      _momInteractionsReceived.clear();
      _momInstancesUpdated.clear();
      _momInstancesReflected.clear();
      _momDeleted = _momRemoved = 0;
      _momServiceReporting = _momExceptionReporting = false;
      _momSerial = 0;
      _momJoinModules = designators;
      _momJoinModuleData = contents;
      _conveyProducingFederate = _fdd->switchEnabled("conveyProducingFederate", false);
      _conveyRegionDesignatorSets = _fdd->switchEnabled("conveyRegionDesignatorSets", false);
      _autoProvide = _fdd->switchEnabled("autoProvide", false);
      _momClock = std::chrono::steady_clock::now();
      _momGrantedMs = _momAdvancingMs = 0;
      _momFederateObject = _momFederationObject = 0;
      _momInfoTopic.clear();
      _momInfoPayload.clear();

      if (Fdd::ObjectClass const* c = _fdd->objectClassByName(kFederateClass))
      {
         ObjectInstance o;
         o.handle = newUniqueValue("O");
         o.objectClass = c->handle;
         o.name = "HLAfederate." + toUtf8(_federateName);
         o.mom = true; // owner 0 here: the RTI, not the federate, owns it
         o.topic = _fedTopic + "/objects/" + classTopicPath(c->qualified) + "/@" + hex64(o.handle);
         _objects[o.handle] = o;
         _momFederateObject = o.handle;
         // Info record as seen by the others: owned by this federate.
         ObjectInstance published = o;
         published.owner = _federateHandle;
         published.ownerClient = _transport->settings().clientId;
         for (std::uint64_t k = c->handle; k != 0; k = _fdd->objectClass(k)->parent)
            for (std::uint64_t a : _fdd->objectClass(k)->attributes)
               published.owned.insert(a);
         infoTopic = o.topic + "/info";
         infoPayload = objectInfo(published);
         _momInfoTopic = infoTopic;
         _momInfoPayload = infoPayload;
      }
      if (Fdd::ObjectClass const* c = _fdd->objectClassByName(kFederationClass))
      {
         ObjectInstance o;
         o.handle = fnv1a64("MOM|HLAfederation|" + _fedTopic);
         o.objectClass = c->handle;
         o.name = "HLAfederation." + toUtf8(_federationName);
         o.mom = true;
         _objects[o.handle] = o;
         _momFederationObject = o.handle;
      }
   }
   if (!infoTopic.empty())
      _transport->publish(infoTopic, infoPayload, 1, true);
   momStopThread(); // left running by a lost connection
   {
      std::lock_guard<std::mutex> lock(_momThreadMutex);
      _momStop = false;
      _momPeriodChanged = false;
      _momReportPeriod = 0;
   }
   _momThread = std::thread([this] { momThreadLoop(); });
   updateBrokerSubscriptions(); // MOM request topics
}

void MqttRTIambassador::momStopThread()
{
   {
      std::lock_guard<std::mutex> lock(_momThreadMutex);
      _momStop = true;
   }
   _momCv.notify_all();
   if (_momThread.joinable() && _momThread.get_id() != std::this_thread::get_id())
      _momThread.join();
}

void MqttRTIambassador::momResigning()
{
   momStopThread();
   ObjectInstance o;
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      auto it = _objects.find(_momFederateObject);
      if (it == _objects.end())
         return;
      o = it->second;
      _objects.erase(it);
      _momFederateObject = 0;
   }
   try
   {
      deleteOwnObject(o, VariableLengthData(), std::string());
   }
   catch (std::runtime_error const&)
   {
   }
}

void MqttRTIambassador::momThreadLoop()
{
   std::unique_lock<std::mutex> lock(_momThreadMutex);
   for (;;)
   {
      if (_momReportPeriod <= 0)
         _momCv.wait(lock, [this] { return _momStop || _momPeriodChanged; });
      else
         _momCv.wait_for(lock, std::chrono::seconds(_momReportPeriod),
                         [this] { return _momStop || _momPeriodChanged; });
      if (_momStop)
         return;
      if (_momPeriodChanged)
      {
         _momPeriodChanged = false;
         continue;
      }
      lock.unlock();
      {
         std::lock_guard<std::mutex> v(_viewMutex);
         if (_fdd)
            momSendFederateUpdate(std::set<std::uint64_t>());
      }
      lock.lock();
   }
}

std::vector<std::string> MqttRTIambassador::momTopics() const
{
   std::vector<std::string> out;
   for (std::string const& q : {kFederateI + ".HLArequest", kFederateI + ".HLAadjust", kFederateI + ".HLAservice",
                                kFederationI + ".HLArequest", kFederationI + ".HLAadjust"})
      if (Fdd::InteractionClass const* c = _fdd->interactionClassByName(q))
         out.push_back(_fedTopic + "/interactions/" + classTopicPath(c->qualified) + "/#");
   return out;
}

bool MqttRTIambassador::momHandles(std::uint64_t cls) const
{
   Fdd::InteractionClass const* c = _fdd->interactionClass(cls);
   if (!c)
      return false;
   for (std::string const& q : {kFederateI + ".HLArequest.", kFederateI + ".HLAadjust.", kFederateI + ".HLAservice.",
                                kFederationI + ".HLArequest.", kFederationI + ".HLAadjust."})
      if (startsWith(c->qualified, q))
         return true;
   return false;
}

bool MqttRTIambassador::momElected(std::uint64_t excluded) const
{
   std::set<std::uint64_t> live = liveFederateHandles();
   live.erase(excluded);
   return !live.empty() && *live.begin() == _federateHandle;
}

// =========================================================== object values
std::string MqttRTIambassador::momTimeBytes(TimeValue const& t) const
{
   if (t.infinite)
      return std::string();
   return encodeTime(*t.toTime());
}

std::string MqttRTIambassador::momIntervalBytes(TimeValue const& t) const
{
   auto factory = HLAlogicalTimeFactoryFactory::makeLogicalTimeFactory(_timeImplementation);
   if (!factory.get())
      return std::string();
   auto interval = factory->makeZero();
   t.toInterval(*interval);
   return bytesOf(interval->encode());
}

std::map<std::string, VariableLengthData> MqttRTIambassador::momFederateValues()
{
   momAccountTime();
   std::map<std::string, VariableLengthData> v;
   v["HLAfederateHandle"] = encHandle(_federateHandle);
   v["HLAfederateName"] = encString(_federateName);
   v["HLAfederateType"] = encString(_federateType);
   v["HLAfederateHost"] = encString(hostName());
   v["HLARTIversion"] = encString(kRtiVersion);
   v["HLAFOMmoduleDesignatorList"] = encStringList(_momJoinModules);
   v["HLAtimeConstrained"] = encBool(_time.constrained);
   v["HLAtimeRegulating"] = encBool(_time.regulating);
   v["HLAasynchronousDelivery"] = encBool(_time.asyncDelivery);
   v["HLAfederateState"] = encInt(1); // ActiveFederate
   v["HLAtimeManagerState"] = encInt(_time.advance == Advance::None ? 0 : 1);
   v["HLAlogicalTime"] = encBytes(momTimeBytes(_time.time));
   v["HLAlookahead"] = encBytes(momIntervalBytes(_time.lookahead));
   Bound g;
   bool const hasG = galt(g);
   v["HLAGALT"] = encBytes(hasG ? momTimeBytes(g.value) : std::string());
   TimeValue lits = queueMin();
   if (hasG && g.value < lits)
      lits = g.value;
   v["HLALITS"] = encBytes(momTimeBytes(lits));
   v["HLAROlength"] = encInt(static_cast<long long>(_roHeld.size()));
   v["HLATSOlength"] = encInt(static_cast<long long>(_tsoQueue.size()));
   v["HLAreflectionsReceived"] = encInt(total(_momReflections));
   v["HLAupdatesSent"] = encInt(total(_momUpdatesSent));
   v["HLAinteractionsReceived"] = encInt(total(_momInteractionsReceived));
   v["HLAinteractionsSent"] = encInt(total(_momInteractionsSent));
   long long deletable = 0, registered = 0, discovered = 0, updated = 0, reflected = 0;
   for (auto const& kv : _objects)
   {
      ObjectInstance const& o = kv.second;
      if (o.owner == _federateHandle)
      {
         ++registered;
         Fdd::Attribute const* priv = _fdd->findAttribute(o.objectClass, "HLAprivilegeToDeleteObject");
         if (priv && o.owned.count(priv->handle))
            ++deletable;
      }
      else if (o.knownClass != 0 && !o.mom)
         ++discovered;
   }
   for (auto const& kv : _momInstancesUpdated)
      updated += static_cast<long long>(kv.second.size());
   for (auto const& kv : _momInstancesReflected)
      reflected += static_cast<long long>(kv.second.size());
   v["HLAobjectInstancesThatCanBeDeleted"] = encInt(deletable);
   v["HLAobjectInstancesUpdated"] = encInt(updated);
   v["HLAobjectInstancesReflected"] = encInt(reflected);
   v["HLAobjectInstancesDeleted"] = encInt(_momDeleted);
   v["HLAobjectInstancesRemoved"] = encInt(_momRemoved);
   v["HLAobjectInstancesRegistered"] = encInt(registered);
   v["HLAobjectInstancesDiscovered"] = encInt(discovered);
   // Time spent in each state since the previous report of these values.
   v["HLAtimeGrantedTime"] = encInt(static_cast<long long>(_momGrantedMs));
   v["HLAtimeAdvancingTime"] = encInt(static_cast<long long>(_momAdvancingMs));
   _momGrantedMs = _momAdvancingMs = 0;
   v["HLAconveyRegionDesignatorSets"] = encInt(_conveyRegionDesignatorSets ? 1 : 0);
   v["HLAconveyProducingFederate"] = encInt(_conveyProducingFederate ? 1 : 0);
   return v;
}

// Modules of the federation in merge order: (MIM hash, FOM module hashes,
// FOM module designators). FOM modules: those of the creation, then those
// added at join by the live federates.
void MqttRTIambassador::momModules(std::string& mimHash, std::wstring& mimDesignator, std::vector<std::string>& hashes,
                                   std::vector<std::wstring>& designators) const
{
   mimDesignator = L"HLAstandardMIM";
   auto it = _view.find(_fedTopic + "/execution");
   if (it != _view.end())
   {
      try
      {
         Json exec = Json::parse(it->second);
         mimHash = exec["mim"].str();
         if (!exec["mimDesignator"].str().empty())
            mimDesignator = fromUtf8(exec["mimDesignator"].str());
         std::vector<std::string> const h = exec["modules"].strings();
         std::vector<std::string> const d = exec["designators"].strings();
         for (std::string const& hash : h) // "designators" lists the FOM modules only
            if (hash != mimHash)
            {
               size_t const k = hashes.size();
               hashes.push_back(hash);
               designators.push_back(k < d.size() ? fromUtf8(d[k]) : fromUtf8(hash));
            }
      }
      catch (std::exception const&)
      {
      }
   }
   for (FederateRecord const& r : liveFederates(_fedTopic))
   {
      auto rec = _view.find(r.topic);
      if (rec == _view.end())
         continue;
      try
      {
         Json const record = Json::parse(rec->second); // kept alive for the loop
         for (Json const& m : record["modules"].items())
            if (std::find(hashes.begin(), hashes.end(), m["hash"].str()) == hashes.end())
            {
               hashes.push_back(m["hash"].str());
               designators.push_back(fromUtf8(m["designator"].str()));
            }
      }
      catch (std::exception const&)
      {
      }
   }
}

std::string MqttRTIambassador::momModuleContent(std::string const& hash) const
{
   auto it = _view.find(_fedTopic + "/fom/" + hash);
   return it == _view.end() ? std::string() : it->second;
}

std::map<std::string, VariableLengthData> MqttRTIambassador::momFederationValues() const
{
   std::string mimHash;
   std::wstring mimDesignator;
   std::vector<std::string> hashes;
   std::vector<std::wstring> designators;
   momModules(mimHash, mimDesignator, hashes, designators);
   std::string fdd = momModuleContent(mimHash);
   for (std::string const& h : hashes)
      fdd += momModuleContent(h);
   std::set<std::uint64_t> const live = liveFederateHandles();

   std::map<std::string, VariableLengthData> v;
   v["HLAfederationName"] = encString(_federationName);
   v["HLAfederatesInFederation"] = encHandleList(std::vector<std::uint64_t>(live.begin(), live.end()));
   v["HLARTIversion"] = encString(kRtiVersion);
   v["HLAMIMdesignator"] = encString(mimDesignator);
   v["HLAFOMmoduleDesignatorList"] = encStringList(designators);
   v["HLAcurrentFDD"] = encString(fromUtf8(fdd)); // the merged modules, MIM first
   v["HLAtimeImplementationName"] = encString(_timeImplementation);
   v["HLAlastSaveName"] = encString(L""); // save and restore: not implemented
   v["HLAlastSaveTime"] = encBytes(std::string());
   v["HLAnextSaveName"] = encString(L"");
   v["HLAnextSaveTime"] = encBytes(std::string());
   v["HLAautoProvide"] = encInt(_autoProvide ? 1 : 0);
   return v;
}

// Values of the attributes `attrs` (all when empty) of a local MOM entry.
AttributeHandleValueMap MqttRTIambassador::momValues(ObjectInstance const& o, std::set<std::uint64_t> const& attrs)
{
   std::map<std::string, VariableLengthData> const byName =
      o.handle == _momFederateObject ? momFederateValues() : momFederationValues();
   AttributeHandleValueMap values;
   for (auto const& kv : byName)
   {
      Fdd::Attribute const* a = _fdd->findAttribute(o.objectClass, kv.first);
      if (a && (attrs.empty() || attrs.count(a->handle)))
         values[AttributeHandleFriend::create(a->handle)] = kv.second;
   }
   return values;
}

void MqttRTIambassador::momReflectLocal(ObjectInstance const& o, AttributeHandleValueMap const& all)
{
   if (o.knownClass == 0)
      return;
   std::set<std::uint64_t> const& wanted = _subscribedAttributes[o.knownClass];
   AttributeHandleValueMap values;
   for (auto const& kv : all)
      if (wanted.count(AttributeHandleFriend::value(kv.first)))
         values.insert(kv);
   if (values.empty())
      return;
   ObjectInstanceHandle const h = ObjectInstanceHandleFriend::create(o.handle);
   SupplementalReflectInfo const info = reflectInfo(_federateHandle);
   deliverRO([h, values, info](FederateAmbassador& fa) {
      fa.reflectAttributeValues(h, values, VariableLengthData(), RECEIVE, RELIABLE, info);
   });
}

void MqttRTIambassador::momSendFederateUpdate(std::set<std::uint64_t> const& attrs)
{
   auto it = _objects.find(_momFederateObject);
   if (it == _objects.end())
      return;
   ObjectInstance const& o = it->second;
   AttributeHandleValueMap const values = momValues(o, attrs);
   if (values.empty())
      return;
   WireWriter w(WireKind::Update);
   w.u64(_federateHandle).u64(nextSeq()).u64(o.handle);
   w.bytes(nullptr, 0).u8(0).u64(0);
   w.u32(std::uint32_t(values.size()));
   for (auto const& kv : values)
      w.u64(AttributeHandleFriend::value(kv.first)).bytes(kv.second.data(), kv.second.size());
   try
   {
      _transport->publishAsync(o.topic + "/update", w.str(), 1, false);
   }
   catch (std::runtime_error const&)
   {
   }
   momReflectLocal(o, values); // a federate may watch its own HLAfederate object
}

void MqttRTIambassador::momProvide(ObjectInstance const& o, std::set<std::uint64_t> const& attrs)
{
   if (o.handle == _momFederateObject)
      momSendFederateUpdate(attrs);
   else
      momReflectLocal(o, momValues(o, attrs));
}

// ======================================================= reports (sending)
void MqttRTIambassador::momSend(std::string const& qualified,
                                std::vector<std::pair<std::string, VariableLengthData>> const& params)
{
   Fdd::InteractionClass const* ic = _fdd->interactionClassByName(qualified);
   if (!ic)
      return; // class missing from a non-standard MIM
   std::vector<std::pair<std::uint64_t, VariableLengthData const*>> values;
   for (auto const& p : params)
      if (Fdd::Parameter const* fp = _fdd->findParameter(ic->handle, p.first))
         values.emplace_back(fp->handle, &p.second);
   WireWriter w(WireKind::Interaction);
   w.u64(_federateHandle).u64(nextSeq()).u64(ic->handle);
   w.bytes(nullptr, 0).u8(0).u64(0);
   w.u32(std::uint32_t(values.size()));
   for (auto const& kv : values)
      w.u64(kv.first).bytes(kv.second->data(), kv.second->size());
   writeExtents(w, std::vector<Extents>());
   try
   {
      _transport->publishAsync(_fedTopic + "/interactions/" + classTopicPath(ic->qualified) + "/@", w.str(), 1, false);
   }
   catch (std::runtime_error const&)
   {
   }
   // The federate may ask itself: our own messages do not come back.
   std::uint64_t const received = receivedInteractionClass(ic->handle);
   if (received == 0)
      return;
   ParameterHandleValueMap local;
   for (auto const& kv : values)
      if (_fdd->classHasParameter(received, kv.first))
         local[ParameterHandleFriend::create(kv.first)] = *kv.second;
   InteractionClassHandle const h = InteractionClassHandleFriend::create(received);
   SupplementalReceiveInfo const info = receiveInfo(_federateHandle);
   deliverRO([h, local, info](FederateAmbassador& fa) {
      fa.receiveInteraction(h, local, VariableLengthData(), RECEIVE, RELIABLE, info);
   });
}

void MqttRTIambassador::momSendFederate(std::string const& report,
                                        std::vector<std::pair<std::string, VariableLengthData>> params)
{
   params.insert(params.begin(), std::make_pair(std::string("HLAfederate"), encHandle(_federateHandle)));
   momSend(kFederateI + ".HLAreport." + report, params);
}

void MqttRTIambassador::momException(std::wstring const& service, std::wstring const& what, bool parameterError)
{
   momSendFederate("HLAreportMOMexception", {{"HLAservice", encString(service)},
                                             {"HLAexception", encString(what)},
                                             {"HLAparameterError", encBool(parameterError)}});
}

void MqttRTIambassador::momReportCallbackException(std::wstring const& what)
{
   std::lock_guard<std::mutex> v(_viewMutex);
   if (!_fdd || !_momExceptionReporting)
      return;
   momSendFederate("HLAreportException", {{"HLAservice", encString(L"FederateAmbassador callback")},
                                          {"HLAexception", encString(what)}});
}

void MqttRTIambassador::momFederateLost(std::string const& clientId)
{
   if (!_fdd)
      return;
   for (auto const& kv : recordsUnder(_fedTopic + "/federates/"))
   {
      try
      {
         Json j = Json::parse(kv.second);
         if (j["clientId"].str() != clientId)
            continue;
         std::uint64_t const lost = std::strtoull(j["handle"].str().c_str(), nullptr, 16);
         if (lost == _federateHandle || !momElected(lost))
            return;
         momSend(kFederateI + ".HLAreport.HLAreportFederateLost",
                 {{"HLAfederate", encHandle(lost)},
                  {"HLAfederateName", encString(fromUtf8(j["name"].str()))},
                  {"HLAtimeStamp", encBytes(momTimeBytes(_time.time))},
                  {"HLAfaultDescription", encString(L"MQTT session lost without resign (Last Will)")}});
         return;
      }
      catch (std::exception const&)
      {
      }
   }
}

// ======================================================= requests (receiving)
namespace
{
struct MomParams
{
   Fdd const& fdd;
   std::uint64_t cls;
   std::map<std::uint64_t, std::string> const& values;

   std::string const& operator()(char const* name) const
   {
      Fdd::Parameter const* p = fdd.findParameter(cls, name);
      auto it = p ? values.find(p->handle) : values.end();
      if (it == values.end())
         throw MomParameterError{L"missing parameter " + fromUtf8(name)};
      return it->second;
   }
   bool has(char const* name) const
   {
      Fdd::Parameter const* p = fdd.findParameter(cls, name);
      return p && values.count(p->handle) && !values.at(p->handle).empty();
   }
};

AttributeHandleSet attributeSet(std::vector<std::uint64_t> const& handles)
{
   AttributeHandleSet out;
   for (std::uint64_t h : handles)
      out.insert(AttributeHandleFriend::create(h));
   return out;
}

TransportationType transportationOf(std::wstring const& name)
{
   if (name == L"HLAreliable")
      return RELIABLE;
   if (name == L"HLAbestEffort")
      return BEST_EFFORT;
   throw MomParameterError{L"unknown transportation " + name};
}

OrderType orderOf(long long v)
{
   if (v == 0)
      return RECEIVE;
   if (v == 1)
      return TIMESTAMP;
   throw MomParameterError{L"unknown order type " + std::to_wstring(v)};
}
} // namespace

void MqttRTIambassador::handleMomInteraction(std::uint64_t cls, std::map<std::uint64_t, std::string> const& params)
{
   Fdd::InteractionClass const* ic = _fdd->interactionClass(cls);
   if (!ic)
      return;
   MomParams const p{*_fdd, cls, params};
   std::string const& name = ic->name;
   bool const federation = startsWith(ic->qualified, kFederationI + ".");
   try
   {
      if (federation)
      {
         if (name == "HLAsetSwitches") // every federate applies it
         {
            _autoProvide = decInt(p("HLAautoProvide")) == 1;
            return;
         }
         if (!momElected(0))
            return;
         momFederationRequest(name, p.cls, params);
         return;
      }
      if (decHandle(p("HLAfederate")) != _federateHandle)
         return;
   }
   catch (MomParameterError const&)
   {
      return; // the target cannot be told apart: nobody answers
   }

   try
   {
      if (startsWith(ic->qualified, kFederateI + ".HLArequest."))
         momFederateRequest(name, p.cls, params);
      else if (startsWith(ic->qualified, kFederateI + ".HLAadjust."))
         momAdjust(name, p.cls, params);
      else
         momService(name, p.cls, params);
   }
   catch (MomParameterError const& e)
   {
      momException(fromUtf8(name), e.what, true);
   }
}

void MqttRTIambassador::momFederateRequest(std::string const& name, std::uint64_t cls,
                                           std::map<std::uint64_t, std::string> const& params)
{
   MomParams const p{*_fdd, cls, params};
   using Pairs = std::vector<std::pair<std::uint64_t, long long>>;
   auto classCounts = [](std::map<std::uint64_t, std::set<std::uint64_t>> const& m) {
      Pairs out;
      for (auto const& kv : m)
         out.emplace_back(kv.first, static_cast<long long>(kv.second.size()));
      return out;
   };
   auto byTransport = [](std::map<std::uint64_t, MomCounts> const& m, bool bestEffort) {
      Pairs out;
      for (auto const& kv : m)
      {
         std::uint32_t const n = bestEffort ? kv.second.bestEffort : kv.second.reliable;
         if (n)
            out.emplace_back(kv.first, n);
      }
      return out;
   };
   auto perTransport = [&](char const* report, char const* field, std::map<std::uint64_t, MomCounts> const& m) {
      for (bool be : {false, true})
         momSendFederate(report, {{"HLAtransportation", encString(be ? L"HLAbestEffort" : L"HLAreliable")},
                                  {field, encHandleIntList(byTransport(m, be))}});
   };

   if (name == "HLArequestPublications")
   {
      std::vector<std::pair<std::uint64_t, std::set<std::uint64_t>>> classes;
      for (auto const& kv : _publishedAttributes)
         if (!kv.second.empty())
            classes.emplace_back(kv.first, kv.second);
      for (auto const& c : classes)
         momSendFederate("HLAreportObjectClassPublication",
                         {{"HLAnumberOfClasses", encInt(static_cast<long long>(classes.size()))},
                          {"HLAobjectClass", encHandle(c.first)},
                          {"HLAattributeList", encHandleList(std::vector<std::uint64_t>(c.second.begin(), c.second.end()))}});
      momSendFederate("HLAreportInteractionPublication",
                      {{"HLAinteractionClassList", encHandleList(std::vector<std::uint64_t>(
                                                      _publishedInteractions.begin(), _publishedInteractions.end()))}});
   }
   else if (name == "HLArequestSubscriptions")
   {
      std::vector<std::pair<std::uint64_t, std::set<std::uint64_t>>> classes;
      for (auto const& kv : _subscribedAttributes)
         if (!kv.second.empty())
            classes.emplace_back(kv.first, kv.second);
      for (auto const& c : classes)
         momSendFederate("HLAreportObjectClassSubscription",
                         {{"HLAnumberOfClasses", encInt(static_cast<long long>(classes.size()))},
                          {"HLAobjectClass", encHandle(c.first)},
                          {"HLAactive", encBool(true)},
                          {"HLAmaxUpdateRate", encString(L"")},
                          {"HLAattributeList", encHandleList(std::vector<std::uint64_t>(c.second.begin(), c.second.end()))}});
      Pairs subs;
      for (std::uint64_t h : _subscribedInteractions)
         subs.emplace_back(h, 1);
      momSendFederate("HLAreportInteractionSubscription", {{"HLAinteractionClassList", encHandleIntList(subs)}});
   }
   else if (name == "HLArequestObjectInstancesThatCanBeDeleted")
   {
      std::map<std::uint64_t, std::set<std::uint64_t>> deletable;
      for (auto const& kv : _objects)
      {
         Fdd::Attribute const* priv = _fdd->findAttribute(kv.second.objectClass, "HLAprivilegeToDeleteObject");
         if (kv.second.owner == _federateHandle && priv && kv.second.owned.count(priv->handle))
            deletable[kv.second.objectClass].insert(kv.first);
      }
      momSendFederate("HLAreportObjectInstancesThatCanBeDeleted",
                      {{"HLAobjectInstanceCounts", encHandleIntList(classCounts(deletable))}});
   }
   else if (name == "HLArequestObjectInstancesUpdated")
      momSendFederate("HLAreportObjectInstancesUpdated",
                      {{"HLAobjectInstanceCounts", encHandleIntList(classCounts(_momInstancesUpdated))}});
   else if (name == "HLArequestObjectInstancesReflected")
      momSendFederate("HLAreportObjectInstancesReflected",
                      {{"HLAobjectInstanceCounts", encHandleIntList(classCounts(_momInstancesReflected))}});
   else if (name == "HLArequestUpdatesSent")
      perTransport("HLAreportUpdatesSent", "HLAupdateCounts", _momUpdatesSent);
   else if (name == "HLArequestReflectionsReceived")
      perTransport("HLAreportReflectionsReceived", "HLAreflectCounts", _momReflections);
   else if (name == "HLArequestInteractionsSent")
      perTransport("HLAreportInteractionsSent", "HLAinteractionCounts", _momInteractionsSent);
   else if (name == "HLArequestInteractionsReceived")
      perTransport("HLAreportInteractionsReceived", "HLAinteractionCounts", _momInteractionsReceived);
   else if (name == "HLArequestObjectInstanceInformation")
   {
      std::uint64_t const instance = decHandle(p("HLAobjectInstance"));
      auto it = _objects.find(instance);
      if (it == _objects.end() || (it->second.owner != _federateHandle && it->second.knownClass == 0))
         throw MomParameterError{L"object instance not known by the federate"};
      ObjectInstance const& o = it->second;
      std::vector<std::uint64_t> owned;
      if (o.owner == _federateHandle)
         owned.assign(o.owned.begin(), o.owned.end());
      momSendFederate("HLAreportObjectInstanceInformation",
                      {{"HLAobjectInstance", encHandle(instance)},
                       {"HLAownedInstanceAttributeList", encHandleList(owned)},
                       {"HLAregisteredClass", encHandle(o.objectClass)},
                       {"HLAknownClass", encHandle(o.owner == _federateHandle ? o.objectClass : o.knownClass)}});
   }
   else if (name == "HLArequestFOMmoduleData")
   {
      long long const index = decInt(p("HLAFOMmoduleIndicator"));
      if (index < 0 || index >= static_cast<long long>(_momJoinModuleData.size()))
         throw MomParameterError{L"no FOM module " + std::to_wstring(index) + L" supplied by this federate"};
      momSendFederate("HLAreportFOMmoduleData",
                      {{"HLAFOMmoduleIndicator", encInt(index)},
                       {"HLAFOMmoduleData", encString(fromUtf8(_momJoinModuleData[size_t(index)]))}});
   }
   else
      throw MomParameterError{L"request not handled by this RTI"};
}

void MqttRTIambassador::momFederationRequest(std::string const& name, std::uint64_t cls,
                                             std::map<std::uint64_t, std::string> const& params)
{
   MomParams const p{*_fdd, cls, params};
   std::string const report = kFederationI + ".HLAreport.";
   try
   {
      if (name == "HLArequestSynchronizationPoints")
      {
         std::vector<std::wstring> labels;
         for (auto const& kv : _syncPoints)
            if (kv.second.known && !kv.second.synchronized)
               labels.push_back(kv.second.label);
         momSend(report + "HLAreportSynchronizationPoints", {{"HLAsyncPoints", encStringList(labels)}});
      }
      else if (name == "HLArequestSynchronizationPointStatus")
      {
         for (auto const& kv : _syncPoints)
         {
            SyncPoint const& sp = kv.second;
            if (!sp.known || sp.synchronized)
               continue;
            std::vector<std::pair<std::uint64_t, long long>> status;
            for (std::uint64_t f : sp.members)
            {
               auto a = sp.achieved.find(f);
               status.emplace_back(f, a != sp.achieved.end() && a->second ? 3 : 2); // WaitingForRestOfFederation / MovingToSynchPoint
            }
            momSend(report + "HLAreportSynchronizationPointStatus",
                    {{"HLAsyncPointName", encString(sp.label)}, {"HLAsyncPointFederates", encHandleIntList(status)}});
         }
      }
      else if (name == "HLArequestFOMmoduleData")
      {
         long long const index = decInt(p("HLAFOMmoduleIndicator"));
         std::string mimHash;
         std::wstring mimDesignator;
         std::vector<std::string> hashes;
         std::vector<std::wstring> designators;
         momModules(mimHash, mimDesignator, hashes, designators);
         if (index < 0 || index >= static_cast<long long>(hashes.size()))
            throw MomParameterError{L"no FOM module " + std::to_wstring(index) + L" in the federation"};
         momSend(report + "HLAreportFOMmoduleData",
                 {{"HLAFOMmoduleIndicator", encInt(index)},
                  {"HLAFOMmoduleData", encString(fromUtf8(momModuleContent(hashes[size_t(index)])))}});
      }
      else if (name == "HLArequestMIMdata")
      {
         std::string mimHash;
         std::wstring mimDesignator;
         std::vector<std::string> hashes;
         std::vector<std::wstring> designators;
         momModules(mimHash, mimDesignator, hashes, designators);
         momSend(report + "HLAreportMIMdata", {{"HLAMIMdata", encString(fromUtf8(momModuleContent(mimHash)))}});
      }
   }
   catch (MomParameterError const& e)
   {
      // No federate to address: HLAreportMOMexception names the elected one.
      momException(fromUtf8(name), e.what, true);
   }
}

void MqttRTIambassador::momAdjust(std::string const& name, std::uint64_t cls,
                                  std::map<std::uint64_t, std::string> const& params)
{
   MomParams const p{*_fdd, cls, params};
   if (name == "HLAsetTiming")
   {
      long long const period = decInt(p("HLAreportPeriod"));
      if (period < 0)
         throw MomParameterError{L"negative report period"};
      {
         std::lock_guard<std::mutex> lock(_momThreadMutex);
         _momReportPeriod = int(period);
         _momPeriodChanged = true;
      }
      _momCv.notify_all();
   }
   else if (name == "HLAsetServiceReporting")
   {
      bool const on = decInt(p("HLAreportingState")) == 1;
      if (on)
      {
         // A federate subscribed to the reports would report its own reception.
         Fdd::InteractionClass const* report =
            _fdd->interactionClassByName(kFederateI + ".HLAreport.HLAreportServiceInvocation");
         if (report && receivedInteractionClass(report->handle) != 0)
            throw MomParameterError{L"the federate subscribes to HLAreportServiceInvocation"};
      }
      _momServiceReporting = on;
   }
   else if (name == "HLAsetExceptionReporting")
      _momExceptionReporting = decInt(p("HLAreportingState")) == 1;
   else if (name == "HLAsetSwitches")
   {
      if (p.has("HLAconveyRegionDesignatorSets"))
         _conveyRegionDesignatorSets = decInt(p("HLAconveyRegionDesignatorSets")) == 1;
      if (p.has("HLAconveyProducingFederate"))
         _conveyProducingFederate = decInt(p("HLAconveyProducingFederate")) == 1;
   }
   else if (name == "HLAmodifyAttributeState")
      momException(L"HLAmodifyAttributeState", L"ownership management is not implemented by this RTI", false);
   else
      throw MomParameterError{L"adjustment not handled by this RTI"};
}

// HLAservice: the service runs later on the federate thread (internal queue
// entry), as if the federate had called it.
void MqttRTIambassador::momService(std::string const& name, std::uint64_t cls,
                                   std::map<std::uint64_t, std::string> const& params)
{
   MomParams const p{*_fdd, cls, params};
   auto handle = [&](char const* n) { return decHandle(p(n)); };
   auto objectClass = [&](char const* n) { return ObjectClassHandleFriend::create(handle(n)); };
   auto interactionClass = [&](char const* n) { return InteractionClassHandleFriend::create(handle(n)); };
   auto instance = [&](char const* n) { return ObjectInstanceHandleFriend::create(handle(n)); };
   auto attributes = [&](char const* n) { return attributeSet(decHandleList(p(n))); };
   auto time = [&](char const* n) {
      std::shared_ptr<LogicalTime> t = decodeTime(decBytes(p(n)));
      if (!t)
         throw MomParameterError{L"undecodable logical time"};
      return t;
   };
   auto interval = [&](char const* n) {
      auto factory = HLAlogicalTimeFactoryFactory::makeLogicalTimeFactory(_timeImplementation);
      std::shared_ptr<LogicalTimeInterval> i;
      try
      {
         if (factory.get())
            i.reset(factory->decodeLogicalTimeInterval(vld(decBytes(p(n)))).release());
      }
      catch (Exception const&)
      {
      }
      if (!i)
         throw MomParameterError{L"undecodable time interval"};
      return i;
   };

   std::function<void()> call;
   std::wstring const service = fromUtf8(name.substr(3)); // HLAenableTimeConstrained -> enableTimeConstrained
   if (name == "HLAresignFederationExecution")
   {
      long long const a = decInt(p("HLAresignAction"));
      static ResignAction const actions[] = {UNCONDITIONALLY_DIVEST_ATTRIBUTES, DELETE_OBJECTS,
                                             CANCEL_PENDING_OWNERSHIP_ACQUISITIONS, DELETE_OBJECTS_THEN_DIVEST,
                                             CANCEL_THEN_DELETE_THEN_DIVEST, NO_ACTION};
      if (a < 1 || a > 6)
         throw MomParameterError{L"unknown resign action " + std::to_wstring(a)};
      ResignAction const action = actions[a - 1];
      call = [this, action] { resignFederationExecution(action); };
   }
   else if (name == "HLAsynchronizationPointAchieved")
   {
      std::wstring const label = decString(p("HLAlabel"));
      call = [this, label] { synchronizationPointAchieved(label, true); };
   }
   else if (name == "HLAfederateSaveBegun")
      call = [this] { federateSaveBegun(); };
   else if (name == "HLAfederateSaveComplete")
   {
      bool const ok = decInt(p("HLAsuccessIndicator")) == 1;
      call = [this, ok] { ok ? federateSaveComplete() : federateSaveNotComplete(); };
   }
   else if (name == "HLAfederateRestoreComplete")
   {
      bool const ok = decInt(p("HLAsuccessIndicator")) == 1;
      call = [this, ok] { ok ? federateRestoreComplete() : federateRestoreNotComplete(); };
   }
   else if (name == "HLApublishObjectClassAttributes")
   {
      auto const c = objectClass("HLAobjectClass");
      auto const a = attributes("HLAattributeList");
      call = [this, c, a] { publishObjectClassAttributes(c, a); };
   }
   else if (name == "HLAunpublishObjectClassAttributes")
   {
      auto const c = objectClass("HLAobjectClass");
      auto const a = attributes("HLAattributeList");
      call = [this, c, a] { unpublishObjectClassAttributes(c, a); };
   }
   else if (name == "HLApublishInteractionClass")
   {
      auto const c = interactionClass("HLAinteractionClass");
      call = [this, c] { publishInteractionClass(c); };
   }
   else if (name == "HLAunpublishInteractionClass")
   {
      auto const c = interactionClass("HLAinteractionClass");
      call = [this, c] { unpublishInteractionClass(c); };
   }
   else if (name == "HLAsubscribeObjectClassAttributes")
   {
      auto const c = objectClass("HLAobjectClass");
      auto const a = attributes("HLAattributeList");
      bool const active = !p.has("HLAactive") || decInt(p("HLAactive")) == 1;
      call = [this, c, a, active] { subscribeObjectClassAttributes(c, a, active, L""); };
   }
   else if (name == "HLAunsubscribeObjectClassAttributes")
   {
      auto const c = objectClass("HLAobjectClass");
      auto const a = attributes("HLAattributeList");
      call = [this, c, a] { unsubscribeObjectClassAttributes(c, a); };
   }
   else if (name == "HLAsubscribeInteractionClass")
   {
      auto const c = interactionClass("HLAinteractionClass");
      bool const active = !p.has("HLAactive") || decInt(p("HLAactive")) == 1;
      call = [this, c, active] { subscribeInteractionClass(c, active); };
   }
   else if (name == "HLAunsubscribeInteractionClass")
   {
      auto const c = interactionClass("HLAinteractionClass");
      call = [this, c] { unsubscribeInteractionClass(c); };
   }
   else if (name == "HLAdeleteObjectInstance")
   {
      auto const o = instance("HLAobjectInstance");
      VariableLengthData const tag = p.has("HLAtag") ? vld(decBytes(p("HLAtag"))) : VariableLengthData();
      std::shared_ptr<LogicalTime> const t =
         p.has("HLAtimeStamp") && !decBytes(p("HLAtimeStamp")).empty() ? time("HLAtimeStamp") : nullptr;
      call = [this, o, tag, t] {
         if (t)
            deleteObjectInstance(o, tag, *t);
         else
            deleteObjectInstance(o, tag);
      };
   }
   else if (name == "HLAlocalDeleteObjectInstance")
   {
      auto const o = instance("HLAobjectInstance");
      call = [this, o] { localDeleteObjectInstance(o); };
   }
   else if (name == "HLArequestAttributeTransportationTypeChange")
   {
      auto const o = instance("HLAobjectInstance");
      auto const a = attributes("HLAattributeList");
      TransportationType const t = transportationOf(decString(p("HLAtransportation")));
      call = [this, o, a, t] { requestAttributeTransportationTypeChange(o, a, t); };
   }
   else if (name == "HLArequestInteractionTransportationTypeChange")
   {
      auto const c = interactionClass("HLAinteractionClass");
      TransportationType const t = transportationOf(decString(p("HLAtransportation")));
      call = [this, c, t] { requestInteractionTransportationTypeChange(c, t); };
   }
   else if (name == "HLAunconditionalAttributeOwnershipDivestiture")
   {
      auto const o = instance("HLAobjectInstance");
      auto const a = attributes("HLAattributeList");
      call = [this, o, a] { unconditionalAttributeOwnershipDivestiture(o, a); };
   }
   else if (name == "HLAenableTimeRegulation")
   {
      auto const i = interval("HLAlookahead");
      call = [this, i] { enableTimeRegulation(*i); };
   }
   else if (name == "HLAdisableTimeRegulation")
      call = [this] { disableTimeRegulation(); };
   else if (name == "HLAenableTimeConstrained")
      call = [this] { enableTimeConstrained(); };
   else if (name == "HLAdisableTimeConstrained")
      call = [this] { disableTimeConstrained(); };
   else if (name == "HLAtimeAdvanceRequest")
   {
      auto const t = time("HLAtimeStamp");
      call = [this, t] { timeAdvanceRequest(*t); };
   }
   else if (name == "HLAtimeAdvanceRequestAvailable")
   {
      auto const t = time("HLAtimeStamp");
      call = [this, t] { timeAdvanceRequestAvailable(*t); };
   }
   else if (name == "HLAnextMessageRequest")
   {
      auto const t = time("HLAtimeStamp");
      call = [this, t] { nextMessageRequest(*t); };
   }
   else if (name == "HLAnextMessageRequestAvailable")
   {
      auto const t = time("HLAtimeStamp");
      call = [this, t] { nextMessageRequestAvailable(*t); };
   }
   else if (name == "HLAflushQueueRequest")
   {
      auto const t = time("HLAtimeStamp");
      call = [this, t] { flushQueueRequest(*t); };
   }
   else if (name == "HLAenableAsynchronousDelivery")
      call = [this] { enableAsynchronousDelivery(); };
   else if (name == "HLAdisableAsynchronousDelivery")
      call = [this] { disableAsynchronousDelivery(); };
   else if (name == "HLAmodifyLookahead")
   {
      auto const i = interval("HLAlookahead");
      call = [this, i] { modifyLookahead(*i); };
   }
   else if (name == "HLAchangeAttributeOrderType")
   {
      auto const o = instance("HLAobjectInstance");
      auto const a = attributes("HLAattributeList");
      OrderType const t = orderOf(decInt(p("HLAsendOrder")));
      call = [this, o, a, t] { changeAttributeOrderType(o, a, t); };
   }
   else if (name == "HLAchangeInteractionOrderType")
   {
      auto const c = interactionClass("HLAinteractionClass");
      OrderType const t = orderOf(decInt(p("HLAsendOrder")));
      call = [this, c, t] { changeInteractionOrderType(c, t); };
   }
   else
      throw MomParameterError{L"service not handled by this RTI"};

   enqueueInternal([this, call, service] {
      std::wstring exception;
      try
      {
         call();
      }
      catch (Exception const& e)
      {
         exception = e.what();
      }
      catch (std::exception const& e)
      {
         exception = fromUtf8(e.what());
      }
      std::lock_guard<std::mutex> v(_viewMutex);
      if (!_fdd)
         return; // resigned by the service itself
      if (!exception.empty())
         momException(L"HLA" + service, exception, false);
      if (_momServiceReporting)
         momSendFederate("HLAreportServiceInvocation",
                         {{"HLAservice", encString(service)},
                          {"HLAsuccessIndicator", encBool(exception.empty())},
                          {"HLAsuppliedArguments", encStringList({})},
                          {"HLAreturnedArguments", encStringList({})},
                          {"HLAexception", encString(exception)},
                          {"HLAserialNumber", encInt(++_momSerial)}});
   });
}
} // namespace hla_mqtt
