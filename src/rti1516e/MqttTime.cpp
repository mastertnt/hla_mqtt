// Time management (IEEE 1516.1-2010, chapter 8) without a central RTI.
//
// Every regulating or constrained federate publishes its time state, retained:
//   F/time/<federate handle>  {"v","client","reg","regp","con","conp","t","la",
//                               "adv","req","q","p","po","seen":{handle:version}}
// "p"/"po" is the PROMISE of a regulating federate: a lower bound on the
// timestamp of every TSO message it may still send ("po" = strictly greater).
// "v" (version) changes only when a meaningful field changes; "seen" gives the
// versions of the other states this one was computed from.
//
// GALT of a federate = min of the promises of the other regulating federates.
// It is safe because the broker keeps the order of the messages from one
// sender to one receiver: a TSO message sent before a promise is received
// before it (TSO messages and states are both published with QoS 1).
//
// Promise of a regulating federate (L = lookahead):
//   granted at T              T + L        (open if L = 0 and granted by TAR/NER)
//   TAR(t) / TARA(t)          t + L        (open if L = 0 for TAR)
//   NER(t) / NERA(t) / FQR    min(t, own queue, GALT) + L
// While advancing, TSO sends must also respect next + L (next = t for TAR,
// min(t, queue) for NER), which keeps the cut argument below valid.
//
// Zero lookahead: promises alone stall (null-message deadlock). When the cut
// formed by the states is consistent (every regulating federate waits in an
// advance, and its "seen" versions are the latest ones we hold, so every TSO
// message sent before is counted in the queues), nobody can be granted before
// M = min of the "next" of the regulating federates, so nobody will send below
// M + L: the promises are raised to that bound.
//
// Grant of a constrained federate (next as above, but with its own queue):
//   TAR/NER   when GALT is beyond next (every message <= next received)
//   TARA/NERA when GALT reaches next   (every message <  next received)
// The TSO messages with timestamp <= grant time are delivered first, in
// timestamp order, then timeAdvanceGrant.
//
// Enabling regulation waits until every constrained federate has seen our
// pending state; the logical time is then raised to the largest of their
// logical times. Enabling constraint waits until GALT reaches our time.

#include "MqttRTIambassador.h"

#include "HandleFriends.h"

#include "Json.h"
#include "MqttTransport.h"
#include "Utf.h"
#include "Wire.h"

#include <algorithm>

