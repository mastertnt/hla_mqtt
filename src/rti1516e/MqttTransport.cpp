#include "MqttTransport.h"

#include "Version.h"
#include "Utf.h"

#include <mqtt/async_client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <future>
#include <mutex>
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
   Impl(LocalSettings const& s)
      : client(s.brokerUri, s.clientId, mqtt::create_options(MQTTVERSION_3_1_1))
   {
   }
   mqtt::async_client client;
   MessageHandler onMessage;
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
            mqtt::delivery_token_ptr tok =
               client.publish(o.topic, o.payload.data(), o.payload.size(), o.qos, o.retained);
            if (o.done)
               o.done->set_value(tok);
         }
         catch (...)
         {
            if (o.done)
               o.done->set_exception(std::current_exception());
         }
         lock.lock();
         sending = false;
         outCv.notify_all();
      }
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

   void stop()
   {
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
   try
   {
      _impl.reset(new Impl(_settings));
   }
   catch (mqtt::exception const& e)
   {
      throw std::runtime_error(std::string("MQTT client creation failed: ") + e.what());
   }
   _impl->client.set_connection_lost_handler(
      [onLost](std::string const& cause)
      {
         if (onLost)
            onLost(cause.empty() ? std::string("connection to MQTT broker lost") : cause);
      });
   _impl->onMessage = std::move(onMessage);
   _impl->sender = std::thread([impl = _impl.get()] { impl->runSender(); });
   _impl->syncTopic = _settings.topicPrefix + "/sync/" + _settings.clientId;
   Impl* impl = _impl.get();
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

std::string MqttTransport::presenceTopic() const
{
   return _settings.topicPrefix + "/clients/" + _settings.clientId;
}

void MqttTransport::connect()
{
   auto will = mqtt::message(presenceTopic(),
                             presencePayload("offline", _settings.clientId), 1, true);

   auto builder = mqtt::connect_options_builder()
                     .clean_session(true)
                     .automatic_reconnect(false)
                     .keep_alive_interval(std::chrono::seconds(_settings.keepAliveSeconds))
                     .connect_timeout(std::chrono::seconds(_settings.connectTimeoutSeconds))
                     .will(will);
   if (!_settings.username.empty())
      builder.user_name(_settings.username);
   if (!_settings.password.empty())
      builder.password(_settings.password);

   try
   {
      auto timeout = std::chrono::seconds(_settings.connectTimeoutSeconds + 1);
      auto tok = _impl->client.connect(builder.finalize());
      if (!tok->wait_for(timeout))
         throw std::runtime_error("timeout while connecting to " + _settings.brokerUri);
      subscribe(_impl->syncTopic);
      publish(presenceTopic(), presencePayload("online", _settings.clientId), 1, true);
   }
   catch (mqtt::exception const& e)
   {
      throw std::runtime_error("cannot connect to " + _settings.brokerUri + ": " + e.what());
   }
}

void MqttTransport::disconnect() noexcept
{
   if (!_impl)
      return;
   try
   {
      if (_impl->client.is_connected())
      {
         // Clean disconnect: pending publications first, then remove the
         // retained presence record.
         _impl->drain(std::chrono::seconds(2));
         publish(presenceTopic(), std::string(), 1, true);
         _impl->client.disconnect()->wait_for(std::chrono::seconds(2));
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
