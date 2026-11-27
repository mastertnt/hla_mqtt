// Federation synchronization points (IEEE 1516.1-2010, 4.11 to 4.15).
//
// Topics under the joined federation F (label percent-encoded):
//   F/sync/<label>/point            retained {"label","tag","members":[hex],"registrant","whole"}
//   F/sync/<label>/claims/<client>  label claim during the registration (transient)
//   F/sync/<label>/achieved/<hex>   retained {"success":bool}
//
// The label is claimed like a federation name (claim + barrier). Every member
// decides by itself that the point is synchronized: each member has achieved
// it, or has left the federation (resigned or vanished). The registrant (or,
// if it is gone, the live member with the smallest handle) then clears the
// records, which makes the label available again.
// The synchronization set is the given set, or, without one, the whole
// federation: the federates joined at registration, plus the ones that join
// before the point is synchronized (they find the retained point record and
// are announced the point, as with the usual 1516 RTIs). A member that sees
// the records cleared before it has seen the synchronization (it joined while
// the others concluded) is told the point is synchronized.

#include "MqttRTIambassador.h"

#include "Fdd.h"
#include "HandleFriends.h"
#include "Json.h"
#include "MqttTransport.h"
#include "Utf.h"
#include "Wire.h"

namespace hla_mqtt
{
namespace
{
bool startsWith(std::string const& s, std::string const& prefix)
{
   return s.compare(0, prefix.size(), prefix) == 0;
}

std::string toHexBytes(void const* data, size_t size)
{
   static char const* digits = "0123456789abcdef";
   std::string out;
   unsigned char const* p = static_cast<unsigned char const*>(data);
   for (size_t k = 0; k < size; ++k)
   {
      out += digits[p[k] >> 4];
      out += digits[p[k] & 0xF];
   }
   return out;
}

std::string fromHexBytes(std::string const& hex)
{
   std::string out;
   for (size_t k = 0; k + 1 < hex.size(); k += 2)
      out += char(std::strtoul(hex.substr(k, 2).c_str(), nullptr, 16));
   return out;
}

int const kSyncAttempts = 8;
} // namespace

std::set<std::uint64_t> MqttRTIambassador::liveFederateHandles() const
{
   std::set<std::uint64_t> out;
   for (FederateRecord const& r : liveFederates(_fedTopic))
      out.insert(r.handle);
   out.insert(_federateHandle);
   return out;
}

// ------------------------------------------------------------------ 4.11
void MqttRTIambassador::registerFederationSynchronizationPoint(std::wstring const& label,
                                                               VariableLengthData const& theUserSuppliedTag)
{
   registerFederationSynchronizationPoint(label, theUserSuppliedTag, FederateHandleSet());
}

void MqttRTIambassador::registerFederationSynchronizationPoint(std::wstring const& label,
                                                               VariableLengthData const& theUserSuppliedTag,
                                                               FederateHandleSet const& synchronizationSet)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();

   std::set<std::uint64_t> members;
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      std::set<std::uint64_t> const live = liveFederateHandles();
      if (synchronizationSet.empty())
         members = live;
      else
      {
         for (FederateHandle const& h : synchronizationSet)
         {
            std::uint64_t const value = FederateHandleFriend::value(h);
            if (!live.count(value))
            {
               enqueue([label](FederateAmbassador& fa) {
                  fa.synchronizationPointRegistrationFailed(label, SYNCHRONIZATION_SET_MEMBER_NOT_JOINED);
               });
               return;
            }
            members.insert(value);
         }
      }
   }

   std::string const key = encodeTopicLevel(toUtf8(label));
   std::string const base = _fedTopic + "/sync/" + key;
   std::string const myId = _transport->settings().clientId;
   std::string const claimTopic = base + "/claims/" + encodeTopicLevel(myId);

   auto pointExists = [&] {
      std::lock_guard<std::mutex> v(_viewMutex);
      return _view.count(base + "/point") != 0;
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
      return _view.count(base + "/point") != 0;
   };

   try
   {
      for (int attempt = 0; attempt < kSyncAttempts; ++attempt)
      {
         _transport->barrier();
         if (pointExists())
            break;
         Json claim = Json::object();
         claim.set("clientId", myId);
         _transport->publish(claimTopic, claim.dump(), 1, true);
         _transport->barrier();
         if (contended())
         {
            _transport->publish(claimTopic, std::string(), 1, true);
            backoff(attempt);
            continue;
         }
         Json point = Json::object();
         Json m = Json::array();
         for (std::uint64_t h : members)
            m.push(hex64(h));
         point.set("label", toUtf8(label));
         point.set("tag", toHexBytes(theUserSuppliedTag.data(), theUserSuppliedTag.size()));
         point.set("members", m);
         point.set("registrant", hex64(_federateHandle));
         point.set("whole", synchronizationSet.empty());
         // Succeeded is queued before the announcement (delivered by the broker).
         enqueue([label](FederateAmbassador& fa) { fa.synchronizationPointRegistrationSucceeded(label); });
         _transport->publish(base + "/point", point.dump(), 1, true);
         _transport->publish(claimTopic, std::string(), 1, true);
         return;
      }
   }
   catch (std::runtime_error const& e)
   {
      throw RTIinternalError(fromUtf8(e.what()));
   }
   enqueue([label](FederateAmbassador& fa) {
      fa.synchronizationPointRegistrationFailed(label, SYNCHRONIZATION_POINT_LABEL_NOT_UNIQUE);
   });
}

