#include "MqttTransport.h"

#include "Log.h"
#include "Version.h"
#include "Utf.h"

#include <mqtt/async_client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <vector>
#include <stdexcept>
#include <thread>

namespace hla_mqtt
{
namespace
{
std::string presencePayload(char const* state, std::string const& clientId)
{
   return std::string("{\"state\":\"") + state + "\",\"clientId\":\"" + clientId +
          "\",\"rti\":\"" + toUtf8(HLA_MQTT_RTI_NAME) + "\",\"version\":\"" +
          toUtf8(HLA_MQTT_RTI_VERSION) + "\"}";
}
}

struct MqttTransport::Impl
{
   Impl(LocalSettings const& s) : settings(s), client(s.brokerUri, s.clientId, createOptions()) {}

   static mqtt::create_options createOptions()
   {
      // Paho C refuses a publication once maxBufferedMessages commands wait in
      // its queue (default 100): a larger queue, and the sender retries anyway.
      mqtt::create_options o(MQTTVERSION_3_1_1, 100000);
      o.set_send_while_disconnected(true, false); // buffered during an outage
      return o;
   }

   LocalSettings settings;
   mqtt::async_client client;
   MessageHandler onMessage;
   ConnectionLostHandler onLost;
   ReconnectedHandler onReconnected;
   std::string presenceTopic, presencePrefix, onlinePayload;

   // Housekeeping thread: reconnection, stale presences (houseMutex).
   std::mutex houseMutex;
   std::condition_variable houseCv;
   std::thread house;
   bool stopHouse = false;
   bool leaving = false;     // clean disconnect in progress: no reconnection, no presence repair
   bool reconnecting = false;
   std::string lostCause;
   std::chrono::steady_clock::time_point lostAt, nextAttempt;
   std::set<std::string> subscriptions;                            // made again on reconnection
   std::map<std::string, std::chrono::steady_clock::time_point> offline; // other clients' presences
   bool repairPresence = false;
   std::string syncTopic;
   std::mutex syncMutex;
   std::condition_variable syncCv;
   std::uint64_t lastSyncSeen = 0;
   std::atomic<std::uint64_t> nextSync{0};

   // Outbox: every publication goes through one FIFO served by one thread.
   // Callers never call Paho themselves, so a caller holding its own lock
   // cannot deadlock with the Paho callback thread waiting for that lock,
   // and the order of all publications is the order of the calls.
   struct Outgoing
   {
      std::string topic;
      std::string payload;
      int qos;
      bool retained;
      std::shared_ptr<std::promise<mqtt::delivery_token_ptr>> done; // null: fire and forget
   };
   std::mutex outMutex;
   std::condition_variable outCv;
   std::deque<Outgoing> out;
   bool stopSender = false;
   bool sending = false;
   std::thread sender;

   void runSender()
   {
      std::unique_lock<std::mutex> lock(outMutex);
      for (;;)
      {
         outCv.wait(lock, [this] { return stopSender || !out.empty(); });
         if (out.empty())
            return; // stopping, queue drained
         Outgoing o = std::move(out.front());
         out.pop_front();
         sending = true;
         lock.unlock();
         try
         {
            mqtt::delivery_token_ptr tok = handOver(o);
            if (o.done)
               o.done->set_value(tok);
         }
         catch (...)
         {
            if (o.done)
               o.done->set_exception(std::current_exception());
            else
               reportDropped(o, std::current_exception());
         }
         lock.lock();
         sending = false;
         outCv.notify_all();
      }
   }

