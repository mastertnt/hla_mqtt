// Federation management (IEEE 1516.1-2010, 4.5 to 4.10) without a central RTI.
//
// Retained topics (P = prefix, F = federation name, percent-encoded):
//   P/clients/<clientId>              presence (jalon 1); "offline" by Last Will
//   P/federations/F/execution         {"name","timeImplementation","modules":[hash..],...}
//   P/federations/F/fom/<hash>        raw XML of a FOM module (hash = FNV-1a 64 of the octets)
//   P/federations/F/claims/<clientId> creation claim (transient)
//   P/federations/F/federates/<hex>   {"name","type","handle","clientId","state"}
//
// A record whose clientId has no "online" presence is stale (federate or
// creator crashed): it is ignored, and removed by destroy.
//
// Exclusive operations (create a federation, take a federate name) use
// claim + barrier: publish the claim, make a broker round trip, then look at
// the competitors. Whoever sees a competitor withdraws, waits a random delay
// and retries. Two competitors may both withdraw (then retry), never both win.
//
// Federate handle: FNV-1a 64 of "F|<clientId>" (unique as long as client ids are).
// Object/interaction/attribute/parameter/dimension handles: see Fdd.h.

#include "MqttRTIambassador.h"

#include "Fdd.h"
#include "Json.h"
#include "HandleFriends.h"
#include "Log.h"
#include "MqttTransport.h"
#include "Utf.h"
#include "Version.h"
#include "Wire.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