namespace hla_mqtt
{
namespace
{
char const* advanceName(int a)
{
   static char const* names[] = {"", "tar", "tara", "ner", "nera", "fqr"};
   return names[a];
}

int advanceIndex(std::string const& s)
{
   static char const* names[] = {"", "tar", "tara", "ner", "nera", "fqr"};
   for (int k = 0; k < 6; ++k)
      if (s == names[k])
         return k;
   return 0;
}
} // namespace

// ================================================================ state
void MqttRTIambassador::resetTime()
{
   bool const isInt = timeIsInteger();
   _timeStates.clear();
   _time = OwnTime();
   _time.time = TimeValue::zero(isInt);
   _time.lookahead = TimeValue::zero(isInt);
   _time.requested = TimeValue::zero(isInt);
   _tsoQueue.clear();
   _roHeld.clear();
   _sentTso.clear();
   _deliveredTso.clear();
   _deliveredTsoOrder.clear();
}

TimeValue MqttRTIambassador::queueMin() const
{
   return _tsoQueue.empty() ? TimeValue::inf(timeIsInteger()) : _tsoQueue.begin()->first;
}

void MqttRTIambassador::handleTimeState(std::string const& hexHandle, std::string const& payload)
{
   std::uint64_t const h = std::strtoull(hexHandle.c_str(), nullptr, 16);
   if (h == _federateHandle)
      return;
   if (payload.empty())
      _timeStates.erase(h);
   else
   {
      try
      {
         bool const isInt = timeIsInteger();
         Json j = Json::parse(payload);
         TimeState s;
         s.version = std::strtoull(j["v"].str().c_str(), nullptr, 10);
         s.clientId = j["client"].str();
         s.regulating = j["reg"].boolean();
         s.regPending = j["regp"].boolean();
         s.constrained = j["con"].boolean();
         s.conPending = j["conp"].boolean();
         s.time = TimeValue::parse(j["t"].str(), isInt);
         s.lookahead = TimeValue::parse(j["la"].str(), isInt);
         s.advance = Advance(advanceIndex(j["adv"].str()));
         s.requested = TimeValue::parse(j["req"].str(), isInt);
         s.queueMin = TimeValue::parse(j["q"].str(), isInt);
         s.promise = TimeValue::parse(j["p"].str(), isInt);
         s.promiseOpen = j["po"].boolean();
         for (auto const& kv : j["seen"].members())
            s.seen[std::strtoull(kv.first.c_str(), nullptr, 16)] = std::strtoull(kv.second.str().c_str(), nullptr, 10);
         _timeStates[h] = s;
      }
      catch (std::exception const&)
      {
         return;
      }
   }
   evaluateTime();
}

// ============================================================ the engine
bool MqttRTIambassador::snapshot(TimeValue& minNext) const
{
   bool const meRegulating = _time.regulating || _time.regPending;
   std::vector<std::pair<std::uint64_t, TimeState const*>> reg;
   for (auto const& kv : _timeStates)
      if ((kv.second.regulating || kv.second.regPending) && clientAlive(kv.second.clientId))
         reg.push_back(std::make_pair(kv.first, &kv.second));
   if (reg.empty())
      return false;

   auto isWaiting = [](Advance a) { return a == Advance::TAR || a == Advance::TARA || a == Advance::NER || a == Advance::NERA; };
   minNext = TimeValue::inf(timeIsInteger());
   for (auto const& r : reg)
   {
      TimeState const& s = *r.second;
      if (s.regPending || !isWaiting(s.advance))
         return false;
      for (auto const& x : reg)
      {
         if (x.first == r.first)
            continue;
         auto it = s.seen.find(x.first);
         if (it == s.seen.end() || it->second != x.second->version)
            return false;
      }
      if (meRegulating)
      {
         auto it = s.seen.find(_federateHandle);
         if (it == s.seen.end() || it->second != _time.version)
            return false;
      }
      TimeValue next = s.requested;
      if ((s.advance == Advance::NER || s.advance == Advance::NERA) && s.queueMin < next)
         next = s.queueMin;
      minNext = std::min(minNext, next);
   }
   if (meRegulating)
   {
      if (_time.regPending || !isWaiting(_time.advance))
         return false;
      TimeValue next = _time.requested;
      if ((_time.advance == Advance::NER || _time.advance == Advance::NERA) && queueMin() < next)
         next = queueMin();
      minNext = std::min(minNext, next);
   }
   return true;
}

bool MqttRTIambassador::galt(Bound& out) const
{
   TimeValue m;
   bool const cut = snapshot(m);
   bool any = false;
   for (auto const& kv : _timeStates)
   {
      TimeState const& s = kv.second;
      if (!(s.regulating || s.regPending) || !clientAlive(s.clientId))
         continue;
      Bound b{s.promise, s.promiseOpen};
      if (cut)
      {
         Bound c{m.plus(s.lookahead), s.lookahead.isZero() && (s.advance == Advance::TAR || s.advance == Advance::NER)};
         if (b < c)
            b = c;
      }
      if (!any || b < out)
         out = b;
      any = true;
   }
   return any;
}

Bound MqttRTIambassador::ownPromise(Bound const* g) const
{
   bool const zero = _time.lookahead.isZero();
   Bound p;
   switch (_time.advance)
   {
   case Advance::None:
      p = Bound{_time.time.plus(_time.lookahead), zero && _time.lastGrantOpen};
      break;
   case Advance::TAR:
      p = Bound{_time.requested.plus(_time.lookahead), zero};
      break;
   case Advance::TARA:
      p = Bound{_time.requested.plus(_time.lookahead), false};
      break;
   default: // NER, NERA, FQR
   {
      TimeValue m = std::min(_time.requested, queueMin());
      if (g && g->value < m)
         m = g->value;
      p = Bound{m.plus(_time.lookahead), zero && _time.advance != Advance::NERA};
      break;
   }
   }
   if (_time.advance != Advance::None)
   {
      TimeValue cutMin;
      if (snapshot(cutMin))
      {
         Bound c{cutMin.plus(_time.lookahead), zero && (_time.advance == Advance::TAR || _time.advance == Advance::NER)};
         if (p < c)
            p = c;
      }
   }
   if (_time.promiseFloorSet && p < _time.promiseFloor)
      p = _time.promiseFloor;
   return p;
}

Bound MqttRTIambassador::sendBound() const
{
   bool const zero = _time.lookahead.isZero();
   Bound b;
   switch (_time.advance)
   {
   case Advance::None:
      b = Bound{_time.time.plus(_time.lookahead), zero && _time.lastGrantOpen};
      break;
   case Advance::TAR:
   case Advance::TARA:
      b = Bound{_time.requested.plus(_time.lookahead), zero && _time.advance == Advance::TAR};
      break;
   default:
      b = Bound{std::min(_time.requested, queueMin()).plus(_time.lookahead), zero && _time.advance != Advance::NERA};
      break;
   }
   if (_time.promiseFloorSet && b < _time.promiseFloor)
      b = _time.promiseFloor;
   return b;
}

void MqttRTIambassador::evaluateTime()
{
   if (!_fdd || !_joined)
      return;
   for (int round = 0; round < 8; ++round)
   {
      bool changed = false;
      Bound g;
      bool const hasG = galt(g);

      // Regulation pending: every constrained federate has seen our pending state.
      if (_time.regPending && _time.regBarrierDone)
      {
         bool ready = true;
         TimeValue newTime = _time.time;
         for (auto const& kv : _timeStates)
         {
            TimeState const& s = kv.second;
            if (!(s.constrained || s.conPending) || !clientAlive(s.clientId))
               continue;
            auto it = s.seen.find(_federateHandle);
            if (it == s.seen.end() || it->second < _time.regPendingVersion)
               ready = false;
            else if (s.constrained && s.time > newTime)
               newTime = s.time;
         }
         if (ready)
         {
            _time.regPending = false;
            _time.regulating = true;
            _time.time = newTime;
            _time.lastGrantOpen = true;
            std::shared_ptr<LogicalTime> t = newTime.toTime();
            enqueue([t](FederateAmbassador& fa) { fa.timeRegulationEnabled(*t); });
            changed = true;
         }
      }

      // Constraint pending: no TSO message below our time can still come.
      if (_time.conPending && _time.conBarrierDone && (!hasG || g.reaches(_time.time)))
      {
         _time.conPending = false;
         _time.constrained = true;
         std::shared_ptr<LogicalTime> t = _time.time.toTime();
         enqueue([t](FederateAmbassador& fa) { fa.timeConstrainedEnabled(*t); });
         changed = true;
      }

      // Time advance.
      if (_time.advance != Advance::None && !_time.regPending && !_time.conPending)
      {
         Advance const a = _time.advance;
         bool const available = a == Advance::TARA || a == Advance::NERA;
         TimeValue next = _time.requested;
         if (_time.constrained && (a == Advance::NER || a == Advance::NERA || a == Advance::FQR) && queueMin() < next)
            next = queueMin();
         bool grant = false;
         if (a == Advance::FQR)
         {
            grant = true;
            if (_time.constrained && hasG && g.value < next)
               next = std::max(g.value, _time.time);
         }
         else
            grant = !_time.constrained || !hasG || (available ? g.reaches(next) : g.beyond(next));
         if (grant)
         {
            if (next < _time.time)
               next = _time.time;
            // TSO messages up to the grant time (all of them for a flush).
            while (!_tsoQueue.empty() && (a == Advance::FQR || _tsoQueue.begin()->first <= next))
            {
               TimedCallback cb = _tsoQueue.begin()->second.cb;
               tsoDelivered(_tsoQueue.begin()->second.retraction);
               _tsoQueue.erase(_tsoQueue.begin());
               enqueue([cb](FederateAmbassador& fa) { cb(fa, TIMESTAMP); });
            }
            _time.time = next;
            _time.advance = Advance::None;
            _time.lastGrantOpen = !available;
            std::shared_ptr<LogicalTime> t = next.toTime();
            enqueue([t](FederateAmbassador& fa) { fa.timeAdvanceGrant(*t); });
            changed = true;
         }
      }

      publishTimeState(changed);
      if (!changed)
         break;
   }
}

void MqttRTIambassador::publishTimeState(bool /*meaningful*/)
{
   if (!_transport || _fedTopic.empty())
      return;
   std::string const topic = _fedTopic + "/time/" + hex64(_federateHandle);
   bool const active = _time.regulating || _time.regPending || _time.constrained || _time.conPending;
   if (!active)
   {
      if (!_time.lastPublished.empty())
      {
         _time.lastPublished.clear();
         try
         {
            _transport->publishAsync(topic, std::string(), 1, true);
         }
         catch (std::runtime_error const&)
         {
         }
      }
      return;
   }

   Bound g;
   bool const hasG = galt(g);
   Bound promise = ownPromise(hasG ? &g : nullptr);
   if (_time.regulating || _time.regPending)
   {
      _time.promiseFloor = promise;
      _time.promiseFloorSet = true;
   }

   Json meaning = Json::object();
   meaning.set("reg", _time.regulating);
   meaning.set("regp", _time.regPending);
   meaning.set("con", _time.constrained);
   meaning.set("conp", _time.conPending);
   meaning.set("t", _time.time.str());
   meaning.set("la", _time.lookahead.str());
   meaning.set("adv", advanceName(int(_time.advance)));
   meaning.set("req", _time.requested.str());
   meaning.set("q", queueMin().str());
   meaning.set("p", promise.value.str());
   meaning.set("po", promise.open);
   std::string const meaningText = meaning.dump();
   if (meaningText != _time.lastMeaning)
   {
      _time.lastMeaning = meaningText;
      ++_time.version;
   }

   Json j = meaning;
   j.set("v", std::to_string(_time.version));
   j.set("client", _transport->settings().clientId);
   Json seen = Json::object();
   for (auto const& kv : _timeStates)
      if (clientAlive(kv.second.clientId))
         seen.set(hex64(kv.first), std::to_string(kv.second.version));
   j.set("seen", seen);
   std::string const payload = j.dump();
   if (payload == _time.lastPublished)
      return;
   _time.lastPublished = payload;
   try
   {
      _transport->publishAsync(topic, payload, 1, true);
   }
   catch (std::runtime_error const&)
   {
   }
}

// ===================================================== message delivery
void MqttRTIambassador::deliverRO(Callback cb)
{
   if (_time.constrained && !_time.asyncDelivery && _time.advance == Advance::None)
      _roHeld.push_back(std::move(cb));
   else
      enqueue(std::move(cb));
}

void MqttRTIambassador::deliverTimed(TimeValue const& ts, bool tso, TimedCallback cb, std::uint64_t retraction)
{
   if (tso && _time.constrained)
   {
      _tsoQueue.emplace(ts, QueuedTso{retraction, std::move(cb)});
      evaluateTime();
   }
   else
      deliverRO([cb](FederateAmbassador& fa) { cb(fa, RECEIVE); });
}

bool MqttRTIambassador::sendsTso(bool timestampOrder, TimeValue const& ts)
{
   if (!_time.regulating || !timestampOrder)
      return false;
   Bound const b = sendBound();
   if (b.open ? !(ts > b.value) : ts < b.value)
      throw InvalidLogicalTime(L"timestamp below the logical time plus lookahead (or the requested time while advancing)");
   return true;
}

// ======================================================== 8.21 retract
// The sender keeps the TSO messages it may still retract (timestamp beyond its
// logical time) and publishes a Retract message on the topic of the original:
// same sender, same topic, so the broker delivers it after the original. A
// receiver drops the message from its TSO queue, or calls requestRetraction
// (8.22) if the message was already delivered TSO. Only TSO messages are
// retractable; the RO part of an update and deletions are not.
void MqttRTIambassador::rememberSentTso(std::uint64_t retraction, TimeValue const& ts, std::string const& topic)
{
   if (_sentTso.size() > 4096)
      for (auto it = _sentTso.begin(); it != _sentTso.end();)
         it = it->second.time <= _time.time ? _sentTso.erase(it) : std::next(it);
   _sentTso[retraction] = SentTso{ts, topic};
}

void MqttRTIambassador::tsoDelivered(std::uint64_t retraction)
{
   if (retraction == 0 || !_deliveredTso.insert(retraction).second)
      return;
   _deliveredTsoOrder.push_back(retraction);
   if (_deliveredTsoOrder.size() > 4096)
   {
      _deliveredTso.erase(_deliveredTsoOrder.front());
      _deliveredTsoOrder.pop_front();
   }
}

void MqttRTIambassador::handleRetraction(std::uint64_t retraction)
{
   for (auto it = _tsoQueue.begin(); it != _tsoQueue.end(); ++it)
      if (it->second.retraction == retraction)
      {
         _tsoQueue.erase(it);
         evaluateTime(); // the queue minimum may have changed
         return;
      }
   if (_deliveredTso.count(retraction))
   {
      MessageRetractionHandle const h = MessageRetractionHandleFriend::create(retraction);
      enqueue([h](FederateAmbassador& fa) { fa.requestRetraction(h); });
   }
}

void MqttRTIambassador::retract(MessageRetractionHandle theHandle)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   if (!_time.regulating)
      throw TimeRegulationIsNotEnabled(L"time regulation is not enabled");
   std::uint64_t const h = MessageRetractionHandleFriend::value(theHandle);
   auto it = _sentTso.find(h);
   if (h == 0 || it == _sentTso.end())
      throw InvalidMessageRetractionHandle(theHandle.toString());
   if (it->second.time <= _time.time)
   {
      _sentTso.erase(it);
      throw MessageCanNoLongerBeRetracted(theHandle.toString() + L": its timestamp is not beyond the logical time");
   }
   WireWriter w(WireKind::Retract);
   w.u64(_federateHandle).u64(nextSeq()).u64(h);
   std::string const topic = it->second.topic;
   _sentTso.erase(it);
   try
   {
      _transport->publishAsync(topic, w.str(), 1, false);
   }
   catch (std::runtime_error const& e)
   {
      throw RTIinternalError(fromUtf8(e.what()));
   }
}

