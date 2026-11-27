// hla-load: one federate per process, for load tests over several processes
// and machines (#43) and for the performance bench (#42). Needs a broker.
//
//   hla-load --index I --federates N [options]
//
// All the processes of a run share the federation name and N. Index 0 drives
// the synchronization points, once it has discovered the N HLAfederate MOM
// objects (every federate joined). Modes:
//
//   load      every federate registers --objects entities, then updates each
//             of them --updates times (--attributes attributes of --payload
//             bytes). Every federate checks that it discovered the entities of
//             the others and received each of their updates exactly once.
//   pingpong  index 0 sends --count Ping interactions, one at a time; every
//             other federate answers each with a Pong. Index 0 measures the
//             round trip (all the answers received).
//
// Output: progress on stderr, then one line on stdout:
//   RESULT {"index":0,"mode":"load",...}
// Exit code: 0 success, 1 check failed, 2 timeout or RTI error.
//
// Options:
//   --settings S      local settings designator (default: HLA_MQTT_BROKER, or
//                     mqtt://localhost:1883); HLA_MQTT_SETTINGS also applies
//   --federation F    federation execution name (default HlaLoad)
//   --fom PATH        FOM module (default: the LoadFom.xml built in)
//   --mode M          load (default) or pingpong
//   --objects K       entities per federate (load, default 10)
//   --attributes A    attributes per update, 1 to 32 (load, default 4)
//   --updates U       updates per entity (load, default 100)
//   --payload B       bytes per attribute or parameter (default 64, minimum 16)
//   --count M         round trips (pingpong, default 1000)
//   --timeout S       limit of each wait, in seconds (default 120)

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS // std::getenv
#endif

#include <RTI/NullFederateAmbassador.h>
#include <RTI/RTI1516.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#ifndef HLA_LOAD_FOM
#define HLA_LOAD_FOM "LoadFom.xml"
#endif

using namespace rti1516e;
using Clock = std::chrono::steady_clock;