// ------------------------------------------------------------------ 4.14
void MqttRTIambassador::synchronizationPointAchieved(std::wstring const& label, bool successfully)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   checkNotSavingOrRestoring();
   std::string const key = encodeTopicLevel(toUtf8(label));
   {
      std::lock_guard<std::mutex> v(_viewMutex);
      auto it = _syncPoints.find(key);
      if (it == _syncPoints.end() || !it->second.announced || it->second.achievedSent || it->second.synchronized)
         throw SynchronizationPointLabelNotAnnounced(label);
      it->second.achievedSent = true;
   }
   Json j = Json::object();
   j.set("success", successfully);
   try
   {
      _transport->publish(_fedTopic + "/sync/" + key + "/achieved/" + hex64(_federateHandle), j.dump(), 1, true);
   }
   catch (std::runtime_error const& e)
   {
      throw RTIinternalError(fromUtf8(e.what()));
   }
}

// ------------------------------------------------------- incoming records
void MqttRTIambassador::handleSyncTraffic(std::string const& rest, std::string const& payload)
{
   // rest = "sync/<key>/point" | "sync/<key>/achieved/<hex>" | "sync/<key>/claims/..."
   size_t const slash = rest.find('/', 5);
   if (slash == std::string::npos)
      return;
   std::string const key = rest.substr(5, slash - 5);
   std::string const what = rest.substr(slash + 1);
   try
   {
      if (what == "point")
      {
         if (payload.empty())
         {
            // Cleared after synchronization: label free again.
            auto it = _syncPoints.find(key);
            if (it != _syncPoints.end() && it->second.announced && !it->second.synchronized)
            {
               std::wstring const label = it->second.label;
               enqueue([label](FederateAmbassador& fa) { fa.federationSynchronized(label, FederateHandleSet()); });
            }
            _syncPoints.erase(key);
            return;
         }
         Json j = Json::parse(payload);
         SyncPoint& sp = _syncPoints[key];
         if (!sp.known)
         {
            sp.known = true;
            sp.label = fromUtf8(j["label"].str());
            sp.tag = fromHexBytes(j["tag"].str());
            sp.registrant = std::strtoull(j["registrant"].str().c_str(), nullptr, 16);
            for (std::string const& h : j["members"].strings())
               sp.members.insert(std::strtoull(h.c_str(), nullptr, 16));
            sp.whole = j["whole"].boolean(); // false in records without it
         }
         if (_federateHandle == 0)
            return; // joining: the record comes again once joined
         if (sp.whole && !sp.synchronized)
            sp.members.insert(_federateHandle); // joined after the registration, if not already in
         if (!sp.announced && sp.members.count(_federateHandle))
         {
            sp.announced = true;
            std::wstring const label = sp.label;
            std::string const tag = sp.tag;
            enqueue([label, tag](FederateAmbassador& fa) {
               fa.announceSynchronizationPoint(label, VariableLengthData(tag.data(), tag.size()));
            });
         }
         evaluateSync(key, sp);
      }
      else if (startsWith(what, "achieved/"))
      {
         if (payload.empty())
            return;
         SyncPoint& sp = _syncPoints[key];
         std::uint64_t const h = std::strtoull(what.substr(9).c_str(), nullptr, 16);
         sp.achieved[h] = Json::parse(payload)["success"].boolean();
         evaluateSync(key, sp);
      }
   }
   catch (std::exception const&)
   {
   }
}

void MqttRTIambassador::evaluateSync(std::string const& key, SyncPoint& sp)
{
   if (!sp.known || sp.synchronized)
      return;
   std::set<std::uint64_t> const live = liveFederateHandles();
   std::set<std::uint64_t> members = sp.members;
   if (sp.whole)
      members.insert(live.begin(), live.end()); // late joiners
   FederateHandleSet failed;
   for (std::uint64_t m : members)
   {
      auto it = sp.achieved.find(m);
      if (it == sp.achieved.end())
      {
         if (live.count(m))
            return; // still waiting for this member
         continue;  // member left the federation: removed from the set
      }
      if (!it->second)
         failed.insert(FederateHandleFriend::create(m));
   }
   sp.synchronized = true;
   if (sp.members.count(_federateHandle))
   {
      std::wstring const label = sp.label;
      enqueue([label, failed](FederateAmbassador& fa) { fa.federationSynchronized(label, failed); });
   }

   // Cleanup by the registrant, or by the smallest live member if it is gone.
   std::uint64_t cleaner = sp.registrant;
   if (!live.count(cleaner))
   {
      cleaner = 0;
      for (std::uint64_t m : sp.members)
         if (live.count(m))
         {
            cleaner = m;
            break;
         }
   }
   if (cleaner == _federateHandle && _transport)
   {
      std::string const base = _fedTopic + "/sync/" + key;
      try
      {
         for (auto const& kv : sp.achieved)
            _transport->publishAsync(base + "/achieved/" + hex64(kv.first), std::string(), 1, true);
         _transport->publishAsync(base + "/point", std::string(), 1, true);
      }
      catch (std::runtime_error const&)
      {
      }
   }
}

void MqttRTIambassador::evaluateAllSync()
{
   for (auto& kv : _syncPoints)
      evaluateSync(kv.first, kv.second);
}
} // namespace hla_mqtt