// ======================================================== 8 services
void MqttRTIambassador::enableTimeRegulation(LogicalTimeInterval const& theLookahead)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   std::unique_lock<std::mutex> v(_viewMutex);
   if (_time.regulating)
      throw TimeRegulationAlreadyEnabled(L"time regulation already enabled");
   if (_time.regPending)
      throw RequestForTimeRegulationPending(L"time regulation request pending");
   if (_time.advance != Advance::None)
      throw InTimeAdvancingState(L"in time advancing state");
   _time.lookahead = TimeValue::ofInterval(theLookahead, timeIsInteger());
   _time.regPending = true;
   _time.regBarrierDone = false;
   _time.lastGrantOpen = true;
   _time.promiseFloorSet = false;
   publishTimeState(true);
   _time.regPendingVersion = _time.version;
   v.unlock();
   try
   {
      _transport->barrier(); // pending state at the broker, earlier states received
   }
   catch (std::runtime_error const& e)
   {
      throw RTIinternalError(fromUtf8(e.what()));
   }
   v.lock();
   _time.regBarrierDone = true;
   evaluateTime();
}

void MqttRTIambassador::disableTimeRegulation()
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   if (!_time.regulating && !_time.regPending)
      throw TimeRegulationIsNotEnabled(L"time regulation is not enabled");
   _time.regulating = false;
   _time.regPending = false;
   _time.promiseFloorSet = false;
   evaluateTime();
   publishTimeState(true);
}