namespace
{
struct Options
{
   std::wstring settings;
   std::wstring federation = L"HlaLoad";
   std::wstring fom = L"" HLA_LOAD_FOM;
   std::string mode = "load";
   int index = -1;
   int federates = 0;
   int objects = 10;
   int attributes = 4;
   int updates = 100;
   int payload = 64;
   int count = 1000;
   double timeout = 120;
};

std::wstring widen(std::string const& s)
{
   return std::wstring(s.begin(), s.end());
}

std::string narrow(std::wstring const& s)
{
   std::string out;
   for (wchar_t c : s)
      out += (c < 128) ? char(c) : '?';
   return out;
}

std::int64_t nowNs()
{
   return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

void put64(std::vector<char>& b, size_t at, std::uint64_t v)
{
   for (int i = 0; i < 8; ++i)
      b[at + i] = char((v >> (56 - 8 * i)) & 0xff);
}

std::uint64_t get64(VariableLengthData const& d, size_t at)
{
   if (d.size() < at + 8)
      return 0;
   unsigned char const* p = static_cast<unsigned char const*>(d.data()) + at;
   std::uint64_t v = 0;
   for (int i = 0; i < 8; ++i)
      v = (v << 8) | p[i];
   return v;
}

struct Failure
{
   std::string what;
};

struct Federate : NullFederateAmbassador
{
   Options const& opt;
   std::unique_ptr<RTIambassador> rti;
   FederateHandle self;

   ObjectClassHandle entity, momFederate;
   std::vector<AttributeHandle> attributes;
   AttributeHandle momFederateHandle;
   InteractionClassHandle ping, pong;
   ParameterHandle pingSeq, pingPayload, pongSeq, pongPayload;

   std::set<ObjectInstanceHandle> momFederates;
   std::set<ObjectInstanceHandle> entities;
   std::map<ObjectInstanceHandle, std::uint64_t> lastUpdate; // per foreign entity
   std::uint64_t reflects = 0;
   std::uint64_t duplicatesOrGaps = 0;
   std::vector<double> latenciesMs; // one-way, same host only
   Clock::time_point lastReflect;

   std::set<std::wstring> announced, synchronized;

   std::vector<std::uint64_t> pendingPongs; // pongs to send after the callback
   std::uint64_t pongsFor = 0;              // pinger: sequence awaited
   int pongsReceived = 0;
   bool checkFailed = false;

   explicit Federate(Options const& o) : opt(o)
   {
   }

   // ------------------------------------------------------------ callbacks
   void announceSynchronizationPoint(std::wstring const& label, VariableLengthData const&) override
   {
      announced.insert(label);
   }
   void federationSynchronized(std::wstring const& label, FederateHandleSet const&) override
   {
      synchronized.insert(label);
   }
   void discoverObjectInstance(ObjectInstanceHandle o, ObjectClassHandle c, std::wstring const&) override
   {
      if (c == momFederate)
         momFederates.insert(o);
      else if (c == entity)
         entities.insert(o);
   }
   void discoverObjectInstance(ObjectInstanceHandle o, ObjectClassHandle c, std::wstring const& n,
                               FederateHandle) override
   {
      discoverObjectInstance(o, c, n);
   }
   void removeObjectInstance(ObjectInstanceHandle o, VariableLengthData const&, OrderType,
                             SupplementalRemoveInfo) override
   {
      momFederates.erase(o);
   }
   void reflectAttributeValues(ObjectInstanceHandle o, AttributeHandleValueMap const& values,
                               VariableLengthData const&, OrderType, TransportationType,
                               SupplementalReflectInfo) override
   {
      if (!entities.count(o))
         return; // MOM object
      ++reflects;
      lastReflect = Clock::now();
      auto it = values.find(attributes[0]);
      if (it == values.end())
         return;
      std::int64_t const sent = std::int64_t(get64(it->second, 0));
      std::uint64_t const seq = get64(it->second, 8);
      latenciesMs.push_back(double(nowNs() - sent) / 1e6);
      std::uint64_t& last = lastUpdate[o];
      if (seq != last + 1)
         ++duplicatesOrGaps; // reliable updates of one object arrive in order
      last = seq;
   }
   void receiveInteraction(InteractionClassHandle c, ParameterHandleValueMap const& values,
                           VariableLengthData const&, OrderType, TransportationType,
                           SupplementalReceiveInfo) override
   {
      if (c == ping)
      {
         auto it = values.find(pingSeq);
         if (it != values.end())
            pendingPongs.push_back(get64(it->second, 0));
      }
      else if (c == pong)
      {
         auto it = values.find(pongSeq);
         if (it != values.end() && get64(it->second, 0) == pongsFor)
            ++pongsReceived;
      }
   }

   // ---------------------------------------------------------------- waits
   template <class Done>
   void waitFor(Done done, std::string const& what)
   {
      auto const deadline = Clock::now() + std::chrono::duration<double>(opt.timeout);
      while (!done())
      {
         if (Clock::now() > deadline)
            throw Failure{"timeout waiting for " + what};
         rti->evokeMultipleCallbacks(0.0, 0.05);
         flushPongs();
      }
   }

   void flushPongs()
   {
      std::vector<std::uint64_t> todo;
      todo.swap(pendingPongs);
      for (std::uint64_t seq : todo)
      {
         std::vector<char> s(8);
         put64(s, 0, seq);
         std::vector<char> p(size_t(opt.payload), 'p');
         ParameterHandleValueMap v;
         v[pongSeq] = VariableLengthData(s.data(), s.size());
         v[pongPayload] = VariableLengthData(p.data(), p.size());
         rti->sendInteraction(pong, v, VariableLengthData());
      }
   }

   // Index 0 registers the point; everyone achieves it once announced.
   void sync(std::wstring const& label)
   {
      if (opt.index == 0)
         rti->registerFederationSynchronizationPoint(label, VariableLengthData());
      waitFor([&] { return announced.count(label) != 0; }, "announcement of " + narrow(label));
      rti->synchronizationPointAchieved(label);
      waitFor([&] { return synchronized.count(label) != 0; }, "synchronization " + narrow(label));
   }

   // --------------------------------------------------------------- phases
   void joinAll()
   {
      rti = RTIambassadorFactory().createRTIambassador();
      rti->connect(*this, HLA_EVOKED, opt.settings);
      try
      {
         rti->createFederationExecution(opt.federation, std::vector<std::wstring>{opt.fom});
      }
      catch (FederationExecutionAlreadyExists const&)
      {
      }
      std::wstring const name = L"load-" + std::to_wstring(opt.index);
      self = rti->joinFederationExecution(name, L"hla-load", opt.federation,
                                          std::vector<std::wstring>{opt.fom});

      entity = rti->getObjectClassHandle(L"Entity");
      AttributeHandleSet set;
      for (int i = 0; i < opt.attributes; ++i)
      {
         attributes.push_back(rti->getAttributeHandle(entity, L"A" + std::to_wstring(i)));
         set.insert(attributes.back());
      }
      rti->publishObjectClassAttributes(entity, set);
      rti->subscribeObjectClassAttributes(entity, set);

      momFederate = rti->getObjectClassHandle(L"HLAmanager.HLAfederate");
      momFederateHandle = rti->getAttributeHandle(momFederate, L"HLAfederateHandle");
      AttributeHandleSet mom;
      mom.insert(momFederateHandle);
      rti->subscribeObjectClassAttributes(momFederate, mom);

      ping = rti->getInteractionClassHandle(L"Ping");
      pong = rti->getInteractionClassHandle(L"Pong");
      pingSeq = rti->getParameterHandle(ping, L"Sequence");
      pingPayload = rti->getParameterHandle(ping, L"Payload");
      pongSeq = rti->getParameterHandle(pong, L"Sequence");
      pongPayload = rti->getParameterHandle(pong, L"Payload");
      if (opt.mode == "pingpong")
      {
         rti->publishInteractionClass(opt.index == 0 ? ping : pong);
         rti->subscribeInteractionClass(opt.index == 0 ? pong : ping);
      }

      waitFor([&] { return int(momFederates.size()) >= opt.federates; },
              std::to_string(opt.federates) + " joined federates");
      sync(L"ready");
   }

   void leave()
   {
      sync(L"done");
      rti->resignFederationExecution(CANCEL_THEN_DELETE_THEN_DIVEST);
      try
      {
         rti->destroyFederationExecution(opt.federation);
      }
      catch (FederatesCurrentlyJoined const&)
      {
      }
      catch (FederationExecutionDoesNotExist const&)
      {
      }
      rti->disconnect();
   }

   std::string runLoad()
   {
      std::vector<ObjectInstanceHandle> mine;
      for (int k = 0; k < opt.objects; ++k)
         mine.push_back(rti->registerObjectInstance(entity));
      std::uint64_t const expectedEntities = std::uint64_t(opt.federates - 1) * opt.objects;
      waitFor([&] { return entities.size() >= expectedEntities; }, "discovery of the entities");
      sync(L"registered");

      std::vector<char> value(size_t(opt.payload), 'v');
      auto const start = Clock::now();
      for (int u = 1; u <= opt.updates; ++u)
      {
         for (ObjectInstanceHandle const& o : mine)
         {
            put64(value, 0, std::uint64_t(nowNs()));
            put64(value, 8, std::uint64_t(u));
            AttributeHandleValueMap v;
            for (AttributeHandle const& a : attributes)
               v[a] = VariableLengthData(value.data(), value.size());
            rti->updateAttributeValues(o, v, VariableLengthData());
         }
         rti->evokeMultipleCallbacks(0.0, 0.0); // keep up with the incoming updates
      }
      double const sendSeconds = std::chrono::duration<double>(Clock::now() - start).count();
      std::uint64_t const expected = expectedEntities * std::uint64_t(opt.updates);
      try
      {
         waitFor([&] { return reflects >= expected; }, "the updates of the others");
      }
      catch (Failure& f)
      {
         // Usually a broker that drops messages for slow clients (Mosquitto
         // max_queued_messages, 1000 by default): see broker/mosquitto-hla.conf.
         f.what += " (" + std::to_string(reflects) + " of " + std::to_string(expected) +
                   " received; check the broker queue limits)";
         throw;
      }
      double const receiveSeconds = std::chrono::duration<double>(lastReflect - start).count();
      sync(L"updated");
      rti->evokeMultipleCallbacks(0.2, 0.2); // late duplicates, if any

      std::sort(latenciesMs.begin(), latenciesMs.end());
      auto pct = [&](double p) {
         return latenciesMs.empty() ? 0.0 : latenciesMs[std::min(latenciesMs.size() - 1, size_t(p * latenciesMs.size()))];
      };
      std::ostringstream r;
      r << "\"objects\":" << opt.objects << ",\"attributes\":" << opt.attributes << ",\"updates\":" << opt.updates
        << ",\"payload\":" << opt.payload << ",\"sent\":" << std::uint64_t(opt.objects) * opt.updates
        << ",\"expected\":" << expected << ",\"received\":" << reflects << ",\"discovered\":" << entities.size()
        << ",\"outOfOrder\":" << duplicatesOrGaps << ",\"sendSeconds\":" << sendSeconds
        << ",\"receiveSeconds\":" << receiveSeconds
        << ",\"receivedPerSecond\":" << (receiveSeconds > 0 ? double(reflects) / receiveSeconds : 0.0)
        << ",\"latencyMsP50\":" << pct(0.5) << ",\"latencyMsP99\":" << pct(0.99)
        << ",\"latencyMsMax\":" << (latenciesMs.empty() ? 0.0 : latenciesMs.back());
      for (ObjectInstanceHandle const& o : mine)
         rti->deleteObjectInstance(o, VariableLengthData());
      checkFailed = reflects != expected || duplicatesOrGaps != 0 || entities.size() != expectedEntities;
      return r.str();
   }

   std::string runPingPong()
   {
      std::vector<double> rtt;
      if (opt.index == 0)
      {
         int const answers = opt.federates - 1;
         std::vector<char> p(size_t(opt.payload), 'q');
         int const warmup = std::min(10, opt.count);
         for (int i = 1; i <= warmup + opt.count; ++i)
         {
            pongsFor = std::uint64_t(i);
            pongsReceived = 0;
            std::vector<char> s(8);
            put64(s, 0, pongsFor);
            ParameterHandleValueMap v;
            v[pingSeq] = VariableLengthData(s.data(), s.size());
            v[pingPayload] = VariableLengthData(p.data(), p.size());
            auto const t0 = Clock::now();
            rti->sendInteraction(ping, v, VariableLengthData());
            auto const deadline = t0 + std::chrono::duration<double>(opt.timeout);
            while (pongsReceived < answers)
            {
               if (Clock::now() > deadline)
                  throw Failure{"timeout waiting for pong " + std::to_string(i)};
               rti->evokeCallback(0.01);
            }
            if (i > warmup)
               rtt.push_back(std::chrono::duration<double, std::milli>(Clock::now() - t0).count());
         }
      }
      std::sort(rtt.begin(), rtt.end());
      auto pct = [&](double q) { return rtt.empty() ? 0.0 : rtt[std::min(rtt.size() - 1, size_t(q * rtt.size()))]; };
      double sum = 0;
      for (double x : rtt)
         sum += x;
      std::ostringstream r;
      r << "\"count\":" << rtt.size() << ",\"payload\":" << opt.payload
        << ",\"rttMsMean\":" << (rtt.empty() ? 0.0 : sum / double(rtt.size())) << ",\"rttMsP50\":" << pct(0.5)
        << ",\"rttMsP99\":" << pct(0.99) << ",\"rttMsMin\":" << (rtt.empty() ? 0.0 : rtt.front())
        << ",\"rttMsMax\":" << (rtt.empty() ? 0.0 : rtt.back());
      return r.str();
   }
};

int usage(char const* error)
{
   std::cerr << "hla-load: " << error << "\n"
             << "usage: hla-load --index I --federates N [--mode load|pingpong] [--settings S]\n"
                "       [--federation F] [--fom PATH] [--objects K] [--attributes A] [--updates U]\n"
                "       [--payload B] [--count M] [--timeout S]\n";
   return 2;
}
} // namespace

int main(int argc, char** argv)
{
   Options opt;
   if (char const* b = std::getenv("HLA_MQTT_BROKER"))
      opt.settings = widen(b);
   for (int i = 1; i < argc; ++i)
   {
      std::string const a = argv[i];
      if (i + 1 >= argc)
         return usage(("missing value after " + a).c_str());
      std::string const v = argv[++i];
      if (a == "--settings")
         opt.settings = widen(v);
      else if (a == "--federation")
         opt.federation = widen(v);
      else if (a == "--fom")
         opt.fom = widen(v);
      else if (a == "--mode")
         opt.mode = v;
      else if (a == "--index")
         opt.index = std::atoi(v.c_str());
      else if (a == "--federates")
         opt.federates = std::atoi(v.c_str());
      else if (a == "--objects")
         opt.objects = std::atoi(v.c_str());
      else if (a == "--attributes")
         opt.attributes = std::atoi(v.c_str());
      else if (a == "--updates")
         opt.updates = std::atoi(v.c_str());
      else if (a == "--payload")
         opt.payload = std::atoi(v.c_str());
      else if (a == "--count")
         opt.count = std::atoi(v.c_str());
      else if (a == "--timeout")
         opt.timeout = std::atof(v.c_str());
      else
         return usage(("unknown option " + a).c_str());
   }
   if (opt.index < 0 || opt.federates < 1 || opt.index >= opt.federates)
      return usage("--index and --federates are required (0 <= index < federates)");
   if (opt.mode != "load" && opt.mode != "pingpong")
      return usage("--mode is load or pingpong");
   if (opt.attributes < 1 || opt.attributes > 32 || opt.objects < 1 || opt.updates < 1 || opt.count < 1)
      return usage("out of range value");
   if (opt.mode == "pingpong" && opt.federates < 2)
      return usage("pingpong needs at least 2 federates");
   opt.payload = std::max(opt.payload, 16);

   Federate f(opt);
   int code = 0;
   std::string body;
   auto const t0 = Clock::now();
   try
   {
      f.joinAll();
      std::cerr << "[hla-load " << opt.index << "] " << opt.federates << " federates joined" << std::endl;
      body = opt.mode == "load" ? f.runLoad() : f.runPingPong();
      f.leave();
      if (f.checkFailed)
      {
         std::cerr << "[hla-load " << opt.index << "] FAIL: updates lost, duplicated or out of order" << std::endl;
         code = 1;
      }
   }
   catch (Failure const& e)
   {
      std::cerr << "[hla-load " << opt.index << "] FAIL: " << e.what << std::endl;
      code = e.what.compare(0, 7, "timeout") == 0 ? 2 : 1;
   }
   catch (Exception const& e)
   {
      std::cerr << "[hla-load " << opt.index << "] RTI exception: " << narrow(e.what()) << std::endl;
      code = 2;
   }
   double const total = std::chrono::duration<double>(Clock::now() - t0).count();
   std::cout << "RESULT {\"index\":" << opt.index << ",\"federates\":" << opt.federates << ",\"mode\":\""
             << opt.mode << "\",\"ok\":" << (code == 0 ? "true" : "false") << ",\"totalSeconds\":" << total
             << (body.empty() ? "" : ",") << body << "}" << std::endl;
   return code;
}
