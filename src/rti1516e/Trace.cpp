// Diagnostic mode (HLA_MQTT_TRACE): logs every rti1516e exception thrown by
// this DLL, with its type and message, even when the federate catches it.
// Meant for substitution tests with existing federates, whose sources are not
// available: the exception that makes a federate give up is often caught and
// rethrown as a vendor exception.
//
//   HLA_MQTT_TRACE=1           -> stderr
//   HLA_MQTT_TRACE=<file path> -> appended to that file
//
// Windows only (vectored exception handler on MSVC C++ exceptions).

#if defined(_WIN32)

#define _CRT_SECURE_NO_WARNINGS // getenv, fopen
#include <RTI/Exception.h>

#include "Utf.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace
{
HMODULE thisModule()
{
   HMODULE h = nullptr;
   GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(&thisModule), &h);
   return h;
}

std::string g_target; // "" = stderr
std::mutex g_mutex;

void writeLine(std::string const& line)
{
   std::lock_guard<std::mutex> lock(g_mutex);
   if (g_target.empty())
   {
      std::fprintf(stderr, "%s\n", line.c_str());
      std::fflush(stderr);
      return;
   }
   if (FILE* f = std::fopen(g_target.c_str(), "a"))
   {
      std::fprintf(f, "%s\n", line.c_str());
      std::fclose(f);
   }
}

// MSVC x64 C++ exception: params[1] = object, params[2] = ThrowInfo,
// params[3] = image base; ThrowInfo/CatchableType hold image-relative offsets.
std::string thrownTypeName(ULONG_PTR throwInfo, ULONG_PTR imageBase)
{
   auto rva = [imageBase](ULONG_PTR addr) { return *reinterpret_cast<int const*>(addr); };
   ULONG_PTR const cta = imageBase + rva(throwInfo + 12);
   ULONG_PTR const firstType = imageBase + rva(cta + 4);
   ULONG_PTR const typeDescriptor = imageBase + rva(firstType + 4);
   return reinterpret_cast<char const*>(typeDescriptor + 16); // decorated name, e.g. .?AVNameNotFound@rti1516e@@
}

LONG CALLBACK onException(PEXCEPTION_POINTERS info)
{
   EXCEPTION_RECORD const* r = info->ExceptionRecord;
   if (r->ExceptionCode != 0xE06D7363 || r->NumberParameters < 4 ||
       r->ExceptionInformation[3] != reinterpret_cast<ULONG_PTR>(thisModule()))
      return EXCEPTION_CONTINUE_SEARCH;
   try
   {
      std::string const type = thrownTypeName(r->ExceptionInformation[2], r->ExceptionInformation[3]);
      if (type.find("@rti1516e@@") == std::string::npos)
         return EXCEPTION_CONTINUE_SEARCH;
      // Every rti1516e exception derives (single inheritance) from rti1516e::Exception.
      auto const* e = reinterpret_cast<rti1516e::Exception const*>(r->ExceptionInformation[1]);
      std::string name = type.substr(4, type.find('@') - 4);
      writeLine("[hla-mqtt-rti] throw " + name + ": " + hla_mqtt::toUtf8(e->what()));
   }
   catch (...)
   {
   }
   return EXCEPTION_CONTINUE_SEARCH;
}

struct TraceInstaller
{
   TraceInstaller()
   {
      char const* v = std::getenv("HLA_MQTT_TRACE");
      if (v == nullptr || *v == '\0' || std::strcmp(v, "0") == 0)
         return;
      if (std::strcmp(v, "1") != 0)
         g_target = v;
      AddVectoredExceptionHandler(1, onException);
      writeLine("[hla-mqtt-rti] trace enabled");
   }
} g_traceInstaller;
} // namespace

#endif