void MqttRTIambassador::enableTimeConstrained()
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   std::unique_lock<std::mutex> v(_viewMutex);
   if (_time.constrained)
      throw TimeConstrainedAlreadyEnabled(L"time constrained already enabled");
   if (_time.conPending)
      throw RequestForTimeConstrainedPending(L"time constrained request pending");
   if (_time.advance != Advance::None)
      throw InTimeAdvancingState(L"in time advancing state");
   _time.conPending = true;
   _time.conBarrierDone = false;
   publishTimeState(true);
   v.unlock();
   try
   {
      _transport->barrier();
   }
   catch (std::runtime_error const& e)
   {
      throw RTIinternalError(fromUtf8(e.what()));
   }
   v.lock();
   _time.conBarrierDone = true;
   evaluateTime();
}

void MqttRTIambassador::disableTimeConstrained()
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   if (!_time.constrained && !_time.conPending)
      throw TimeConstrainedIsNotEnabled(L"time constrained is not enabled");
   _time.constrained = false;
   _time.conPending = false;
   // Queued TSO messages and held RO messages are delivered now (RO).
   for (auto& kv : _tsoQueue)
   {
      TimedCallback cb = kv.second.cb;
      enqueue([cb](FederateAmbassador& fa) { cb(fa, RECEIVE); });
   }
   _tsoQueue.clear();
   for (Callback& cb : _roHeld)
      enqueue(std::move(cb));
   _roHeld.clear();
   evaluateTime();
   publishTimeState(true);
}