namespace hla_mqtt
{
namespace embedded
{
extern unsigned char const standardMim[]; // mim/HLAstandardMIM.xml, see cmake/EmbedFile.cmake
extern unsigned long long const standardMimSize;
} // namespace embedded

namespace
{
int const kMaxAttempts = 8;

bool startsWith(std::string const& s, std::string const& prefix)
{
   return s.compare(0, prefix.size(), prefix) == 0;
}

// FOM/MIM designator: a file path or a file: URL. Returns the octets.
std::string readModule(std::wstring const& designator)
{
   std::wstring path = designator;
   if (path.compare(0, 8, L"file:///") == 0)
   {
      path = path.substr(8);
#if !defined(_WIN32)
      path = L"/" + path;
#endif
   }
   else if (path.compare(0, 7, L"file://") == 0)
      path = path.substr(7);
   else if (path.find(L"://") != std::wstring::npos)
      throw std::runtime_error("unsupported designator (only files): " + toUtf8(designator));

   std::ifstream in(std::filesystem::path(path), std::ios::binary);
   if (!in)
      throw std::runtime_error("cannot open " + toUtf8(designator));
   std::ostringstream content;
   content << in.rdbuf();
   if (in.bad())
      throw std::runtime_error("cannot read " + toUtf8(designator));
   return content.str();
}

struct Module
{
   std::string label;
   std::string content;
   std::string hash;
};

// IEEE 1516.1-2010 standard MIM, built into the library.
// "Reprinted with permission from IEEE 1516.1(TM)-2010".
Module standardMimModule()
{
   Module m;
   m.label = "HLAstandardMIM";
   m.content.assign(reinterpret_cast<char const*>(embedded::standardMim), size_t(embedded::standardMimSize));
   m.hash = hex64(fnv1a64(m.content));
   return m;
}

Module loadModule(std::wstring const& designator)
{
   Module m;
   m.label = toUtf8(designator);
   m.content = readModule(designator);
   m.hash = hex64(fnv1a64(m.content));
   return m;
}

// FddError -> standard exception (FDD or MIM flavour).
[[noreturn]] void throwFddError(FddError const& e, bool mim)
{
   std::wstring const what = fromUtf8(e.what());
   if (e.kind == FddError::Inconsistent)
      throw InconsistentFDD(what);
   if (mim)
      throw ErrorReadingMIM(what);
   throw ErrorReadingFDD(what);
}

std::string presenceOnline(std::string const& payload)
{
   try
   {
      return Json::parse(payload)["state"].str();
   }
   catch (std::exception const&)
   {
      return std::string();
   }
}
} // namespace

// ------------------------------------------------------------ view helpers
void MqttRTIambassador::onMessage(std::string const& topic, std::string const& payload)
{
   std::lock_guard<std::mutex> lock(_viewMutex);
   if (_resyncing)
      _resyncSeen.insert(topic);
   // Object and interaction traffic of the joined federation: not mirrored.
   if (_fdd && !_fedTopic.empty() && startsWith(topic, _fedTopic + "/"))
   {
      std::string const rest = topic.substr(_fedTopic.size() + 1);
      if (startsWith(rest, "objects/") || startsWith(rest, "interactions/") || startsWith(rest, "requests/") ||
          startsWith(rest, "classrequests/"))
      {
         handleObjectTraffic(rest, payload);
         return;
      }
      if (startsWith(rest, "ownership/"))
      {
         handleOwnershipTraffic(rest, payload);
         return;
      }
      if (startsWith(rest, "owners/"))
      {
         // Mirrored: the resynchronization after a reconnection sees the records gone.
         if (payload.empty())
            _view.erase(topic);
         else
            _view[topic] = payload;
         handleOwnershipTraffic(rest, payload);
         return;
      }
      if (startsWith(rest, "decl/"))
      {
         handleDeclarations(rest.substr(5), payload);
         return;
      }
      if (startsWith(rest, "time/"))
      {
         handleTimeState(rest.substr(5), payload);
         return;
      }
   }
   // Presence change while joined: objects of a vanished federate are removed.
   // (_fdd is only set while joined, hence while _transport exists.)
   std::string lostClient;
   if (_fdd && _transport)
   {
      std::string const presencePrefix = _transport->settings().topicPrefix + "/clients/";
      if (startsWith(topic, presencePrefix) && handlePresence(topic.substr(presencePrefix.size()), payload))
         lostClient = topic.substr(presencePrefix.size());
   }
   if (payload.empty())
      _view.erase(topic);
   else
      _view[topic] = payload;
   if (!lostClient.empty())
      ownershipPresenceLost(lostClient); // presence now offline: its records are dead

   if (_fdd && !_fedTopic.empty() && startsWith(topic, _fedTopic + "/"))
   {
      std::string const rest = topic.substr(_fedTopic.size() + 1);
      if (startsWith(rest, "sync/"))
         handleSyncTraffic(rest, payload);
      else if (startsWith(rest, "save/"))
         handleSaveTraffic(rest, payload);
      else if (startsWith(rest, "restore/"))
         handleRestoreTraffic(rest, payload);
      else if (startsWith(rest, "federates/"))
      {
         // A federate joined or left: synchronization sets, GALT, save and restore members.
         evaluateAllSync();
         evaluateTime();
         evaluateAdvisories();
         evaluateSave();
         evaluateRestore();
      }
   }
   if (_fdd && _transport && startsWith(topic, _transport->settings().topicPrefix + "/clients/"))
   {
      evaluateAllSync();
      evaluateTime();
      evaluateAdvisories();
      evaluateSave();
      evaluateRestore();
   }

   // Module added by another federate of our federation: the FDD only grows.
   if (_fdd && !payload.empty() && startsWith(topic, _fedTopic + "/fom/"))
   {
      std::string const hash = topic.substr(_fedTopic.size() + 5);
      if (_mergedModules.insert(hash).second)
      {
         try
         {
            _fdd->merge(payload, "module " + hash);
         }
         catch (FddError const&)
         {
            // Validated by the federate that added it; nothing better to do here.
         }
      }
   }
}

std::map<std::string, std::string> MqttRTIambassador::recordsUnder(std::string const& topicPrefix) const
{
   std::map<std::string, std::string> out;
   for (auto it = _view.lower_bound(topicPrefix); it != _view.end() && startsWith(it->first, topicPrefix); ++it)
      out.insert(*it);
   return out;
}

bool MqttRTIambassador::clientAlive(std::string const& clientId) const
{
   if (_transport && clientId == _transport->settings().clientId)
      return true;
   auto it = _view.find(_transport->settings().topicPrefix + "/clients/" + clientId);
   return it != _view.end() && presenceOnline(it->second) == "online";
}

std::vector<MqttRTIambassador::FederateRecord> MqttRTIambassador::liveFederates(std::string const& fedTopic) const
{
   std::vector<FederateRecord> out;
   for (auto const& kv : recordsUnder(fedTopic + "/federates/"))
   {
      try
      {
         Json j = Json::parse(kv.second);
         FederateRecord r;
         r.name = j["name"].str();
         r.type = j["type"].str();
         r.clientId = j["clientId"].str();
         r.state = j["state"].str();
         r.handle = std::strtoull(j["handle"].str().c_str(), nullptr, 16);
         r.topic = kv.first;
         if (clientAlive(r.clientId))
            out.push_back(r);
      }
      catch (std::exception const&)
      {
      }
   }
   return out;
}

void MqttRTIambassador::forgetUnder(std::string const& topicPrefix)
{
   auto it = _view.lower_bound(topicPrefix);
   while (it != _view.end() && startsWith(it->first, topicPrefix))
      it = _view.erase(it);
}

void MqttRTIambassador::checkConnected() const
{
   if (!_connected)
      throw NotConnected(L"not connected to the RTI (call connect first)");
}

void MqttRTIambassador::checkJoined() const
{
   checkConnected();
   if (!_joined)
      throw FederateNotExecutionMember(L"not joined to a federation execution");
}

std::string MqttRTIambassador::federationTopic(std::wstring const& federationName) const
{
   return _transport->settings().topicPrefix + "/federations/" + encodeTopicLevel(toUtf8(federationName));
}

void MqttRTIambassador::backoff(int attempt)
{
   std::uniform_int_distribution<int> ms(20, 100 * (attempt + 1));
   std::this_thread::sleep_for(std::chrono::milliseconds(ms(_random)));
}

// ------------------------------------------------------------------ 4.5
void MqttRTIambassador::createFederation(std::wstring const& federationName,
                                         std::vector<std::wstring> const& fomModules,
                                         std::wstring const* mimModule,
                                         std::wstring const& logicalTimeImplementationName)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkConnected();
   if (federationName.empty())
      throw RTIinternalError(L"empty federation execution name");

