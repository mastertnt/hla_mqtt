// Concrete RTIambassador speaking MQTT, without central RTI process.
//
// Jalon 1: connect / disconnect / callback delivery (evoked + immediate).
// Jalon 2: federation management (create/destroy/list/join/resign, see
// MqttFederation.cpp for the MQTT protocol) and the support services that
// only need the FDD (MqttSupportServices.cpp).
// Every other service throws RTIinternalError("... not implemented yet"),
// see generated/RTIambassadorStubs.cpp (tools/gen_rti_sources.py).
#pragma once

#include <RTI/RTI1516.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace hla_mqtt
{
using namespace rti1516e;

class MqttTransport;
class Fdd;
}
#include "TimeValue.h"
namespace hla_mqtt
{

class MqttRTIambassador : public RTIambassador
{
public:
   MqttRTIambassador();
   ~MqttRTIambassador() override;

#include "generated/RTIambassadorOverrides.inc"

private:
   using Callback = std::function<void(FederateAmbassador&)>;

   [[noreturn]] void notImplemented(wchar_t const* service) const;

   void checkNotInCallback() const;
   void enqueue(Callback cb);
   // Pops and runs one callback; `lock` is released while it runs.
   void deliverOne(std::unique_lock<std::mutex>& lock);
   bool deliverable() const; // requires _queueMutex
   void startDispatcher();
   void stopDispatcher();
   void onConnectionLost(std::string const& cause);

   // ------------------------------------------- jalon 2 (MqttFederation.cpp)
   struct FederateRecord
   {
      std::uint64_t handle = 0;
      std::string name;
      std::string type;
      std::string clientId;
      std::string state; // "joining" (name claim in progress) or "joined"
      std::string topic;
   };

   // Paho thread: mirrors retained records, merges FOM modules published
   // by other federates of the joined federation.
   void onMessage(std::string const& topic, std::string const& payload);
   void checkConnected() const;
   void checkJoined() const;
   std::string federationTopic(std::wstring const& federationName) const;
   void createFederation(std::wstring const& federationName, std::vector<std::wstring> const& fomModules,
                         std::wstring const* mimModule, std::wstring const& logicalTimeImplementationName);
   FederateHandle joinFederation(std::wstring const& federateName, std::wstring const& federateType,
                                 std::wstring const& federationName,
                                 std::vector<std::wstring> const& additionalFomModules);
   void backoff(int attempt);
   // The following require _viewMutex.
   std::map<std::string, std::string> recordsUnder(std::string const& topicPrefix) const;
   bool clientAlive(std::string const& clientId) const;
   std::vector<FederateRecord> liveFederates(std::string const& fedTopic) const;
   void forgetUnder(std::string const& topicPrefix);

   // Retained records seen on our subscriptions (topic -> payload).
   mutable std::mutex _viewMutex;
   std::map<std::string, std::string> _view;
   // Joined federation (written with _stateMutex and _viewMutex held).
   std::string _fedTopic;
   std::wstring _federationName;
   std::wstring _timeImplementation;
   std::uint64_t _federateHandle = 0;
   std::wstring _federateName;
   std::wstring _federateType;
   std::shared_ptr<Fdd> _fdd;
   std::set<std::string> _mergedModules;
   std::atomic<ResignAction> _automaticResignDirective{CANCEL_THEN_DELETE_THEN_DIVEST};

   // ------------------------------------------- jalon 3 (MqttObjects.cpp)
   struct ObjectInstance
   {
      std::uint64_t handle = 0;
      std::uint64_t objectClass = 0; // registered class
      std::uint64_t owner = 0;       // federate handle, 0 = no owner (divested)
      std::string ownerClient;
      std::string name;
      std::string topic;               // .../objects/<class path>/@<handle>
      std::uint64_t knownClass = 0;    // discovered as (0 = not discovered)
      bool locallyDeleted = false;     // rediscovered on the next update only
      std::set<std::uint64_t> owned;   // attributes owned by us (own objects)
   };

   void subscribeJoinedTopics();
   void unsubscribeJoinedTopics() noexcept;
   void resignObjects(ResignAction action);
   void resetObjectState(); // requires _viewMutex
   // Paho thread, _viewMutex held:
   void handleObjectTraffic(std::string const& rest, std::string const& payload);
   void handlePresence(std::string const& clientId, std::string const& payload);
   void tryDiscover(ObjectInstance& o);
   void discoverPending();
   // Federate thread, no lock held: broker subscriptions follow the declarations.
   void updateBrokerSubscriptions();
   std::uint64_t knownObjectClass(std::uint64_t objectClass) const;      // _viewMutex
   std::uint64_t receivedInteractionClass(std::uint64_t interaction) const; // _viewMutex
   bool isSubclassOf(std::uint64_t objectClass, std::uint64_t ancestor) const;
   bool seenBefore(std::uint64_t sender, std::uint64_t seq);
   std::uint64_t nextSeq() { return ++_seq; }
   std::uint64_t newUniqueValue(char const* kind);
   std::string encodeTime(LogicalTime const& theTime) const;
   std::shared_ptr<LogicalTime> decodeTime(std::string const& encoded) const;
   void deleteOwnObject(ObjectInstance const& o, VariableLengthData const& tag, std::string const& time,
                        bool tso = false);
   void reserveNames(std::set<std::wstring> const& names, bool multiple);

