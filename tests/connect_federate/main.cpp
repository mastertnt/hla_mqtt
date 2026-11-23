// hla-test-federate: test client for the jalon 1 services (connect/disconnect).
//
// Uses ONLY the standard IEEE 1516e API (RTI/RTI1516.h): the same binary can
// be run against any 1516e RTI DLL, which is how drop-in compatibility is
// checked.
//
// Usage:
//   hla-test-federate --selftest [--settings "<designator>"]
//   hla-test-federate [--settings "<designator>"] [--model evoked|immediate]
//                     [--wait <seconds>]
//
//   --selftest  run the automatic checks, exit code 0 = all passed
//   --settings  localSettingsDesignator passed to connect()
//               e.g. "mqtt://localhost:1883;clientId=fed1"
//   --model     callback model (default: evoked)
//   --wait      stay connected N seconds while pumping callbacks
//               (stop the broker meanwhile to see connectionLost)

#include <RTI/RTI1516.h>
#include <RTI/NullFederateAmbassador.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace rti1516e;

namespace
{
class TestFederateAmbassador : public NullFederateAmbassador
{
public:
   void connectionLost(std::wstring const& faultDescription) override
   {
      std::wcout << L"  [callback] connectionLost: " << faultDescription << std::endl;
      lost = true;
   }
   std::atomic<bool> lost{false};
};

int g_failures = 0;

void report(bool ok, std::wstring const& name, std::wstring const& detail = L"")
{
   std::wcout << (ok ? L"[PASS] " : L"[FAIL] ") << name;
   if (!detail.empty())
      std::wcout << L" -- " << detail;
   std::wcout << std::endl;
   if (!ok)
      ++g_failures;
}

std::wstring widen(char const* s)
{
   std::string n(s);
   return std::wstring(n.begin(), n.end());
}

// Runs `f`, expects exception type E.
template <class E, class F>
void expectThrow(std::wstring const& name, F f)
{
   try
   {
      f();
      report(false, name, L"no exception");
   }
   catch (E const& e)
   {
      report(true, name, e.what());
   }
   catch (Exception const& e)
   {
      report(false, name, L"unexpected " + e.what());
   }
}

template <class F>
void expectOk(std::wstring const& name, F f)
{
   try
   {
      f();
      report(true, name);
   }
   catch (Exception const& e)
   {
      report(false, name, e.what());
   }
}

int selfTest(std::wstring const& settings)
{
   std::wcout << L"RTI: " << rtiName() << L" / " << rtiVersion() << std::endl;
   std::wcout << L"settings: \"" << settings << L"\"" << std::endl;

   RTI_UNIQUE_PTR<RTIambassador> rti;
   expectOk(L"createRTIambassador", [&] { rti = RTIambassadorFactory().createRTIambassador(); });
   if (!rti.get())
      return 1;

   TestFederateAmbassador fed;

   expectThrow<InvalidLocalSettingsDesignator>(L"connect with invalid designator",
      [&] { rti->connect(fed, HLA_EVOKED, L"thisKey=doesNotExist"); });

   expectThrow<ConnectionFailed>(L"connect to unreachable broker",
      [&] { rti->connect(fed, HLA_EVOKED, L"tcp://127.0.0.1:1;connectTimeout=1"); });

   expectOk(L"connect (HLA_EVOKED)", [&] { rti->connect(fed, HLA_EVOKED, settings); });

   expectThrow<AlreadyConnected>(L"second connect",
      [&] { rti->connect(fed, HLA_EVOKED, settings); });

   expectOk(L"evokeMultipleCallbacks(0.1, 0.2)",
      [&] { rti->evokeMultipleCallbacks(0.1, 0.2); });

   expectOk(L"disable/enableCallbacks", [&] { rti->disableCallbacks(); rti->enableCallbacks(); });

   // Federation management (jalon 2): a missing FOM file is reported as such.
   expectThrow<CouldNotOpenFDD>(L"createFederationExecution with a missing FOM",
      [&] { rti->createFederationExecution(L"TestFederation", L"no-such-fom.xml"); });
   // Services of later milestones -> RTIinternalError.
   expectThrow<RTIinternalError>(L"requestFederationSave (not implemented yet)",
      [&] { rti->requestFederationSave(L"save"); });

   expectOk(L"disconnect", [&] { rti->disconnect(); });
   expectOk(L"disconnect when already disconnected", [&] { rti->disconnect(); });

   expectOk(L"reconnect (HLA_IMMEDIATE)", [&] { rti->connect(fed, HLA_IMMEDIATE, settings); });
   expectOk(L"evokeCallback in immediate mode", [&] { rti->evokeCallback(0.05); });
   expectOk(L"disconnect (HLA_IMMEDIATE)", [&] { rti->disconnect(); });

   expectOk(L"destroy ambassador while connected",
      [&]
      {
         RTI_UNIQUE_PTR<RTIambassador> other = RTIambassadorFactory().createRTIambassador();
         other->connect(fed, HLA_EVOKED, settings);
      });

   std::wcout << (g_failures == 0 ? L"ALL TESTS PASSED" : L"SOME TESTS FAILED")
              << L" (" << g_failures << L" failure(s))" << std::endl;
   return g_failures == 0 ? 0 : 1;
}

int interactive(std::wstring const& settings, CallbackModel model, double waitSeconds)
{
   std::wcout << L"RTI: " << rtiName() << L" / " << rtiVersion() << std::endl;
   try
   {
      RTI_UNIQUE_PTR<RTIambassador> rti = RTIambassadorFactory().createRTIambassador();
      TestFederateAmbassador fed;
      rti->connect(fed, model, settings);
      std::wcout << L"connected (" << (model == HLA_EVOKED ? L"evoked" : L"immediate")
                 << L"), waiting " << waitSeconds << L" s..." << std::endl;

      auto end = std::chrono::steady_clock::now() +
                 std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(waitSeconds));
      while (std::chrono::steady_clock::now() < end && !fed.lost)
      {
         if (model == HLA_EVOKED)
            rti->evokeMultipleCallbacks(0.1, 0.5);
         else
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      rti->disconnect();
      std::wcout << (fed.lost ? L"connection lost, disconnected" : L"disconnected") << std::endl;
      return fed.lost ? 2 : 0;
   }
   catch (Exception const& e)
   {
      std::wcerr << L"error: " << e.what() << std::endl;
      return 1;
   }
}
}

int main(int argc, char** argv)
{
   std::wstring settings;
   CallbackModel model = HLA_EVOKED;
   double waitSeconds = 0.0;
   bool runSelfTest = false;

   for (int i = 1; i < argc; ++i)
   {
      std::string a = argv[i];
      if (a == "--selftest")
         runSelfTest = true;
      else if (a == "--settings" && i + 1 < argc)
         settings = widen(argv[++i]);
      else if (a == "--model" && i + 1 < argc)
      {
         std::string m = argv[++i];
         model = (m == "immediate") ? HLA_IMMEDIATE : HLA_EVOKED;
      }
      else if (a == "--wait" && i + 1 < argc)
         waitSeconds = std::atof(argv[++i]);
      else
      {
         std::cerr << "usage: " << argv[0]
                   << " [--selftest] [--settings <designator>] [--model evoked|immediate]"
                      " [--wait <seconds>]\n";
         return 64;
      }
   }
   return runSelfTest ? selfTest(settings) : interactive(settings, model, waitSeconds);
}
