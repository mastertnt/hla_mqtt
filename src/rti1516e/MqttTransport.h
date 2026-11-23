// Thin wrapper around the Paho MQTT C++ client.
// Keeps Paho out of the ambassador header and centralizes topic naming.
//
// Topics (prefix = LocalSettings::topicPrefix, "hla" by default):
//   <prefix>/clients/<clientId>   retained presence record
//       {"state":"online", ...}   published after connect
//       {"state":"offline",...}   Last Will (published by the broker on loss)
//       empty retained payload    clean disconnect (record removed)
//   <prefix>/sync/<clientId>      private round trip used by barrier()
//
// Incoming messages (other than sync) go to the MessageHandler, called on a
// Paho thread: it must not block nor call back into the transport.
#pragma once

#include "LocalSettings.h"

#include <functional>
#include <memory>
#include <string>

namespace hla_mqtt
{
class MqttTransport
{
public:
   using ConnectionLostHandler = std::function<void(std::string const& cause)>;
   using MessageHandler = std::function<void(std::string const& topic, std::string const& payload)>;

   MqttTransport(LocalSettings settings, ConnectionLostHandler onLost, MessageHandler onMessage = nullptr);
   ~MqttTransport();

   MqttTransport(MqttTransport const&) = delete;
   MqttTransport& operator=(MqttTransport const&) = delete;

   // Throws std::runtime_error on failure.
   void connect();
   // Never throws; safe to call when already disconnected.
   void disconnect() noexcept;
   bool isConnected() const;

   // Waits for the broker acknowledgement (QoS 1) or the send (QoS 0).
   void publish(std::string const& topic, std::string const& payload,
                int qos, bool retained);
   // Does not wait (queued). Every publication, synchronous or not, goes
   // through one ordered outbox served by a dedicated thread: safe to call
   // while holding a lock that the message handler also takes.
   void publishAsync(std::string const& topic, std::string const& payload,
                     int qos, bool retained);

   // QoS 1. Waits for the SUBACK; retained messages of the filter are
   // delivered to the MessageHandler. Throws std::runtime_error.
   void subscribe(std::string const& topicFilter);
   void unsubscribe(std::string const& topicFilter) noexcept;

   // Returns once every message the broker sent before this call (retained
   // messages of earlier subscriptions included) has gone through the
   // MessageHandler. Throws std::runtime_error on timeout.
   void barrier();

   LocalSettings const& settings() const { return _settings; }
   std::string presenceTopic() const;

private:
   struct Impl;
   LocalSettings _settings;
   std::unique_ptr<Impl> _impl;
};
}