   std::map<std::uint64_t, ObjectInstance> _objects; // own + seen from info records
   std::map<std::uint64_t, std::set<std::uint64_t>> _publishedAttributes;
   std::map<std::uint64_t, std::set<std::uint64_t>> _subscribedAttributes;
   std::set<std::uint64_t> _publishedInteractions;
   std::set<std::uint64_t> _subscribedInteractions;
   std::set<std::string> _coverTopics;   // object/interaction subscriptions on the broker
   std::set<std::wstring> _reservedNames;
   std::atomic<std::uint64_t> _seq{0};
   std::atomic<std::uint64_t> _uniqueCounter{0};
   std::set<std::pair<std::uint64_t, std::uint64_t>> _recent; // (sender, seq) duplicates guard
   std::deque<std::pair<std::uint64_t, std::uint64_t>> _recentOrder;
   std::atomic<bool> _advisorySwitches[4] = {{false}, {false}, {false}, {false}};

   // ------------------------------------- jalon 4: synchronization (MqttSync.cpp)
   struct SyncPoint
   {
      std::wstring label;
      std::string tag;
      std::set<std::uint64_t> members;
      std::uint64_t registrant = 0;
      bool known = false;        // point record received
      bool announced = false;    // announced to this federate
      bool achievedSent = false;
      bool synchronized = false;
      std::map<std::uint64_t, bool> achieved;
   };
   std::map<std::string, SyncPoint> _syncPoints; // key: encoded label
   void handleSyncTraffic(std::string const& rest, std::string const& payload); // _viewMutex held
   void evaluateSync(std::string const& key, SyncPoint& sp);                    // _viewMutex held
   void evaluateAllSync();                                                       // _viewMutex held
   std::set<std::uint64_t> liveFederateHandles() const;                         // _viewMutex held

   // -------------------------------------- jalon 4: time management (MqttTime.cpp)
   enum class Advance
   {
      None,
      TAR,
      TARA,
      NER,
      NERA,
      FQR
   };
   struct TimeState // published by every regulating or constrained federate
   {
      std::uint64_t version = 0;
      std::string clientId;
      bool regulating = false, regPending = false, constrained = false, conPending = false;
      TimeValue time, lookahead, requested, queueMin, promise;
      bool promiseOpen = false;
      Advance advance = Advance::None;
      std::map<std::uint64_t, std::uint64_t> seen; // federate -> version seen
   };
   struct OwnTime
   {
      bool regulating = false, regPending = false, constrained = false, conPending = false;
      bool asyncDelivery = false;
      TimeValue time, lookahead, requested;
      Advance advance = Advance::None;
      bool lastGrantOpen = true; // zero lookahead after TAR/NER grant: sends > time
      std::uint64_t version = 0;
      std::uint64_t regPendingVersion = 0;
      // Enablement completes only after a broker round trip following the
      // publication of the pending state: each side then holds the state of
      // the other, whatever the order of concurrent requests.
      bool regBarrierDone = false;
      bool conBarrierDone = false;
      Bound promiseFloor;          // published promises never go down
      bool promiseFloorSet = false;
      std::string lastPublished;   // last payload, to republish only on change
      std::string lastMeaning;     // meaningful fields of the last version
   };
   bool timeIsInteger() const { return _timeImplementation == L"HLAinteger64Time"; }
   void resetTime();                                  // _viewMutex held
   void handleTimeState(std::string const& hexHandle, std::string const& payload); // _viewMutex held
   void evaluateTime();                               // _viewMutex held: grants, enablements, publication
   bool galt(Bound& out) const;                       // _viewMutex held; false = no regulating federate
   Bound ownPromise(Bound const* galtValue) const;    // _viewMutex held
   TimeValue queueMin() const;                        // _viewMutex held
   void publishTimeState(bool meaningful);            // _viewMutex held, publishAsync
   void deliverRO(Callback cb);                       // _viewMutex held
   using TimedCallback = std::function<void(FederateAmbassador&, OrderType receivedOrder)>;
   void deliverTimed(TimeValue const& ts, bool tso, TimedCallback cb); // _viewMutex held
   // Send side (MqttObjects.cpp), _viewMutex held: true if the message goes TSO;
   // throws InvalidLogicalTime when the timestamp breaks the promise.
   bool sendsTso(bool timestampOrder, TimeValue const& ts);
   bool snapshot(TimeValue& minNext) const;          // _viewMutex held: consistent cut, all regulators waiting
   Bound sendBound() const;                           // _viewMutex held: smallest timestamp we may send TSO
   void startAdvance(Advance kind, LogicalTime const& theTime);

   std::map<std::uint64_t, TimeState> _timeStates; // other federates
   OwnTime _time;
   std::multimap<TimeValue, TimedCallback> _tsoQueue;
   std::deque<Callback> _roHeld;
   std::mt19937_64 _random{std::random_device{}()};

   // Connection state (guarded by _stateMutex)
   mutable std::mutex _stateMutex;
   std::unique_ptr<MqttTransport> _transport;
   // atomics: read by callback delivery without taking _stateMutex
   std::atomic<FederateAmbassador*> _federateAmbassador{nullptr};
   std::atomic<CallbackModel> _callbackModel{HLA_EVOKED};
   std::atomic<bool> _connected{false};
   std::atomic<bool> _joined{false};

   // Callback queue (guarded by _queueMutex)
   std::mutex _queueMutex;
   std::condition_variable _queueCv;
   std::deque<Callback> _queue;
   bool _callbacksEnabled = true;
   bool _stopDispatcher = false;
   std::thread _dispatcher;
};
}