   // Local checks first: time representation, then the modules.
   if (!HLAlogicalTimeFactoryFactory::makeLogicalTimeFactory(logicalTimeImplementationName).get())
      throw CouldNotCreateLogicalTimeFactory(L"unknown logical time implementation '" +
                                             logicalTimeImplementationName + L"'");
   std::wstring const timeName = logicalTimeImplementationName.empty() ? L"HLAfloat64Time"
                                                                        : logicalTimeImplementationName;

   std::vector<Module> modules;
   std::vector<std::string> designators; // of the FOM modules (MOM HLAFOMmoduleDesignatorList)
   std::wstring mimDesignator = L"HLAstandardMIM";
   Fdd fdd;
   if (mimModule)
   {
      if (*mimModule == L"HLAstandardMIM")
         throw DesignatorIsHLAstandardMIM(L"HLAstandardMIM is implicit, pass no MIM to use it");
      Module m;
      try
      {
         m = loadModule(*mimModule);
      }
      catch (std::exception const& e)
      {
         throw CouldNotOpenMIM(fromUtf8(e.what()));
      }
      try
      {
         fdd.merge(m.content, m.label);
      }
      catch (FddError const& e)
      {
         throwFddError(e, true);
      }
      modules.push_back(m);
      mimDesignator = *mimModule;
   }
   else
   {
      // No MIM given: the standard MIM, published like any other module.
      Module const m = standardMimModule();
      try
      {
         fdd.merge(m.content, m.label);
      }
      catch (FddError const& e)
      {
         throwFddError(e, true);
      }
      modules.push_back(m);
   }
   if (fomModules.empty())
      throw CouldNotOpenFDD(L"no FOM module given");
   for (std::wstring const& d : fomModules)
   {
      Module m;
      try
      {
         m = loadModule(d);
      }
      catch (std::exception const& e)
      {
         throw CouldNotOpenFDD(fromUtf8(e.what()));
      }
      try
      {
         fdd.merge(m.content, m.label);
      }
      catch (FddError const& e)
      {
         throwFddError(e, false);
      }
      modules.push_back(m);
      designators.push_back(m.label);
   }

   std::string const ft = federationTopic(federationName);
   std::string const myId = _transport->settings().clientId;
   std::string const claimTopic = ft + "/claims/" + encodeTopicLevel(myId);
   bool const keepSubscription = _joined && ft == _fedTopic;

