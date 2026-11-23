#include "MqttRTIambassador.h"

#include "LocalSettings.h"
#include "MqttTransport.h"
#include "Utf.h"

#include <chrono>
#include <stdexcept>

namespace hla_mqtt
{
namespace
{
// Depth of callback delivery on the current thread: services that must not
// be called from a callback check it (CallNotAllowedFromWithinCallback).
thread_local int t_callbackDepth = 0;

struct CallbackScope
{
   CallbackScope() { ++t_callbackDepth; }
   ~CallbackScope() { --t_callbackDepth; }
};

using Clock = std::chrono::steady_clock;

Clock::duration toDuration(double seconds)
{
   if (!(seconds > 0.0))
      return Clock::duration::zero();
   if (seconds > 1e6)
      seconds = 1e6;
   return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}
}

MqttRTIambassador::MqttRTIambassador() = default;

MqttRTIambassador::~MqttRTIambassador()
{
   stopDispatcher();
   std::lock_guard<std::mutex> lock(_stateMutex);
   _transport.reset(); // clean MQTT disconnect if still connected
}

void MqttRTIambassador::notImplemented(wchar_t const* service) const
{
   throw RTIinternalError(std::wstring(L"hla-mqtt-rti: service '") + service +
                          L"' not implemented yet");
}

void MqttRTIambassador::checkNotInCallback() const
{
   if (t_callbackDepth > 0)
      throw CallNotAllowedFromWithinCallback(L"service called from within a callback");
}

// ------------------------------------------------------------------ 4.2
void MqttRTIambassador::connect(FederateAmbassador& federateAmbassador,
                                CallbackModel theCallbackModel,
                                std::wstring const& localSettingsDesignator)
{
   checkNotInCallback();
   std::lock_guard<std::mutex> lock(_stateMutex);

   if (_connected)
      throw AlreadyConnected(L"already connected to " +
                             fromUtf8(_transport->settings().brokerUri));
   if (theCallbackModel != HLA_EVOKED && theCallbackModel != HLA_IMMEDIATE)
      throw UnsupportedCallbackModel(L"unknown callback model");

   LocalSettings settings;
   try
   {
      settings = LocalSettings::parse(localSettingsDesignator);
   }
   catch (std::invalid_argument const& e)
   {
      throw InvalidLocalSettingsDesignator(fromUtf8(e.what()));
   }

   std::unique_ptr<MqttTransport> transport;
   try
   {
      {
         std::lock_guard<std::mutex> v(_viewMutex);
         _view.clear();
      }
      transport.reset(new MqttTransport(
         settings, [this](std::string const& cause) { onConnectionLost(cause); },
         [this](std::string const& topic, std::string const& payload) { onMessage(topic, payload); }));
      transport->connect();
      // Presence of every client: tells live federate records from stale ones.
      transport->subscribe(settings.topicPrefix + "/clients/+");
   }
   catch (std::exception const& e)
   {
      throw ConnectionFailed(fromUtf8(e.what()));
   }

   {
      std::lock_guard<std::mutex> qlock(_queueMutex);
      _queue.clear();
      _callbacksEnabled = true;
   }
   _transport = std::move(transport);
   _federateAmbassador = &federateAmbassador;
   _callbackModel = theCallbackModel;
   _joined = false;
   _connected = true;

   if (_callbackModel == HLA_IMMEDIATE)
      startDispatcher();
}

// ------------------------------------------------------------------ 4.3
void MqttRTIambassador::disconnect()
{
   checkNotInCallback();
   std::unique_ptr<MqttTransport> transport;
   {
      std::lock_guard<std::mutex> lock(_stateMutex);
      if (_joined)
         throw FederateIsExecutionMember(L"resign the federation execution before disconnecting");
      transport = std::move(_transport);
      _connected = false;
   }
   stopDispatcher();
   {
      std::lock_guard<std::mutex> qlock(_queueMutex);
      _queue.clear();
   }
   if (transport)
      transport->disconnect();
   _federateAmbassador = nullptr;
}

// ------------------------------------------------------------------ 4.4
// Called from a Paho thread: never call the federate from here, just queue.
void MqttRTIambassador::onConnectionLost(std::string const& cause)
{
   if (!_connected.exchange(false))
      return;
   // The federation sees our presence go "offline" (Last Will): our federate
   // record becomes stale, which amounts to an automatic resign.
   _joined = false;
   std::wstring description = L"MQTT: " + fromUtf8(cause);
   enqueue([description](FederateAmbassador& fa) { fa.connectionLost(description); });
}

// ------------------------------------------------------ callback machinery
void MqttRTIambassador::enqueue(Callback cb)
{
   {
      std::lock_guard<std::mutex> lock(_queueMutex);
      _queue.push_back(std::move(cb));
   }
   _queueCv.notify_all();
}

bool MqttRTIambassador::deliverable() const
{
   return _callbacksEnabled && !_queue.empty();
}

void MqttRTIambassador::deliverOne(std::unique_lock<std::mutex>& lock)
{
   Callback cb = std::move(_queue.front());
   _queue.pop_front();
   FederateAmbassador* fa = _federateAmbassador.load();
   lock.unlock();
   if (fa)
   {
      CallbackScope scope;
      try
      {
         cb(*fa);
      }
      catch (...)
      {
         // FederateInternalError or anything else thrown by the federate:
         // must not escape into the RTI threads.
      }
   }
   lock.lock();
}

void MqttRTIambassador::startDispatcher()
{
   stopDispatcher();
   {
      std::lock_guard<std::mutex> lock(_queueMutex);
      _stopDispatcher = false;
   }
   _dispatcher = std::thread([this]
   {
      std::unique_lock<std::mutex> lock(_queueMutex);
      for (;;)
      {
         _queueCv.wait(lock, [this] { return _stopDispatcher || deliverable(); });
         if (_stopDispatcher)
            return;
         deliverOne(lock);
      }
   });
}

void MqttRTIambassador::stopDispatcher()
{
   {
      std::lock_guard<std::mutex> lock(_queueMutex);
      _stopDispatcher = true;
   }
   _queueCv.notify_all();
   if (_dispatcher.joinable())
   {
      if (_dispatcher.get_id() == std::this_thread::get_id())
         _dispatcher.detach(); // disconnect() called from an immediate callback
      else
         _dispatcher.join();
   }
}

// ------------------------------------------------------------------ 10.41
bool MqttRTIambassador::evokeCallback(double approximateMinimumTimeInSeconds)
{
   checkNotInCallback();
   auto deadline = Clock::now() + toDuration(approximateMinimumTimeInSeconds);
   std::unique_lock<std::mutex> lock(_queueMutex);
   if (_callbackModel == HLA_EVOKED &&
       _queueCv.wait_until(lock, deadline, [this] { return deliverable(); }))
   {
      deliverOne(lock);
   }
   else if (_callbackModel == HLA_IMMEDIATE)
   {
      // Callbacks are delivered by the dispatcher thread: just honour the wait.
      _queueCv.wait_until(lock, deadline, [] { return false; });
   }
   return deliverable();
}

// ------------------------------------------------------------------ 10.42
bool MqttRTIambassador::evokeMultipleCallbacks(double approximateMinimumTimeInSeconds,
                                               double approximateMaximumTimeInSeconds)
{
   checkNotInCallback();
   auto start = Clock::now();
   auto minDeadline = start + toDuration(approximateMinimumTimeInSeconds);
   auto maxDeadline = start + toDuration(approximateMaximumTimeInSeconds);
   if (maxDeadline < minDeadline)
      maxDeadline = minDeadline;

   std::unique_lock<std::mutex> lock(_queueMutex);
   if (_callbackModel == HLA_IMMEDIATE)
   {
      _queueCv.wait_until(lock, minDeadline, [] { return false; });
      return false;
   }
   for (;;)
   {
      while (deliverable() && Clock::now() < maxDeadline)
         deliverOne(lock);
      if (Clock::now() >= minDeadline)
         break;
      _queueCv.wait_until(lock, minDeadline, [this] { return deliverable(); });
   }
   return deliverable();
}

// ------------------------------------------------------------------ 10.43
void MqttRTIambassador::enableCallbacks()
{
   {
      std::lock_guard<std::mutex> lock(_queueMutex);
      _callbacksEnabled = true;
   }
   _queueCv.notify_all();
}

// ------------------------------------------------------------------ 10.44
void MqttRTIambassador::disableCallbacks()
{
   std::lock_guard<std::mutex> lock(_queueMutex);
   _callbacksEnabled = false;
}
}
