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
// Session: clean (nothing stays on the broker). Publications made while
// disconnected are buffered by Paho and sent on reconnection; non-retained
// messages published by the others during an outage are lost, retained
// records come again with the subscriptions.
//
// Reconnection (LocalSettings::reconnectSeconds): after an unexpected loss, a
// housekeeping thread reconnects; the subscriptions are made again (with a
// round trip, so that the retained records have all come again), the
// presence goes "online" again, and the
// ReconnectedHandler runs. ConnectionLostHandler only runs once the
// reconnection window is over (at once when it is 0).
//
// Presence housekeeping (needs a subscription to <prefix>/clients/+): a
// presence left "offline" for staleClientDelay() is removed (absent and
// offline mean the same). A client that sees its own presence removed or
// offline while connected publishes it "online" again.
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
   // Reconnection phases: Resubscribing (retained records about to come
   // again), then Synchronized (every retained record under the
   // subscriptions has come again: a record that did not has been removed
   // during the outage).
   enum class ReconnectPhase { Resubscribing, Synchronized };
   using ReconnectedHandler = std::function<void(ReconnectPhase)>;

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

   // Runs on the housekeeping thread, without any transport lock held. Set it
   // before connect().
   void setReconnectedHandler(ReconnectedHandler h);

   LocalSettings const& settings() const { return _settings; }
   std::string presenceTopic() const;

private:
   struct Impl;
   LocalSettings _settings;
   std::unique_ptr<Impl> _impl;
};
}