   try
   {
      _transport->subscribe(ft + "/#");
      for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
      {
         _transport->barrier();
         bool exists;
         {
            std::lock_guard<std::mutex> v(_viewMutex);
            exists = _view.count(ft + "/execution") != 0;
         }
         if (exists)
         {
            _transport->unsubscribe(ft + "/#");
            if (!keepSubscription)
            {
               std::lock_guard<std::mutex> v(_viewMutex);
               forgetUnder(ft + "/");
            }
            throw FederationExecutionAlreadyExists(federationName);
         }
         Json claim = Json::object();
         claim.set("clientId", myId);
         _transport->publish(claimTopic, claim.dump(), 1, true);
         _transport->barrier();

         bool contended = false;
         {
            std::lock_guard<std::mutex> v(_viewMutex);
            for (auto const& kv : recordsUnder(ft + "/claims/"))
            {
               if (kv.first == claimTopic)
                  continue;
               std::string other;
               try
               {
                  other = Json::parse(kv.second)["clientId"].str();
               }
               catch (std::exception const&)
               {
               }
               if (clientAlive(other))
                  contended = true;
            }
            contended = contended || _view.count(ft + "/execution") != 0;
         }
         if (contended)
         {
            _transport->publish(claimTopic, std::string(), 1, true);
            backoff(attempt);
            continue;
         }

         // We own the name: modules first, then the execution record, then
         // release the claim (competitors keep backing off until then).
         Json record = Json::object();
         Json hashes = Json::array();
         for (Module const& m : modules)
         {
            _transport->publish(ft + "/fom/" + m.hash, m.content, 1, true);
            hashes.push(m.hash);
         }
         record.set("name", toUtf8(federationName));
         record.set("timeImplementation", toUtf8(timeName));
         record.set("modules", hashes);
         record.set("mim", modules.front().hash); // the MIM is merged first
         record.set("mimDesignator", toUtf8(mimDesignator));
         Json names = Json::array();
         for (std::string const& d : designators)
            names.push(d);
         record.set("designators", names);
         record.set("createdBy", myId);
         record.set("rti", toUtf8(HLA_MQTT_RTI_NAME));
         record.set("version", toUtf8(HLA_MQTT_RTI_VERSION));
         _transport->publish(ft + "/execution", record.dump(), 1, true);
         _transport->publish(claimTopic, std::string(), 1, true);
         _transport->unsubscribe(ft + "/#");
         if (!keepSubscription)
         {
            std::lock_guard<std::mutex> v(_viewMutex);
            forgetUnder(ft + "/");
         }
         return;
      }
   }
   catch (std::runtime_error const& e)
   {
      throw RTIinternalError(fromUtf8(e.what()));
   }
   throw RTIinternalError(L"createFederationExecution: too much contention on '" + federationName + L"'");
}

void MqttRTIambassador::createFederationExecution(std::wstring const& federationExecutionName,
                                                  std::wstring const& fomModule,
                                                  std::wstring const& logicalTimeImplementationName)
{
   createFederation(federationExecutionName, std::vector<std::wstring>(1, fomModule), nullptr,
                    logicalTimeImplementationName);
}

void MqttRTIambassador::createFederationExecution(std::wstring const& federationExecutionName,
                                                  std::vector<std::wstring> const& fomModules,
                                                  std::wstring const& logicalTimeImplementationName)
{
   createFederation(federationExecutionName, fomModules, nullptr, logicalTimeImplementationName);
}

void MqttRTIambassador::createFederationExecutionWithMIM(std::wstring const& federationExecutionName,
                                                         std::vector<std::wstring> const& fomModules,
                                                         std::wstring const& mimModule,
                                                         std::wstring const& logicalTimeImplementationName)
{
   createFederation(federationExecutionName, fomModules, &mimModule, logicalTimeImplementationName);
}