   // Hands one publication to Paho. A full Paho queue is back-pressure, not a
   // failure: wait for it to drain instead of losing the message (losing one
   // update silently breaks the order the receivers rely on).
   mqtt::delivery_token_ptr handOver(Outgoing const& o)
   {
      auto const deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
      for (;;)
      {
         try
         {
            HLA_MQTT_LOG(Debug, settings.clientId,
                         "publish " << o.topic << " (" << o.payload.size() << " bytes" << (o.retained ? ", retained)" : ")"));
            return client.publish(o.topic, o.payload.data(), o.payload.size(), o.qos, o.retained);
         }
         catch (mqtt::exception const& e)
         {
            if (e.get_return_code() != MQTTASYNC_MAX_BUFFERED_MESSAGES || !client.is_connected() ||
                std::chrono::steady_clock::now() > deadline)
               throw;
            {
               std::lock_guard<std::mutex> lock(outMutex);
               if (stopSender)
                  throw;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
         }
      }
   }

   // A fire-and-forget publication that could not be handed to Paho: reported
   // once on stderr, since its caller is gone.
   std::atomic<std::uint64_t> dropped{0};
   void reportDropped(Outgoing const& o, std::exception_ptr ep)
   {
      if (dropped++ != 0)
         return;
      std::string why = "unknown error";
      try
      {
         std::rethrow_exception(ep);
      }
      catch (std::exception const& e)
      {
         why = e.what();
      }
      catch (...)
      {
      }
      HLA_MQTT_LOG(Error, settings.clientId, "publication on " << o.topic << " lost: " << why);
   }

   void enqueue(Outgoing o)
   {
      {
         std::lock_guard<std::mutex> lock(outMutex);
         out.push_back(std::move(o));
      }
      outCv.notify_all();
   }

   // Waits until every queued publication has been handed to Paho.
   void drain(std::chrono::milliseconds timeout)
   {
      std::unique_lock<std::mutex> lock(outMutex);
      outCv.wait_for(lock, timeout, [this] { return out.empty() && !sending; });
   }

   // Paho thread: unexpected loss of the connection.
   void connectionLost(std::string const& cause)
   {
      {
         std::lock_guard<std::mutex> lock(houseMutex);
         if (leaving || stopHouse)
            return;
         HLA_MQTT_LOG(Warning, settings.clientId, "connection to " << settings.brokerUri << " lost: " << cause
                                                 << (settings.reconnectSeconds > 0 ? " (reconnecting)" : ""));
         if (settings.reconnectSeconds > 0)
         {
            reconnecting = true;
            lostCause = cause;
            lostAt = nextAttempt = std::chrono::steady_clock::now();
            houseCv.notify_all();
            return;
         }
      }
      if (onLost)
         onLost(cause);
   }

   // Paho thread: presence records (other clients' and ours).
   void presence(std::string const& topic, std::string const& payload)
   {
      bool const online = payload.find("\"state\":\"online\"") != std::string::npos;
      std::lock_guard<std::mutex> lock(houseMutex);
      if (topic == presenceTopic)
      {
         if (!online && !leaving)
         {
            repairPresence = true; // removed by another client, or our own Last Will
            houseCv.notify_all();
         }
         return;
      }
      if (!payload.empty() && !online)
         offline.emplace(topic, std::chrono::steady_clock::now()); // keeps the first sighting
      else
         offline.erase(topic);
   }

   void runHouse()
   {
      std::unique_lock<std::mutex> lock(houseMutex);
      for (;;)
      {
         houseCv.wait_for(lock, std::chrono::milliseconds(250));
         if (stopHouse)
            return;
         auto const now = std::chrono::steady_clock::now();
         if (reconnecting && !leaving)
         {
            if (now - lostAt > std::chrono::seconds(settings.reconnectSeconds))
            {
               reconnecting = false;
               std::string const cause = lostCause;
               HLA_MQTT_LOG(Error, settings.clientId,
                            "no reconnection to " << settings.brokerUri << " within " << settings.reconnectSeconds << " s");
               lock.unlock();
               if (onLost)
                  onLost(cause + " (no reconnection within " + std::to_string(settings.reconnectSeconds) + " s)");
               lock.lock();
               continue;
            }
            if (now < nextAttempt)
               continue;
            nextAttempt = now + std::chrono::seconds(1);
            std::set<std::string> const subs = subscriptions;
            lock.unlock();
            bool const ok = tryReconnect(subs);
            lock.lock();
            if (!ok || leaving || stopHouse)
               continue;
            reconnecting = false;
            repairPresence = false;
            HLA_MQTT_LOG(Info, settings.clientId, "reconnected to " << settings.brokerUri);
            lock.unlock();
            if (onReconnected)
               onReconnected(ReconnectPhase::Synchronized);
            lock.lock();
            continue;
         }
         if (leaving)
            continue;
         // Paho and the outbox are called without houseMutex: the Paho
         // thread takes houseMutex (presence, connectionLost) and Paho may
         // hold its own lock meanwhile.
         lock.unlock();
         bool const connected = client.is_connected();
         lock.lock();
         if (!connected || leaving || reconnecting || stopHouse)
            continue;
         std::vector<Outgoing> out;
         if (repairPresence)
         {
            repairPresence = false;
            out.push_back(Outgoing{presenceTopic, onlinePayload, 1, true, nullptr});
         }
         auto const delay = std::chrono::seconds(settings.staleClientDelay());
         for (auto it = offline.begin(); it != offline.end();)
         {
            if (now - it->second < delay)
            {
               ++it;
               continue;
            }
            HLA_MQTT_LOG(Info, settings.clientId, "stale presence removed: " << it->first);
            out.push_back(Outgoing{it->first, std::string(), 1, true, nullptr}); // stale presence removed
            it = offline.erase(it);
         }
         if (out.empty())
            continue;
         lock.unlock();
         for (Outgoing& o : out)
            enqueue(std::move(o));
         lock.lock();
      }
   }

   // Housekeeping thread, no lock held.
   bool tryReconnect(std::set<std::string> const& subs)
   {
      try
      {
         auto const timeout = std::chrono::seconds(settings.connectTimeoutSeconds + 1);
         mqtt::token_ptr tok = client.reconnect();
         if (!tok->wait_for(timeout))
            return false;
         enqueue(Outgoing{presenceTopic, onlinePayload, 1, true, nullptr});
         if (onReconnected)
            onReconnected(ReconnectPhase::Resubscribing);
         for (std::string const& t : subs) // clean session: retained records come again
            if (!client.subscribe(t, 1)->wait_for(timeout))
               return false;
         // Round trip: the broker sends the retained records of a subscription
         // before the messages published after it.
         std::uint64_t const n = ++nextSync;
         if (!client.publish(syncTopic, std::to_string(n), 1, false)->wait_for(timeout))
            return false;
         std::unique_lock<std::mutex> lock(syncMutex);
         return syncCv.wait_for(lock, timeout, [this, n] { return lastSyncSeen >= n; });
      }
      catch (std::exception const&)
      {
         return false;
      }
   }

   void stopHousekeeping()
   {
      {
         std::lock_guard<std::mutex> lock(houseMutex);
         stopHouse = true;
      }
      houseCv.notify_all();
      if (house.joinable())
         house.join();
   }

   void stop()
   {
      stopHousekeeping();
      {
         std::lock_guard<std::mutex> lock(outMutex);
         stopSender = true;
      }
      outCv.notify_all();
      if (sender.joinable())
         sender.join();
   }
};

MqttTransport::MqttTransport(LocalSettings settings, ConnectionLostHandler onLost, MessageHandler onMessage)
   : _settings(std::move(settings))
{
   // Log settings of this connection (process-wide, see Log.h).
   if (!_settings.logLevel.empty())
      log::setLevel(log::parseLevel(_settings.logLevel));
   if (_settings.logFileSet)
      log::setFile(_settings.logFile);
#if !HLA_MQTT_WITH_SSL
   if (_settings.tls())
      throw std::runtime_error("this RTI library is built without TLS support (Conan option with_ssl=True), "
                               "cannot connect to " +
                               _settings.brokerUri);
#endif
   try
   {
      _impl.reset(new Impl(_settings));
   }
   catch (mqtt::exception const& e)
   {
      throw std::runtime_error(std::string("MQTT client creation failed: ") + e.what());
   }
   Impl* impl = _impl.get();
   _impl->onLost = std::move(onLost);
   _impl->presenceTopic = presenceTopic();
   _impl->presencePrefix = _settings.topicPrefix + "/clients/";
   _impl->onlinePayload = presencePayload("online", _settings.clientId);
   _impl->client.set_connection_lost_handler(
      [impl](std::string const& cause)
      { impl->connectionLost(cause.empty() ? std::string("connection to MQTT broker lost") : cause); });
   _impl->onMessage = std::move(onMessage);
   _impl->sender = std::thread([impl] { impl->runSender(); });
   _impl->house = std::thread([impl] { impl->runHouse(); });
   _impl->syncTopic = _settings.topicPrefix + "/sync/" + _settings.clientId;
   _impl->client.set_message_callback(
      [impl](mqtt::const_message_ptr msg)
      {
         std::string const& topic = msg->get_topic();
         if (topic == impl->syncTopic)
         {
            std::uint64_t n = std::strtoull(msg->get_payload_str().c_str(), nullptr, 10);
            {
               std::lock_guard<std::mutex> lock(impl->syncMutex);
               if (n > impl->lastSyncSeen)
                  impl->lastSyncSeen = n;
            }
            impl->syncCv.notify_all();
            return;
         }
         HLA_MQTT_LOG(Debug, impl->settings.clientId,
                      "received " << topic << " (" << msg->get_payload().size() << " bytes"
                                  << (msg->is_retained() ? ", retained)" : ")"));
         if (topic.compare(0, impl->presencePrefix.size(), impl->presencePrefix) == 0)
            impl->presence(topic, msg->get_payload_str());
         if (impl->onMessage)
         {
            try
            {
               impl->onMessage(topic, msg->get_payload_str());
            }
            catch (...)
            {
            }
         }
      });
}

MqttTransport::~MqttTransport()
{
   disconnect();
   if (_impl)
      _impl->stop();
}

void MqttTransport::setReconnectedHandler(ReconnectedHandler h)
{
   _impl->onReconnected = std::move(h);
}

std::string MqttTransport::presenceTopic() const
{
   return _settings.topicPrefix + "/clients/" + _settings.clientId;
}

void MqttTransport::connect()
{
   auto will = mqtt::message(presenceTopic(),
                             presencePayload("offline", _settings.clientId), 1, true);

   auto builder = mqtt::connect_options_builder()
                     .clean_session(true) // nothing left on the broker; subscriptions made again on reconnection
                     .automatic_reconnect(false)
                     .keep_alive_interval(std::chrono::seconds(_settings.keepAliveSeconds))
                     .connect_timeout(std::chrono::seconds(_settings.connectTimeoutSeconds))
                     .will(will);
   if (!_settings.username.empty())
      builder.user_name(_settings.username);
   if (!_settings.password.empty())
      builder.password(_settings.password);
#if HLA_MQTT_WITH_SSL
   if (_settings.tls())
   {
      // Server verification (certificate chain and host name) unless
      // verifyServer=false; client certificate when certFile is set.
      mqtt::ssl_options_builder ssl;
      if (!_settings.caFile.empty())
         ssl.trust_store(_settings.caFile);
      if (!_settings.caPath.empty())
         ssl.ca_path(_settings.caPath);
      if (!_settings.certFile.empty())
         ssl.key_store(_settings.certFile);
      if (!_settings.keyFile.empty())
         ssl.private_key(_settings.keyFile);
      if (!_settings.keyPassword.empty())
         ssl.private_keypassword(_settings.keyPassword);
      ssl.enable_server_cert_auth(_settings.verifyServer);
      ssl.verify(_settings.verifyServer);
      std::string const clientId = _settings.clientId;
      ssl.error_handler([clientId](std::string const& msg) { HLA_MQTT_LOG(Warning, clientId, "TLS: " << msg); });
      builder.ssl(ssl.finalize());
   }
#endif

   {
      std::lock_guard<std::mutex> lock(_impl->houseMutex);
      _impl->leaving = true; // until connected: a failed connect leaves nothing to clean
   }
   try
   {
      auto timeout = std::chrono::seconds(_settings.connectTimeoutSeconds + 1);
      auto tok = _impl->client.connect(builder.finalize());
      if (!tok->wait_for(timeout))
         throw std::runtime_error("timeout while connecting to " + _settings.brokerUri);
      {
         std::lock_guard<std::mutex> lock(_impl->houseMutex);
         _impl->leaving = false;
      }
      subscribe(_impl->syncTopic);
      publish(presenceTopic(), presencePayload("online", _settings.clientId), 1, true);
      HLA_MQTT_LOG(Info, _settings.clientId,
                   "connected to " << _settings.brokerUri << " (prefix " << _settings.topicPrefix << ")");
   }
   catch (mqtt::exception const& e)
   {
      HLA_MQTT_LOG(Error, _settings.clientId, "cannot connect to " << _settings.brokerUri << ": " << e.what());
      throw std::runtime_error("cannot connect to " + _settings.brokerUri + ": " + e.what());
   }
}

void MqttTransport::disconnect() noexcept
{
   if (!_impl)
      return;
   {
      std::lock_guard<std::mutex> lock(_impl->houseMutex);
      if (_impl->leaving)
         return; // already disconnected (disconnect, then the destructor)
      _impl->leaving = true;
      _impl->reconnecting = false;
   }
   try
   {
      if (_impl->client.is_connected())
      {
         // Clean disconnect: pending publications first, then remove the
         // retained presence record.
         _impl->drain(std::chrono::seconds(2));
         publish(presenceTopic(), std::string(), 1, true);
         _impl->client.disconnect()->wait_for(std::chrono::seconds(2));
         HLA_MQTT_LOG(Info, _settings.clientId, "disconnected from " << _settings.brokerUri);
      }
   }
   catch (...)
   {
   }
}

bool MqttTransport::isConnected() const
{
   return _impl && _impl->client.is_connected();
}

void MqttTransport::subscribe(std::string const& topicFilter)
{
   try
   {
      if (!_impl->client.subscribe(topicFilter, 1)->wait_for(std::chrono::seconds(_settings.connectTimeoutSeconds)))
         throw std::runtime_error("timeout while subscribing to " + topicFilter);
      std::lock_guard<std::mutex> lock(_impl->houseMutex);
      _impl->subscriptions.insert(topicFilter);
   }
   catch (mqtt::exception const& e)
   {
      throw std::runtime_error("MQTT subscribe failed on " + topicFilter + ": " + e.what());
   }
}

void MqttTransport::unsubscribe(std::string const& topicFilter) noexcept
{
   try
   {
      {
         std::lock_guard<std::mutex> lock(_impl->houseMutex);
         _impl->subscriptions.erase(topicFilter);
      }
      if (_impl->client.is_connected())
         _impl->client.unsubscribe(topicFilter)->wait_for(std::chrono::seconds(_settings.connectTimeoutSeconds));
   }
   catch (...)
   {
   }
}

void MqttTransport::barrier()
{
   std::uint64_t const n = ++_impl->nextSync;
   publish(_impl->syncTopic, std::to_string(n), 1, false);
   std::unique_lock<std::mutex> lock(_impl->syncMutex);
   if (!_impl->syncCv.wait_for(lock, std::chrono::seconds(_settings.connectTimeoutSeconds),
                               [this, n] { return _impl->lastSyncSeen >= n; }))
      throw std::runtime_error("timeout waiting for the MQTT broker round trip");
}

void MqttTransport::publish(std::string const& topic, std::string const& payload,
                            int qos, bool retained)
{
   auto done = std::make_shared<std::promise<mqtt::delivery_token_ptr>>();
   std::future<mqtt::delivery_token_ptr> handed = done->get_future();
   _impl->enqueue(Impl::Outgoing{topic, payload, qos, retained, done});
   try
   {
      auto const timeout = std::chrono::seconds(_settings.connectTimeoutSeconds);
      if (handed.wait_for(timeout) != std::future_status::ready)
         throw std::runtime_error("MQTT publish on " + topic + ": timeout");
      mqtt::delivery_token_ptr tok = handed.get();
      if (tok && !tok->wait_for(timeout))
         throw std::runtime_error("MQTT publish on " + topic + ": no acknowledgement");
   }
   catch (mqtt::exception const& e)
   {
      throw std::runtime_error("MQTT publish failed on " + topic + ": " + e.what());
   }
}

void MqttTransport::publishAsync(std::string const& topic, std::string const& payload,
                                 int qos, bool retained)
{
   _impl->enqueue(Impl::Outgoing{topic, payload, qos, retained, nullptr});
}
}