void MqttRTIambassador::startAdvance(Advance kind, LogicalTime const& theTime)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   if (_time.advance != Advance::None)
      throw InTimeAdvancingState(L"already in time advancing state");
   if (_time.regPending)
      throw RequestForTimeRegulationPending(L"time regulation request pending");
   if (_time.conPending)
      throw RequestForTimeConstrainedPending(L"time constrained request pending");
   TimeValue const t = TimeValue::of(theTime, timeIsInteger());
   if (t < _time.time)
      throw LogicalTimeAlreadyPassed(theTime.toString());
   _time.advance = kind;
   _time.requested = t;
   for (Callback& cb : _roHeld) // RO messages are delivered while advancing
      enqueue(std::move(cb));
   _roHeld.clear();
   evaluateTime();
   publishTimeState(true);
}

void MqttRTIambassador::timeAdvanceRequest(LogicalTime const& theTime)
{
   startAdvance(Advance::TAR, theTime);
}

void MqttRTIambassador::timeAdvanceRequestAvailable(LogicalTime const& theTime)
{
   startAdvance(Advance::TARA, theTime);
}

void MqttRTIambassador::nextMessageRequest(LogicalTime const& theTime)
{
   startAdvance(Advance::NER, theTime);
}

void MqttRTIambassador::nextMessageRequestAvailable(LogicalTime const& theTime)
{
   startAdvance(Advance::NERA, theTime);
}