// ------------------------------------------------------------------ 4.6
void MqttRTIambassador::destroyFederationExecution(std::wstring const& federationExecutionName)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkConnected();
   std::string const ft = federationTopic(federationExecutionName);
   bool const keepSubscription = _joined && ft == _fedTopic;

   auto cleanup = [&] {
      _transport->unsubscribe(ft + "/#");
      if (!keepSubscription)
      {
         std::lock_guard<std::mutex> v(_viewMutex);
         forgetUnder(ft + "/");
      }
   };

   try
   {
      _transport->subscribe(ft + "/#");
      _transport->barrier();
      std::string execution;
      bool joined = false;
      {
         std::lock_guard<std::mutex> v(_viewMutex);
         auto it = _view.find(ft + "/execution");
         if (it != _view.end())
            execution = it->second;
         joined = !liveFederates(ft).empty();
      }
      if (execution.empty())
      {
         cleanup();
         throw FederationExecutionDoesNotExist(federationExecutionName);
      }
      if (joined)
      {
         cleanup();
         throw FederatesCurrentlyJoined(federationExecutionName);
      }
      // Remove the execution record, then check again: a federate joining
      // concurrently either shows up now, or will see the record gone.
      _transport->publish(ft + "/execution", std::string(), 1, true);
      _transport->barrier();
      std::map<std::string, std::string> leftovers;
      {
         std::lock_guard<std::mutex> v(_viewMutex);
         joined = !liveFederates(ft).empty();
         leftovers = recordsUnder(ft + "/");
      }
      if (joined)
      {
         _transport->publish(ft + "/execution", execution, 1, true);
         cleanup();
         throw FederatesCurrentlyJoined(federationExecutionName);
      }
      for (auto const& kv : leftovers) // FOM modules, stale federates and claims
         _transport->publish(kv.first, std::string(), 1, true);
      _transport->barrier();
      cleanup();
   }
   catch (std::runtime_error const& e)
   {
      throw RTIinternalError(fromUtf8(e.what()));
   }
}

// ------------------------------------------------------------------ 4.7
void MqttRTIambassador::listFederationExecutions()
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkConnected();
   std::string const prefix = _transport->settings().topicPrefix + "/federations/";
   std::string const filter = prefix + "+/execution";
   FederationExecutionInformationVector list;
   try
   {
      _transport->subscribe(filter);
      _transport->barrier();
      std::lock_guard<std::mutex> v(_viewMutex);
      for (auto const& kv : recordsUnder(prefix))
      {
         if (kv.first.size() < 10 || kv.first.compare(kv.first.size() - 10, 10, "/execution") != 0)
            continue;
         try
         {
            Json j = Json::parse(kv.second);
            list.push_back(FederationExecutionInformation(fromUtf8(j["name"].str()),
                                                          fromUtf8(j["timeImplementation"].str())));
         }
         catch (std::exception const&)
         {
         }
      }
   }
   catch (std::runtime_error const& e)
   {
      throw RTIinternalError(fromUtf8(e.what()));
   }
   _transport->unsubscribe(filter);
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      // Keep the records of the joined federation, drop the others.
      std::string const keep = _joined ? _fedTopic + "/" : std::string("\x01");
      for (auto it = _view.lower_bound(prefix); it != _view.end() && startsWith(it->first, prefix);)
      {
         bool const isExecution =
            it->first.size() >= 10 && it->first.compare(it->first.size() - 10, 10, "/execution") == 0;
         if (isExecution && !startsWith(it->first, keep))
            it = _view.erase(it);
         else
            ++it;
      }
   }
   enqueue([list](FederateAmbassador& fa) { fa.reportFederationExecutions(list); });
}

