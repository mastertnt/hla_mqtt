// hla-mom-test: Management Object Model (IEEE 1516.1-2010, chapter 11)
// between ambassadors in one process: standard MIM in the FDD, HLAfederate
// and HLAfederation objects, HLArequest / HLAreport, HLAadjust and HLAservice
// interactions, HLAreportFederateLost. Needs a broker.
//
//   hla-mom-test [--broker mqtt://host:1883]
//
// Standard API only. Exit code 0 = all checks passed.

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS // std::getenv
#endif

#include <RTI/NullFederateAmbassador.h>
#include <RTI/RTI1516.h>
#include <RTI/encoding/BasicDataElements.h>
#include <RTI/encoding/HLAfixedRecord.h>
#include <RTI/encoding/HLAopaqueData.h>
#include <RTI/encoding/HLAvariableArray.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <vector>

#ifndef HLA_TEST_FOM_DIR
#define HLA_TEST_FOM_DIR "."
#endif

using namespace rti1516e;

namespace
{
int failures = 0;
int checks = 0;
std::wstring broker = L"mqtt://localhost:1883";

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

VariableLengthData text(std::string const& s)
{
   return VariableLengthData(s.data(), s.size());
}

// ------------------------------------------------------------ MIM encodings
std::wstring decString(VariableLengthData const& d)
{
   HLAunicodeString s;
   s.decode(d);
   return s.get();
}

long decInt(VariableLengthData const& d)
{
   HLAinteger32BE i;
   i.decode(d);
   return i.get();
}

VariableLengthData encInt(long v)
{
   return HLAinteger32BE(Integer32(v)).encode();
}

VariableLengthData encString(std::wstring const& s)
{
   return HLAunicodeString(s).encode();
}

template <class H>
VariableLengthData encHandle(H const& h)
{
   VariableLengthData const e = h.encode();
   return HLAopaqueData(static_cast<Octet const*>(e.data()), e.size()).encode();
}

VariableLengthData handleBytes(VariableLengthData const& d) // HLAhandle -> encoded handle
{
   HLAopaqueData o;
   o.decode(d);
   return VariableLengthData(o.get(), o.dataLength());
}

template <class H>
VariableLengthData encHandleList(std::vector<H> const& handles)
{
   HLAvariableArray a{HLAopaqueData()};
   for (H const& h : handles)
   {
      VariableLengthData const e = h.encode();
      a.addElement(HLAopaqueData(static_cast<Octet const*>(e.data()), e.size()));
   }
   return a.encode();
}

std::vector<VariableLengthData> decHandleList(VariableLengthData const& d)
{
   HLAvariableArray a{HLAopaqueData()};
   a.decode(d);
   std::vector<VariableLengthData> out;
   for (size_t i = 0; i < a.size(); ++i)
      out.push_back(handleBytes(a.get(i).encode()));
   return out;
}

std::vector<std::wstring> decStringList(VariableLengthData const& d)
{
   HLAvariableArray a{HLAunicodeString()};
   a.decode(d);
   std::vector<std::wstring> out;
   for (size_t i = 0; i < a.size(); ++i)
      out.push_back(static_cast<HLAunicodeString const&>(a.get(i)).get());
   return out;
}

// HLAobjectClassBasedCounts / HLAinteractionCounts: (encoded handle, count)
std::vector<std::pair<VariableLengthData, long>> decCounts(VariableLengthData const& d)
{
   HLAfixedRecord prototype;
   prototype.appendElement(HLAopaqueData());
   prototype.appendElement(HLAinteger32BE());
   HLAvariableArray a{prototype};
   a.decode(d);
   std::vector<std::pair<VariableLengthData, long>> out;
   for (size_t i = 0; i < a.size(); ++i)
   {
      HLAfixedRecord const& r = static_cast<HLAfixedRecord const&>(a.get(i));
      out.emplace_back(handleBytes(r.get(0).encode()), static_cast<HLAinteger32BE const&>(r.get(1)).get());
   }
   return out;
}

bool sameBytes(VariableLengthData const& a, VariableLengthData const& b)
{
   return a.size() == b.size() && (a.size() == 0 || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

struct Received
{
   InteractionClassHandle cls;
   ParameterHandleValueMap values;
   SupplementalReceiveInfo info;
   std::string tag;
};

struct Federate : NullFederateAmbassador
{
   std::unique_ptr<RTIambassador> rti;
   std::map<ObjectInstanceHandle, ObjectClassHandle> discovered;
   std::map<ObjectInstanceHandle, AttributeHandleValueMap> values; // last values per instance
   std::map<ObjectInstanceHandle, int> reflections;
   std::vector<Received> received;
   bool throwOnBoom = false;

   explicit Federate(std::string const& clientId)
   {
      rti = RTIambassadorFactory().createRTIambassador();
      rti->connect(*this, HLA_EVOKED, broker + L";clientId=" + widen(clientId));
   }

   bool pump(std::function<bool()> done, double seconds = 3.0)
   {
      auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
      while (!done())
      {
         if (std::chrono::steady_clock::now() > deadline)
            return false;
         rti->evokeMultipleCallbacks(0.01, 0.1);
      }
      return true;
   }
   void settle(double seconds = 0.4) { rti->evokeMultipleCallbacks(seconds, seconds + 0.2); }

   // Last interaction of class `c` received since index `from`.
   Received const* last(InteractionClassHandle c, size_t from = 0) const
   {
      for (size_t i = received.size(); i > from; --i)
         if (received[i - 1].cls == c)
            return &received[i - 1];
      return nullptr;
   }
   size_t count(InteractionClassHandle c, size_t from = 0) const
   {
      size_t n = 0;
      for (size_t i = from; i < received.size(); ++i)
         n += received[i].cls == c;
      return n;
   }

   void discoverObjectInstance(ObjectInstanceHandle o, ObjectClassHandle c, std::wstring const&)
      RTI_THROW((FederateInternalError)) override
   {
      discovered[o] = c;
   }
   void removeObjectInstance(ObjectInstanceHandle o, VariableLengthData const&, OrderType, SupplementalRemoveInfo)
      RTI_THROW((FederateInternalError)) override
   {
      discovered.erase(o);
   }
   void reflectAttributeValues(ObjectInstanceHandle o, AttributeHandleValueMap const& v, VariableLengthData const&,
                               OrderType, TransportationType, SupplementalReflectInfo)
      RTI_THROW((FederateInternalError)) override
   {
      ++reflections[o];
      for (auto const& kv : v)
         values[o][kv.first] = kv.second;
   }
   void receiveInteraction(InteractionClassHandle c, ParameterHandleValueMap const& v, VariableLengthData const& tag,
                           OrderType, TransportationType, SupplementalReceiveInfo info)
      RTI_THROW((FederateInternalError)) override
   {
      std::string const t = tag.size() ? std::string(static_cast<char const*>(tag.data()), tag.size()) : std::string();
      received.push_back(Received{c, v, info, t});
      if (throwOnBoom && t == "boom")
         throw FederateInternalError(L"boom");
   }
};
} // namespace

int main(int argc, char** argv)
{
   for (int i = 1; i < argc; ++i)
      if (std::string(argv[i]) == "--broker" && i + 1 < argc)
         broker = widen(argv[++i]);
   if (char const* env = std::getenv("HLA_MQTT_BROKER"))
      broker = widen(env);

   std::random_device rd;
   std::string const run = std::to_string(rd() % 1000000);
   std::wstring const fed = widen("momtest-" + run);
   std::cout << "federation: " << narrow(fed) << std::endl;

   try
   {
      Federate a("momtest-a-" + run), b("momtest-b-" + run);
      a.rti->createFederationExecution(fed, widen(std::string(HLA_TEST_FOM_DIR) + "/base.xml"));
      FederateHandle const ha = a.rti->joinFederationExecution(L"a", L"manager", fed);
      FederateHandle const hb = b.rti->joinFederationExecution(
         L"b", L"worker", fed, std::vector<std::wstring>{widen(std::string(HLA_TEST_FOM_DIR) + "/extra.xml")});
      RTIambassador& ra = *a.rti;

      // ------------------------------------------------ #34 standard MIM
      ObjectClassHandle federateClass, federationClass;
      try
      {
         federateClass = ra.getObjectClassHandle(L"HLAmanager.HLAfederate");
         federationClass = ra.getObjectClassHandle(L"HLAobjectRoot.HLAmanager.HLAfederation");
         check(true, "standard MIM in the FDD: HLAmanager.HLAfederate and HLAmanager.HLAfederation");
      }
      catch (Exception const& e)
      {
         check(false, "standard MIM in the FDD -- " + narrow(e.what()));
         return 1;
      }
      DimensionHandle const fedDim = ra.getDimensionHandle(L"HLAfederate");
      check(ra.getDimensionUpperBound(fedDim) == 0xFFFFFFFFul, "dimension HLAfederate without upper bound -> 2^32-1");
      check(ra.normalizeFederateHandle(hb) < 0xFFFFFFFFul, "normalizeFederateHandle below the HLAfederate bound");
      check(ra.getDimensionUpperBound(ra.getDimensionHandle(L"HLAserviceGroup")) == 7, "dimension HLAserviceGroup [0, 7)");

      auto attr = [&](ObjectClassHandle c, wchar_t const* n) { return ra.getAttributeHandle(c, n); };
      AttributeHandle const fName = attr(federateClass, L"HLAfederateName");
      AttributeHandle const fHandle = attr(federateClass, L"HLAfederateHandle");
      AttributeHandle const fType = attr(federateClass, L"HLAfederateType");
      AttributeHandle const fUpdates = attr(federateClass, L"HLAupdatesSent");
      AttributeHandle const fModules = attr(federateClass, L"HLAFOMmoduleDesignatorList");
      AttributeHandle const fRegulating = attr(federateClass, L"HLAtimeRegulating");
      AttributeHandle const nName = attr(federationClass, L"HLAfederationName");
      AttributeHandle const nFederates = attr(federationClass, L"HLAfederatesInFederation");
      AttributeHandle const nMim = attr(federationClass, L"HLAMIMdesignator");
      AttributeHandle const nModules = attr(federationClass, L"HLAFOMmoduleDesignatorList");
      AttributeHandle const nTime = attr(federationClass, L"HLAtimeImplementationName");

      // ---------------------------------- #35 HLAfederate / HLAfederation
      ra.subscribeObjectClassAttributes(federateClass,
                                        AttributeHandleSet{fName, fHandle, fType, fUpdates, fModules, fRegulating});
      ra.subscribeObjectClassAttributes(federationClass, AttributeHandleSet{nName, nFederates, nMim, nModules, nTime});
      auto instancesOf = [&](ObjectClassHandle c) {
         std::vector<ObjectInstanceHandle> out;
         for (auto const& kv : a.discovered)
            if (kv.second == c)
               out.push_back(kv.first);
         return out;
      };
      check(a.pump([&] { return instancesOf(federateClass).size() == 2 && instancesOf(federationClass).size() == 1; }),
            "A discovers two HLAfederate objects (A and B) and one HLAfederation object");

      ra.requestAttributeValueUpdate(federateClass, AttributeHandleSet{fName, fHandle, fType, fModules}, text(""));
      std::map<std::wstring, ObjectInstanceHandle> byName;
      check(a.pump([&] {
         byName.clear();
         for (ObjectInstanceHandle const& o : instancesOf(federateClass))
            if (a.values[o].count(fName))
               byName[decString(a.values[o][fName])] = o;
         return byName.size() == 2;
      }),
            "class request: both HLAfederate objects reflect HLAfederateName");
      ObjectInstanceHandle const objB = byName[L"b"];
      check(byName.count(L"a") && byName.count(L"b"), "HLAfederateName values a and b");
      check(byName.count(L"b") && sameBytes(handleBytes(a.values[objB][fHandle]), hb.encode()),
            "HLAfederateHandle of b = the handle returned by its join");
      check(byName.count(L"b") && decString(a.values[objB][fType]) == L"worker", "HLAfederateType of b = worker");
      std::vector<std::wstring> const bModules = decStringList(a.values[objB][fModules]);
      check(bModules.size() == 1 && bModules[0].find(L"extra.xml") != std::wstring::npos,
            "HLAFOMmoduleDesignatorList of b = the module given at join");

      ObjectClassHandle const vehicle = b.rti->getObjectClassHandle(L"Vehicle");
      AttributeHandle const position = b.rti->getAttributeHandle(vehicle, L"Position");
      b.rti->publishObjectClassAttributes(vehicle, AttributeHandleSet{position});
      ObjectInstanceHandle const car = b.rti->registerObjectInstance(vehicle);
      for (int i = 0; i < 3; ++i)
      {
         AttributeHandleValueMap v;
         v[position] = text("p");
         b.rti->updateAttributeValues(car, v, text(""));
      }
      int const before = a.reflections[objB];
      ra.requestAttributeValueUpdate(objB, AttributeHandleSet{fUpdates}, text(""));
      check(a.pump([&] { return a.reflections[objB] > before && a.values[objB].count(fUpdates); }) &&
               decInt(a.values[objB][fUpdates]) == 3,
            "instance request: HLAupdatesSent of b = 3");

      ObjectInstanceHandle const federation = instancesOf(federationClass).front();
      ra.requestAttributeValueUpdate(federation, AttributeHandleSet{nName, nFederates, nMim, nModules, nTime}, text(""));
      check(a.pump([&] { return a.values[federation].count(nFederates) != 0; }), "HLAfederation reflected on request");
      check(decString(a.values[federation][nName]) == fed, "HLAfederationName");
      check(decHandleList(a.values[federation][nFederates]).size() == 2, "HLAfederatesInFederation: 2 federates");
      check(decString(a.values[federation][nMim]) == L"HLAstandardMIM", "HLAMIMdesignator = HLAstandardMIM");
      std::vector<std::wstring> const modules = decStringList(a.values[federation][nModules]);
      std::string list;
      for (std::wstring const& m : modules)
         list += " " + narrow(m);
      check(modules.size() == 2 && modules[0].find(L"base.xml") != std::wstring::npos &&
               modules[1].find(L"extra.xml") != std::wstring::npos,
            "HLAFOMmoduleDesignatorList: base.xml (creation) and extra.xml (join of b) --" + list);
      check(decString(a.values[federation][nTime]) == L"HLAfloat64Time", "HLAtimeImplementationName = HLAfloat64Time");
      {
         bool refused = false;
         try
         {
            AttributeHandleValueMap v;
            v[fName] = encString(L"x");
            ra.updateAttributeValues(byName[L"a"], v, text(""));
         }
         catch (AttributeNotOwned const&)
         {
            refused = true;
         }
         check(refused, "the federate cannot update its own HLAfederate object (owned by the RTI)");
      }

      // ------------------------------------ #36 HLArequest / HLAreport
      auto ic = [&](wchar_t const* n) { return ra.getInteractionClassHandle(n); };
      auto pr = [&](InteractionClassHandle c, wchar_t const* n) { return ra.getParameterHandle(c, n); };
      InteractionClassHandle const reqPub = ic(L"HLAmanager.HLAfederate.HLArequest.HLArequestPublications");
      InteractionClassHandle const reqUpd = ic(L"HLAmanager.HLAfederate.HLArequest.HLArequestUpdatesSent");
      InteractionClassHandle const reqInfo = ic(L"HLAmanager.HLAfederate.HLArequest.HLArequestObjectInstanceInformation");
      InteractionClassHandle const reqFom = ic(L"HLAmanager.HLAfederate.HLArequest.HLArequestFOMmoduleData");
      InteractionClassHandle const repPub = ic(L"HLAmanager.HLAfederate.HLAreport.HLAreportObjectClassPublication");
      InteractionClassHandle const repIPub = ic(L"HLAmanager.HLAfederate.HLAreport.HLAreportInteractionPublication");
      InteractionClassHandle const repUpd = ic(L"HLAmanager.HLAfederate.HLAreport.HLAreportUpdatesSent");
      InteractionClassHandle const repInfo = ic(L"HLAmanager.HLAfederate.HLAreport.HLAreportObjectInstanceInformation");
      InteractionClassHandle const repFom = ic(L"HLAmanager.HLAfederate.HLAreport.HLAreportFOMmoduleData");
      InteractionClassHandle const repMomExc = ic(L"HLAmanager.HLAfederate.HLAreport.HLAreportMOMexception");
      InteractionClassHandle const repExc = ic(L"HLAmanager.HLAfederate.HLAreport.HLAreportException");
      InteractionClassHandle const repSvc = ic(L"HLAmanager.HLAfederate.HLAreport.HLAreportServiceInvocation");
      InteractionClassHandle const repLost = ic(L"HLAmanager.HLAfederate.HLAreport.HLAreportFederateLost");
      InteractionClassHandle const fReq = ic(L"HLAmanager.HLAfederation.HLArequest.HLArequestSynchronizationPoints");
      InteractionClassHandle const fReqMim = ic(L"HLAmanager.HLAfederation.HLArequest.HLArequestMIMdata");
      InteractionClassHandle const fRep = ic(L"HLAmanager.HLAfederation.HLAreport.HLAreportSynchronizationPoints");
      InteractionClassHandle const fRepMim = ic(L"HLAmanager.HLAfederation.HLAreport.HLAreportMIMdata");
      ParameterHandle const pFederate = pr(reqPub, L"HLAfederate");
      for (InteractionClassHandle c : {reqPub, reqUpd, reqInfo, reqFom, fReq, fReqMim})
         ra.publishInteractionClass(c);
      for (InteractionClassHandle c : {repPub, repIPub, repUpd, repInfo, repFom, repMomExc, repExc, repSvc, repLost, fRep, fRepMim})
         ra.subscribeInteractionClass(c, true);
      auto toB = [&](ParameterHandleValueMap p = ParameterHandleValueMap()) {
         p[pFederate] = encHandle(hb);
         return p;
      };
      auto pump = [&](std::function<bool()> done, double s = 3.0) {
         auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(s);
         while (!done())
         {
            if (std::chrono::steady_clock::now() > deadline)
               return false;
            a.rti->evokeMultipleCallbacks(0.01, 0.05);
            b.rti->evokeMultipleCallbacks(0.01, 0.05);
         }
         return true;
      };

      size_t mark = a.received.size();
      ra.sendInteraction(reqPub, toB(), text(""));
      check(pump([&] { return a.last(repPub, mark) && a.last(repIPub, mark); }),
            "HLArequestPublications -> HLAreportObjectClassPublication and HLAreportInteractionPublication from b");
      if (Received const* r = a.last(repPub, mark))
      {
         check(sameBytes(handleBytes(r->values.at(pr(repPub, L"HLAobjectClass"))), vehicle.encode()) &&
                  decInt(r->values.at(pr(repPub, L"HLAnumberOfClasses"))) == 1,
               "publication report: 1 class, Vehicle");
         std::vector<VariableLengthData> const attrs = decHandleList(r->values.at(pr(repPub, L"HLAattributeList")));
         check(attrs.size() == 1 && sameBytes(attrs[0], position.encode()), "publication report: attribute Position");
         check(sameBytes(handleBytes(r->values.at(pFederate)), hb.encode()), "report names b in HLAfederate");
      }

      mark = a.received.size();
      ra.sendInteraction(reqUpd, toB(), text(""));
      check(pump([&] { return a.count(repUpd, mark) == 2; }), "HLArequestUpdatesSent -> two reports (reliable, best effort)");
      long total = 0;
      for (size_t i = mark; i < a.received.size(); ++i)
         if (a.received[i].cls == repUpd)
            for (auto const& c : decCounts(a.received[i].values.at(pr(repUpd, L"HLAupdateCounts"))))
               if (sameBytes(c.first, vehicle.encode()))
                  total += c.second;
      check(total == 3, "HLAupdateCounts: 3 updates of Vehicle");

      mark = a.received.size();
      ParameterHandleValueMap info = toB();
      info[pr(reqInfo, L"HLAobjectInstance")] = encHandle(car);
      ra.sendInteraction(reqInfo, info, text(""));
      check(pump([&] { return a.last(repInfo, mark) != nullptr; }),
            "HLArequestObjectInstanceInformation -> HLAreportObjectInstanceInformation");
      if (Received const* r = a.last(repInfo, mark))
         check(sameBytes(handleBytes(r->values.at(pr(repInfo, L"HLAregisteredClass"))), vehicle.encode()) &&
                  decHandleList(r->values.at(pr(repInfo, L"HLAownedInstanceAttributeList"))).size() == 2,
               "instance report: registered class Vehicle, Position and HLAprivilegeToDeleteObject owned");

      mark = a.received.size();
      info[pr(reqInfo, L"HLAobjectInstance")] = encHandle(b.rti->decodeObjectInstanceHandle(text("12345678")));
      ra.sendInteraction(reqInfo, info, text(""));
      check(pump([&] { return a.last(repMomExc, mark) != nullptr; }),
            "unknown object instance -> HLAreportMOMexception");
      if (Received const* r = a.last(repMomExc, mark))
         check(decInt(r->values.at(pr(repMomExc, L"HLAparameterError"))) == 1, "HLAparameterError = true");

      mark = a.received.size();
      ParameterHandleValueMap fom = toB();
      fom[pr(reqFom, L"HLAFOMmoduleIndicator")] = encInt(0);
      ra.sendInteraction(reqFom, fom, text(""));
      check(pump([&] { return a.last(repFom, mark) != nullptr; }) &&
               decString(a.last(repFom, mark)->values.at(pr(repFom, L"HLAFOMmoduleData"))).find(L"objectModel") !=
                  std::wstring::npos,
            "HLArequestFOMmoduleData 0 -> content of extra.xml");

      ra.registerFederationSynchronizationPoint(L"momPoint", text(""));
      a.settle(0.3);
      mark = a.received.size();
      ra.sendInteraction(fReq, ParameterHandleValueMap(), text(""));
      check(pump([&] { return a.count(fRep, mark) >= 1; }), "HLArequestSynchronizationPoints -> one report (elected federate)");
      a.settle(0.3);
      check(a.count(fRep, mark) == 1, "federation-level request answered once");
      if (Received const* r = a.last(fRep, mark))
      {
         std::vector<std::wstring> const points = decStringList(r->values.at(pr(fRep, L"HLAsyncPoints")));
         check(points.size() == 1 && points[0] == L"momPoint", "HLAsyncPoints = {momPoint}");
      }
      mark = a.received.size();
      ra.sendInteraction(fReqMim, ParameterHandleValueMap(), text(""));
      check(pump([&] { return a.last(fRepMim, mark) != nullptr; }) &&
               decString(a.last(fRepMim, mark)->values.at(pr(fRepMim, L"HLAMIMdata"))).find(L"Standard MOM") !=
                  std::wstring::npos,
            "HLArequestMIMdata -> the standard MIM");

      // ------------------------------------ #37 HLAadjust / HLAservice
      InteractionClassHandle const setTiming = ic(L"HLAmanager.HLAfederate.HLAadjust.HLAsetTiming");
      InteractionClassHandle const setSwitches = ic(L"HLAmanager.HLAfederate.HLAadjust.HLAsetSwitches");
      InteractionClassHandle const setSvc = ic(L"HLAmanager.HLAfederate.HLAadjust.HLAsetServiceReporting");
      InteractionClassHandle const setExc = ic(L"HLAmanager.HLAfederate.HLAadjust.HLAsetExceptionReporting");
      InteractionClassHandle const svcSub = ic(L"HLAmanager.HLAfederate.HLAservice.HLAsubscribeInteractionClass");
      InteractionClassHandle const svcUnsub = ic(L"HLAmanager.HLAfederate.HLAservice.HLAunsubscribeInteractionClass");
      InteractionClassHandle const svcReg = ic(L"HLAmanager.HLAfederate.HLAservice.HLAenableTimeRegulation");
      for (InteractionClassHandle c : {setTiming, setSwitches, setSvc, setExc, svcSub, svcUnsub, svcReg})
         ra.publishInteractionClass(c);

      ParameterHandleValueMap timing = toB();
      timing[pr(setTiming, L"HLAreportPeriod")] = encInt(1);
      ra.sendInteraction(setTiming, timing, text(""));
      int const periodic = a.reflections[objB];
      check(a.pump([&] { return a.reflections[objB] >= periodic + 2; }, 4.0),
            "HLAsetTiming 1 s -> periodic updates of b's HLAfederate without request");
      timing[pr(setTiming, L"HLAreportPeriod")] = encInt(0);
      ra.sendInteraction(setTiming, timing, text(""));
      a.settle(0.3);
      int const stopped = a.reflections[objB];
      a.settle(1.5);
      check(a.reflections[objB] == stopped, "HLAsetTiming 0 -> periodic updates stop");

      InteractionClassHandle const message = ra.getInteractionClassHandle(L"Message");
      ra.publishInteractionClass(message);
      ParameterHandleValueMap svcReport = toB();
      svcReport[pr(setSvc, L"HLAreportingState")] = encInt(1);
      ra.sendInteraction(setSvc, svcReport, text(""));
      mark = a.received.size();
      ParameterHandleValueMap sub = toB();
      sub[pr(svcSub, L"HLAinteractionClass")] = encHandle(message);
      sub[pr(svcSub, L"HLAactive")] = encInt(1);
      ra.sendInteraction(svcSub, sub, text(""));
      check(pump([&] { return a.last(repSvc, mark) != nullptr; }),
            "HLAsubscribeInteractionClass service -> HLAreportServiceInvocation (reporting on)");
      if (Received const* r = a.last(repSvc, mark))
         check(decInt(r->values.at(pr(repSvc, L"HLAsuccessIndicator"))) == 1 &&
                  decString(r->values.at(pr(repSvc, L"HLAservice"))) == L"subscribeInteractionClass",
               "service invocation report: subscribeInteractionClass, success");
      size_t const bMark = b.received.size();
      ra.sendInteraction(message, ParameterHandleValueMap(), text("m1"));
      check(pump([&] { return b.last(message, bMark) != nullptr; }), "b receives Message after the MOM subscription");
      if (Received const* r = b.last(message, bMark))
         check(!r->info.hasProducingFederate, "no producing federate while conveyProducingFederate is disabled");

      ParameterHandleValueMap sw = toB();
      sw[pr(setSwitches, L"HLAconveyProducingFederate")] = encInt(1);
      ra.sendInteraction(setSwitches, sw, text(""));
      a.settle(0.2);
      size_t const bMark2 = b.received.size();
      ra.sendInteraction(message, ParameterHandleValueMap(), text("m2"));
      check(pump([&] { return b.last(message, bMark2) != nullptr; }) && b.last(message, bMark2)->info.hasProducingFederate &&
               b.last(message, bMark2)->info.producingFederate == ha,
            "HLAsetSwitches conveyProducingFederate -> producing federate = a");

      mark = a.received.size();
      ParameterHandleValueMap bad = toB();
      bad[pr(svcUnsub, L"HLAinteractionClass")] = encHandle(federateClass); // an object class handle
      ra.sendInteraction(svcUnsub, bad, text(""));
      check(pump([&] { return a.last(repMomExc, mark) != nullptr; }),
            "service failing in b -> HLAreportMOMexception");
      if (Received const* r = a.last(repMomExc, mark))
         check(decInt(r->values.at(pr(repMomExc, L"HLAparameterError"))) == 0, "HLAparameterError = false");
      check(a.last(repSvc, mark) && decInt(a.last(repSvc, mark)->values.at(pr(repSvc, L"HLAsuccessIndicator"))) == 0,
            "service invocation report: failure");

      mark = a.received.size();
      ParameterHandleValueMap reg = toB();
      reg[pr(svcReg, L"HLAlookahead")] = HLAopaqueData().encode();
      ra.sendInteraction(svcReg, reg, text(""));
      check(pump([&] { return a.last(repMomExc, mark) != nullptr; }), "undecodable lookahead -> HLAreportMOMexception");

      ParameterHandleValueMap exc = toB();
      exc[pr(setExc, L"HLAreportingState")] = encInt(1);
      ra.sendInteraction(setExc, exc, text(""));
      a.settle(0.2);
      b.throwOnBoom = true;
      mark = a.received.size();
      ra.sendInteraction(message, ParameterHandleValueMap(), text("boom"));
      check(pump([&] { return a.last(repExc, mark) != nullptr; }),
            "exception thrown by b's callback -> HLAreportException (reporting on)");
      b.throwOnBoom = false;

      // ------------------------------------------- HLAreportFederateLost
      {
         Federate c("momtest-c-" + run);
         c.rti->joinFederationExecution(L"c", L"t", fed);
         a.settle(0.3);
         mark = a.received.size();
      } // destroyed without resign
      check(pump([&] { return a.last(repLost, mark) != nullptr; }, 5.0), "federate lost without resign -> HLAreportFederateLost");
      if (Received const* r = a.last(repLost, mark))
         check(decString(r->values.at(pr(repLost, L"HLAfederateName"))) == L"c", "lost federate name = c");

      // ------------------------------------------------------------ resign
      ObjectInstanceHandle const objA = byName[L"a"];
      b.rti->resignFederationExecution(DELETE_OBJECTS);
      check(a.pump([&] { return instancesOf(federateClass).size() == 1; }), "b's HLAfederate removed when b resigns");
      check(instancesOf(federateClass).size() == 1 && instancesOf(federateClass)[0] == objA, "a's own HLAfederate stays");
      ra.resignFederationExecution(DELETE_OBJECTS);
      ra.destroyFederationExecution(fed);

      // ------------------------------------------ explicit MIM: no default
      std::wstring const fed2 = fed + L"-mim";
      ra.createFederationExecutionWithMIM(fed2, {widen(std::string(HLA_TEST_FOM_DIR) + "/base.xml")},
                                          widen(std::string(HLA_TEST_FOM_DIR) + "/extra.xml"));
      ra.joinFederationExecution(L"a", L"t", fed2);
      bool absent = false;
      try
      {
         ra.getObjectClassHandle(L"HLAmanager.HLAfederate");
      }
      catch (NameNotFound const&)
      {
         absent = true;
      }
      check(absent, "createFederationExecutionWithMIM: the given MIM replaces the standard one");
      ra.resignFederationExecution(NO_ACTION);
      ra.destroyFederationExecution(fed2);
   }
   catch (Exception const& e)
   {
      check(false, "unexpected exception: " + narrow(e.what()));
   }

   std::cout << (checks - failures) << "/" << checks << " checks passed" << std::endl;
   return failures == 0 ? 0 : 1;
}
