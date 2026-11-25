// hla-tls-test: TLS connections (#39). Needs a TLS broker described by
// HLA_MQTT_TLS_DIR, a directory holding ca.pem (CA of the broker),
// other-ca.pem (an unrelated CA), client.pem and client.key (client
// certificate signed by ca.pem), with:
//   mqtts://localhost:8883   server certificate for localhost, no client certificate
//   mqtts://localhost:8884   client certificate required
// (ports: HLA_MQTT_TLS_PORT, HLA_MQTT_TLS_CLIENT_PORT).
//
// Without TLS support in the library (HLA_MQTT_WITH_SSL off): checks that a
// TLS broker is refused with ConnectionFailed. Without HLA_MQTT_TLS_DIR:
// skipped (exit code 77).
//
// Standard API only. Exit code 0 = all checks passed.

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS // std::getenv
#endif

#include <RTI/NullFederateAmbassador.h>
#include <RTI/RTI1516.h>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <string>

#ifndef HLA_TEST_FOM_DIR
#define HLA_TEST_FOM_DIR "."
#endif

using namespace rti1516e;

namespace
{
int failures = 0;
int checks = 0;

void check(bool ok, std::string const& what)
{
   ++checks;
   if (!ok)
      ++failures;
   std::cout << (ok ? "[PASS] " : "[FAIL] ") << what << std::endl;
}

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

std::string env(char const* name, std::string const& fallback = std::string())
{
   char const* v = std::getenv(name);
   return v && *v ? std::string(v) : fallback;
}

struct Federate : NullFederateAmbassador
{
};

// "" when connected (then disconnected), else the ConnectionFailed message.
std::string tryConnect(std::wstring const& settings)
{
   Federate fed;
   std::unique_ptr<RTIambassador> rti = RTIambassadorFactory().createRTIambassador();
   try
   {
      rti->connect(fed, HLA_EVOKED, settings);
   }
   catch (ConnectionFailed const& e)
   {
      std::string const what = narrow(e.what());
      return what.empty() ? std::string("ConnectionFailed") : what;
   }
   catch (Exception const& e)
   {
      return "other exception: " + narrow(e.what());
   }
   rti->disconnect();
   return std::string();
}
} // namespace

int main()
{
#if !HLA_MQTT_WITH_SSL
   {
      std::string const err = tryConnect(L"mqtts://localhost:8883;connectTimeout=2;reconnect=0");
      check(err.find("without TLS support") != std::string::npos,
            "library without TLS: a TLS broker is refused (" + err + ")");
      std::cout << (checks - failures) << "/" << checks << " checks passed" << std::endl;
      return failures == 0 ? 0 : 1;
   }
#endif
   std::string const dir = env("HLA_MQTT_TLS_DIR");
   if (dir.empty())
   {
      std::cout << "HLA_MQTT_TLS_DIR not set: skipped" << std::endl;
      return 77;
   }
   std::wstring const d = widen(dir) + L"/";
   std::wstring const port = widen(env("HLA_MQTT_TLS_PORT", "8883"));
   std::wstring const clientPort = widen(env("HLA_MQTT_TLS_CLIENT_PORT", "8884"));
   std::wstring const common = L";connectTimeout=3;reconnect=0";
   std::wstring const server = L"mqtts://localhost:" + port + common;
   std::wstring const mutual = L"mqtts://localhost:" + clientPort + common;

   std::string err = tryConnect(server + L";caFile=" + d + L"ca.pem");
   check(err.empty(), "server verified with its CA (" + err + ")");
   err = tryConnect(server + L";caFile=" + d + L"other-ca.pem");
   check(!err.empty(), "server certificate from an unknown CA -> ConnectionFailed");
   err = tryConnect(server + L";caFile=" + d + L"other-ca.pem;verifyServer=false");
   check(err.empty(), "verifyServer=false accepts it (" + err + ")");
   err = tryConnect(L"mqtts://127.0.0.1:" + port + common + L";caFile=" + d + L"ca.pem");
   check(err.empty(), "IP address in the certificate (" + err + ")");

   err = tryConnect(mutual + L";caFile=" + d + L"ca.pem");
   check(!err.empty(), "client certificate required, none given -> ConnectionFailed");
   err = tryConnect(mutual + L";caFile=" + d + L"ca.pem;certFile=" + d + L"client.pem;keyFile=" + d + L"client.key");
   check(err.empty(), "client certificate accepted (" + err + ")");

   // A federation over TLS with a client certificate.
   {
      std::random_device rd;
      std::wstring const fedName = widen("tlstest-" + std::to_string(rd() % 1000000));
      Federate fed;
      std::unique_ptr<RTIambassador> rti = RTIambassadorFactory().createRTIambassador();
      try
      {
         rti->connect(fed, HLA_EVOKED,
                      mutual + L";caFile=" + d + L"ca.pem;certFile=" + d + L"client.pem;keyFile=" + d + L"client.key");
         rti->createFederationExecution(fedName, widen(std::string(HLA_TEST_FOM_DIR) + "/base.xml"));
         rti->joinFederationExecution(L"tls", L"t", fedName);
         ObjectClassHandle const vehicle = rti->getObjectClassHandle(L"Vehicle");
         rti->publishObjectClassAttributes(vehicle, AttributeHandleSet{rti->getAttributeHandle(vehicle, L"Position")});
         rti->registerObjectInstance(vehicle);
         rti->resignFederationExecution(CANCEL_THEN_DELETE_THEN_DIVEST);
         rti->destroyFederationExecution(fedName);
         rti->disconnect();
         check(true, "create, join, register, resign and destroy over TLS");
      }
      catch (Exception const& e)
      {
         check(false, "federation over TLS: " + narrow(e.what()));
      }
   }

   std::cout << (checks - failures) << "/" << checks << " checks passed" << std::endl;
   return failures == 0 ? 0 : 1;
}