// ------------------------------------------------------------------ 4.9
FederateHandle MqttRTIambassador::joinFederation(std::wstring const& federateName,
                                                 std::wstring const& federateType,
                                                 std::wstring const& federationName,
                                                 std::vector<std::wstring> const& additionalFomModules)
{
   checkNotInCallback();
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkConnected();
   if (_joined)
      throw FederateAlreadyExecutionMember(L"already joined to " + _federationName);

   // Additional modules are read before talking to the broker.
   std::vector<Module> extra;
   for (std::wstring const& d : additionalFomModules)
   {
      try
      {
         extra.push_back(loadModule(d));
      }
      catch (std::exception const& e)
      {
         throw CouldNotOpenFDD(fromUtf8(e.what()));
      }
   }

   std::string const ft = federationTopic(federationName);
   std::string const myId = _transport->settings().clientId;
   std::uint64_t const handle = fnv1a64("F|" + myId);
   std::string const recordTopic = ft + "/federates/" + hex64(handle);
   std::string const name = federateName.empty() ? "HLAfederate-" + hex64(handle) : toUtf8(federateName);

   auto abandon = [&] {
      _transport->unsubscribe(ft + "/#");
      std::lock_guard<std::mutex> v(_viewMutex);
      forgetUnder(ft + "/");
   };

   try
   {
      _transport->subscribe(ft + "/#");
      _transport->barrier();

      // FDD of the federation: modules of the execution record first, then
      // the modules added by joined federates, then ours.
      std::shared_ptr<Fdd> fdd = std::make_shared<Fdd>();
      std::set<std::string> merged;
      std::wstring timeName;
      std::string execution;
      std::map<std::string, std::string> foms;
      {
         std::lock_guard<std::mutex> v(_viewMutex);
         auto it = _view.find(ft + "/execution");
         if (it != _view.end())
            execution = it->second;
         foms = recordsUnder(ft + "/fom/");
      }
      if (execution.empty())
      {
         abandon();
         throw FederationExecutionDoesNotExist(federationName);
      }
      Json exec = Json::parse(execution);
      timeName = fromUtf8(exec["timeImplementation"].str());
      std::vector<std::string> order = exec["modules"].strings();
      for (auto const& kv : foms)
      {
         std::string const h = kv.first.substr(ft.size() + 5);
         if (std::find(order.begin(), order.end(), h) == order.end())
            order.push_back(h);
      }
      for (std::string const& h : order)
      {
         auto m = foms.find(ft + "/fom/" + h);
         if (m == foms.end())
         {
            abandon();
            throw RTIinternalError(L"FOM module " + fromUtf8(h) + L" of the federation is missing");
         }
         if (merged.insert(h).second)
            fdd->merge(m->second, "module " + h);
      }
      for (Module const& m : extra)
      {
         try
         {
            if (merged.insert(m.hash).second)
               fdd->merge(m.content, m.label);
         }
         catch (FddError const& e)
         {
            abandon();
            throwFddError(e, false);
         }
      }
      if (!HLAlogicalTimeFactoryFactory::makeLogicalTimeFactory(timeName).get())
      {
         abandon();
         throw CouldNotCreateLogicalTimeFactory(L"unknown logical time implementation '" + timeName + L"'");
      }

      Json record = Json::object();
      record.set("name", name);
      record.set("type", toUtf8(federateType));
      record.set("handle", hex64(handle));
      record.set("clientId", myId);
      Json joinModules = Json::array(); // MOM: modules supplied at join
      for (Module const& m : extra)
      {
         Json jm = Json::object();
         jm.set("designator", m.label);
         jm.set("hash", m.hash);
         joinModules.push(jm);
      }
      record.set("modules", joinModules);

      for (int attempt = 0;; ++attempt)
      {
         bool taken = false;
         {
            std::lock_guard<std::mutex> v(_viewMutex);
            // Only a completed join owns the name; "joining" records are
            // competitors, settled by the claim below.
            for (FederateRecord const& r : liveFederates(ft))
               if (r.name == name && r.handle != handle && r.state == "joined")
                  taken = true;
         }
         if (taken)
         {
            abandon();
            throw FederateNameAlreadyInUse(fromUtf8(name));
         }
         record.set("state", "joining");
         _transport->publish(recordTopic, record.dump(), 1, true);
         _transport->barrier();

         bool contended = false;
         bool stillExists = false;
         {
            std::lock_guard<std::mutex> v(_viewMutex);
            stillExists = _view.count(ft + "/execution") != 0;
            for (FederateRecord const& r : liveFederates(ft))
               if (r.name == name && r.handle != handle)
                  contended = true;
         }
         if (!stillExists)
         {
            _transport->publish(recordTopic, std::string(), 1, true);
            abandon();
            throw FederationExecutionDoesNotExist(federationName);
         }
         if (!contended)
            break;
         _transport->publish(recordTopic, std::string(), 1, true);
         if (attempt + 1 >= kMaxAttempts)
         {
            abandon();
            throw RTIinternalError(L"joinFederationExecution: too much contention on the name " + fromUtf8(name));
         }
         backoff(attempt);
         _transport->barrier();
      }

      for (Module const& m : extra)
         _transport->publish(ft + "/fom/" + m.hash, m.content, 1, true);
      record.set("state", "joined");
      _transport->publish(recordTopic, record.dump(), 1, true);

      {
         std::lock_guard<std::mutex> v(_viewMutex);
         // Modules published by others since the FDD was built.
         for (auto const& kv : recordsUnder(ft + "/fom/"))
         {
            std::string const h = kv.first.substr(ft.size() + 5);
            if (merged.insert(h).second)
            {
               try
               {
                  fdd->merge(kv.second, "module " + h);
               }
               catch (FddError const&)
               {
               }
            }
         }
         _fdd = fdd;
         _mergedModules = merged;
         _fedTopic = ft;
      }
      _federationName = federationName;
      _timeImplementation = timeName;
      _federateHandle = handle;
      _federateName = fromUtf8(name);
      _federateType = federateType;
      _joined = true;

      // From now on: only the topics this federate needs (object and
      // interaction subtrees are added by the declaration services).
      subscribeJoinedTopics();
      _transport->unsubscribe(ft + "/#");
      {
         std::lock_guard<std::mutex> v(_viewMutex);
         forgetUnder(ft + "/objects/");
         forgetUnder(ft + "/names/");
         forgetUnder(ft + "/claims/");
      }
      std::vector<std::string> contents;
      for (Module const& m : extra)
         contents.push_back(m.content);
      momJoined(additionalFomModules, contents);
   }
   catch (FddError const& e) // modules already in the federation
   {
      abandon();
      throw RTIinternalError(L"FOM of the federation unreadable: " + fromUtf8(e.what()));
   }
   catch (std::runtime_error const& e) // transport or JSON
   {
      abandon();
      throw RTIinternalError(fromUtf8(e.what()));
   }
   HLA_MQTT_LOG(Info, _transport->settings().clientId,
                "joined " << toUtf8(federationName) << " as " << toUtf8(federateName) << " (handle " << hex64(handle)
                          << ")");
   return FederateHandleFriend::create(handle);
}

