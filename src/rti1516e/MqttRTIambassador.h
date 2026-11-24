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
      std::set<std::uint64_t> ownedRemote; // attributes owned by the owner (from its info record)
      std::set<std::uint64_t> relevant;    // attributes notified by turnUpdatesOn (own objects)
      std::map<std::uint64_t, std::set<std::uint64_t>> updateRegions; // DDM: attribute -> our regions (9.5, 9.6)
      std::map<std::uint64_t, OrderType> order;       // 8.23 changeAttributeOrderType (own objects)
      std::map<std::uint64_t, std::string> transport; // 6.23 transportation changed by the owner (from info for others)
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
   // Effective subscriptions: union of the default-region subscriptions (5.6,
   // 5.8) and of the subscriptions with regions (9.8, 9.10), see
   // recomputeSubscriptions().
   std::map<std::uint64_t, std::set<std::uint64_t>> _subscribedAttributes;
   std::set<std::uint64_t> _publishedInteractions;
   std::set<std::uint64_t> _subscribedInteractions;
   std::map<std::uint64_t, std::map<std::uint64_t, double>> _subscribedRates; // class -> attribute -> update rate (Hz)
   std::map<std::uint64_t, OrderType> _interactionOrder;       // 8.24 changeInteractionOrderType
   std::map<std::uint64_t, std::string> _interactionTransport; // 6.27 requestInteractionTransportationTypeChange
   // Effective order / transportation: change requested by this federate, else the FDD.
   bool timestampOrder(ObjectInstance const& o, std::uint64_t attribute) const;  // _viewMutex
   bool bestEffort(ObjectInstance const& o, std::uint64_t attribute) const;      // _viewMutex
   bool interactionTimestampOrder(std::uint64_t interactionClass) const;          // _viewMutex
   bool interactionBestEffort(std::uint64_t sender, std::uint64_t interactionClass) const; // _viewMutex
   void setSubscribedRates(std::uint64_t cls, AttributeHandleSet const& attrs, double rate); // _viewMutex
   std::set<std::string> _coverTopics;   // object/interaction subscriptions on the broker
   std::set<std::wstring> _reservedNames;
   std::atomic<std::uint64_t> _seq{0};
   std::atomic<std::uint64_t> _uniqueCounter{0};
   std::set<std::pair<std::uint64_t, std::uint64_t>> _recent; // (sender, seq) duplicates guard
   std::deque<std::pair<std::uint64_t, std::uint64_t>> _recentOrder;
   // 0 object class relevance, 1 attribute relevance, 2 attribute scope, 3 interaction relevance
   std::atomic<bool> _advisorySwitches[4] = {{false}, {false}, {false}, {false}};

   // ------------------------------------------- advisories (MqttAdvisories.cpp)
   struct RemoteDeclarations
   {
      std::map<std::uint64_t, std::set<std::uint64_t>> objects; // class -> subscribed attributes
      std::set<std::uint64_t> interactions;
      std::map<std::uint64_t, std::string> interactionTransport; // 6.27, class -> transportation
   };
   std::map<std::uint64_t, RemoteDeclarations> _remoteDecl; // other federates' subscriptions
   std::set<std::uint64_t> _classRelevanceOn;
   std::set<std::uint64_t> _interactionRelevanceOn;
   std::string _lastDeclarations;
   std::string declarationsPayload() const;                                         // _viewMutex held
   void publishDeclarations();                                                       // _viewMutex held (async)
   void handleDeclarations(std::string const& hexHandle, std::string const& payload); // _viewMutex held
   void evaluateAdvisories();                                                        // _viewMutex held
   void notifyScope(ObjectInstance const& o);                                        // _viewMutex held
   void setAdvisorySwitch(int index, bool value);

   // ------------------------------------------- DDM regions (MqttRegions.cpp)
   using Extents = std::map<std::uint64_t, std::pair<unsigned long, unsigned long>>; // dimension -> [lower, upper)
   struct Region
   {
      Extents pending;   // setRangeBounds
      Extents committed; // commitRegionModifications
   };
   std::map<std::uint64_t, Region> _regions; // created by this federate
   std::atomic<std::uint32_t> _regionCounter{0};
   std::map<std::uint64_t, std::set<std::uint64_t>> _plainSubscribedAttributes; // class -> attributes
   std::map<std::uint64_t, std::map<std::uint64_t, std::set<std::uint64_t>>> _regionSubscribedAttributes; // class -> attribute -> regions
   std::set<std::uint64_t> _plainSubscribedInteractions;
   std::map<std::uint64_t, std::set<std::uint64_t>> _regionSubscribedInteractions; // class -> regions
   // The following require _viewMutex.
   void recomputeSubscriptions();
   std::uint64_t ownRegion(RegionHandle const& h) const; // InvalidRegion, RegionNotCreatedByThisFederate
   // Checks the attributes (defined for `cls`), the regions and, with
   // `context`, that each region only spans dimensions of its attributes.
   void checkRegionPairs(std::uint64_t cls, AttributeHandleSetRegionHandleSetPairVector const& pairs,
                         bool context = true) const;
   void checkRegionContext(std::uint64_t region, std::vector<std::string> const& dimensionNames) const;
   bool regionInUse(std::uint64_t region) const;
   std::string objectInfo(ObjectInstance const& o) const; // retained info record, with the update regions
   // Info records of our objects whose update regions changed: (topic, payload).
   std::vector<std::pair<std::string, std::string>> infoRecordsUsing(std::set<std::uint64_t> const& regions) const;
   void publishInfoRecords(std::vector<std::pair<std::string, std::string>> const& records);
   ObjectInstanceHandle registerObject(ObjectClassHandle theClass, std::wstring const& name,
                                       AttributeHandleSetRegionHandleSetPairVector const* regions);

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
   // `retraction`: handle of a TSO message (0: not retractable).
   void deliverTimed(TimeValue const& ts, bool tso, TimedCallback cb, std::uint64_t retraction = 0); // _viewMutex held
   // Send side (MqttObjects.cpp), _viewMutex held: true if the message goes TSO;
   // throws InvalidLogicalTime when the timestamp breaks the promise.
   bool sendsTso(bool timestampOrder, TimeValue const& ts);
   bool snapshot(TimeValue& minNext) const;          // _viewMutex held: consistent cut, all regulators waiting
   Bound sendBound() const;                           // _viewMutex held: smallest timestamp we may send TSO
   void startAdvance(Advance kind, LogicalTime const& theTime);

   std::map<std::uint64_t, TimeState> _timeStates; // other federates
   OwnTime _time;
   struct QueuedTso
   {
      std::uint64_t retraction = 0;
      TimedCallback cb;
   };
   std::multimap<TimeValue, QueuedTso> _tsoQueue;
   void tsoDelivered(std::uint64_t retraction); // _viewMutex held: remembered for requestRetraction

   // ----------------------------------------------- 8.21 retract (MqttTime.cpp)
   struct SentTso
   {
      TimeValue time;
      std::string topic;
   };
   std::map<std::uint64_t, SentTso> _sentTso;    // retraction handle -> TSO message we sent
   std::set<std::uint64_t> _deliveredTso;        // retraction handles of TSO messages delivered to us
   std::deque<std::uint64_t> _deliveredTsoOrder; // bounded memory
   void rememberSentTso(std::uint64_t retraction, TimeValue const& ts, std::string const& topic); // _viewMutex
   void handleRetraction(std::uint64_t retraction); // _viewMutex held
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