void MqttRTIambassador::flushQueueRequest(LogicalTime const& theTime)
{
   startAdvance(Advance::FQR, theTime);
}

void MqttRTIambassador::enableAsynchronousDelivery()
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   if (_time.asyncDelivery)
      throw AsynchronousDeliveryAlreadyEnabled(L"asynchronous delivery already enabled");
   _time.asyncDelivery = true;
   for (Callback& cb : _roHeld)
      enqueue(std::move(cb));
   _roHeld.clear();
}

void MqttRTIambassador::disableAsynchronousDelivery()
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   if (!_time.asyncDelivery)
      throw AsynchronousDeliveryAlreadyDisabled(L"asynchronous delivery already disabled");
   _time.asyncDelivery = false;
}

bool MqttRTIambassador::queryGALT(LogicalTime& theTime)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   Bound g;
   if (!galt(g))
      return false;
   g.value.assignTo(theTime);
   return true;
}

void MqttRTIambassador::queryLogicalTime(LogicalTime& theTime)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   _time.time.assignTo(theTime);
}

bool MqttRTIambassador::queryLITS(LogicalTime& theTime)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   Bound g;
   bool const hasG = galt(g);
   TimeValue lits = queueMin();
   if (hasG && g.value < lits)
      lits = g.value;
   if (lits.infinite)
      return false;
   lits.assignTo(theTime);
   return true;
}

void MqttRTIambassador::modifyLookahead(LogicalTimeInterval const& theLookahead)
{
   std::lock_guard<std::mutex> lock(_stateMutex);
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   if (!_time.regulating)
      throw TimeRegulationIsNotEnabled(L"time regulation is not enabled");
   if (_time.advance != Advance::None)
      throw InTimeAdvancingState(L"in time advancing state");
   _time.lookahead = TimeValue::ofInterval(theLookahead, timeIsInteger());
   // A smaller lookahead takes effect as the time advances (promise floor).
   evaluateTime();
   publishTimeState(true);
}

void MqttRTIambassador::queryLookahead(LogicalTimeInterval& interval)
{
   checkJoined();
   std::lock_guard<std::mutex> v(_viewMutex);
   if (!_time.regulating)
      throw TimeRegulationIsNotEnabled(L"time regulation is not enabled");
   _time.lookahead.toInterval(interval);
}
} // namespace hla_mqtt