FederateHandle MqttRTIambassador::joinFederationExecution(std::wstring const& federateType,
                                                          std::wstring const& federationExecutionName,
                                                          std::vector<std::wstring> const& additionalFomModules)
{
   return joinFederation(std::wstring(), federateType, federationExecutionName, additionalFomModules);
}

FederateHandle MqttRTIambassador::joinFederationExecution(std::wstring const& federateName,
                                                          std::wstring const& federateType,
                                                          std::wstring const& federationExecutionName,
                                                          std::vector<std::wstring> const& additionalFomModules)
{
   if (federateName.empty())
      throw RTIinternalError(L"empty federate name");
   return joinFederation(federateName, federateType, federationExecutionName, additionalFomModules);
}

// ------------------------------------------------------------------ 4.10
void MqttRTIambassador::resignFederationExecution(ResignAction resignAction)
{
   checkNotInCallback();
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   if (resignAction < UNCONDITIONALLY_DIVEST_ATTRIBUTES || resignAction > NO_ACTION)
      throw InvalidResignAction(L"unknown resign action");
   std::string const ft = _fedTopic;
   {
      // Resigned during a restore: the other members end it (federationNotRestored).
      std::lock_guard<std::mutex> v(_viewMutex);
      if (_restore.active && !_restore.done && _restore.member)
      {
         std::uint64_t pre = _federateHandle;
         if (_restore.loaded)
            for (RestoreMember const& m : _restore.members)
               if (m.saved == _federateHandle)
                  pre = m.handle;
         Json st = Json::object();
         st.set("id", _restore.id);
         st.set("status", "resigned");
         try
         {
            _transport->publishAsync(ft + "/restore/status/" + hex64(pre), st.dump(), 1, true);
         }
         catch (std::runtime_error const&)
         {
         }
      }
      _restoring = false;
      _saveBusy = false;
      _restoreBusy = false;
   }
   try
   {
      resignObjects(resignAction); // FederateOwnsAttributes before any change
      momResigning();               // HLAfederate object deleted
      _transport->publish(ft + "/federates/" + hex64(_federateHandle), std::string(), 1, true);
   }
   catch (std::runtime_error const& e)
   {
      throw RTIinternalError(fromUtf8(e.what()));
   }
   try
   {
      _transport->publish(ft + "/time/" + hex64(_federateHandle), std::string(), 1, true);
      _transport->publish(ft + "/decl/" + hex64(_federateHandle), std::string(), 1, true);
   }
   catch (std::runtime_error const&)
   {
   }
   unsubscribeJoinedTopics();
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      forgetUnder(ft + "/");
      _fdd.reset();
      _mergedModules.clear();
      _fedTopic.clear();
      resetObjectState();
      _syncPoints.clear();
      resetTime();
   }
   _joined = false;
   HLA_MQTT_LOG(Info, _transport->settings().clientId, "resigned from " << toUtf8(_federationName));
   _federationName.clear();
   _federateName.clear();
   _federateType.clear();
   _federateHandle = 0;
}
} // namespace hla_mqtt
