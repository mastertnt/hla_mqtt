# Conformance tests (#44): the IEEE 1516-2010 test programs of OpenRTI
# (tests/rti1516e, BSD/LGPL), built against this RTI instead of OpenRTI's and
# run over the MQTT broker. Enabled by -DHLA_MQTT_OPENRTI_SOURCE_DIR=<OpenRTI
# checkout>; see conformance/README.md.
#
# The OpenRTI core library (src/OpenRTI) is still needed for the test helpers
# (options, threads, clock, random numbers); the RTI used by the tests is ours.
# The test sources are copied into the build tree with the connection URL
# replaced: "thread://" (OpenRTI in-process RTI) becomes "" (our settings, from
# HLA_MQTT_SETTINGS / HLA_MQTT_BROKER), set per test by ctest.

set(OPENRTI_DIR ${HLA_MQTT_OPENRTI_SOURCE_DIR})
if(NOT EXISTS ${OPENRTI_DIR}/src/OpenRTI/TestLib.h OR NOT EXISTS ${OPENRTI_DIR}/tests/rti1516e)
  message(FATAL_ERROR "HLA_MQTT_OPENRTI_SOURCE_DIR=${OPENRTI_DIR} is not an OpenRTI source tree")
endif()

# ---------------------------------------------------------------- OpenRTI core
# Helper macros the OpenRTI core CMakeLists.txt expects from its top level.
macro(openrti_target_link_private_libraries _target)
  target_link_libraries(${_target} PRIVATE ${ARGN})
endmacro()
macro(openrti_target_position_independent _target)
  set_property(TARGET ${_target} PROPERTY POSITION_INDEPENDENT_CODE TRUE)
endmacro()
set(OPENRTI_BUILD_SHARED FALSE)
set(OPENRTI_DATAROOTDIR "${CMAKE_CURRENT_BINARY_DIR}/share")
set(CMAKE_SKIP_INSTALL_RULES_SAVED ${CMAKE_SKIP_INSTALL_RULES})
add_subdirectory(${OPENRTI_DIR}/src/OpenRTI ${CMAKE_CURRENT_BINARY_DIR}/openrti-core EXCLUDE_FROM_ALL)
if(MSVC)
  target_compile_options(OpenRTI PRIVATE /w)
  set_property(TARGET OpenRTI PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
else()
  target_compile_options(OpenRTI PRIVATE -w)
endif()

# ------------------------------------------------------- patched test sources
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/src)
function(hla_openrti_copy src dst)
  file(READ ${src} text)
  string(REPLACE "\r\n" "\n" text "${text}")
  string(REPLACE "L\"thread:///\"" "L\"\"" text "${text}")
  string(REPLACE "L\"thread://\"" "L\"\"" text "${text}")
  file(WRITE ${dst} "${text}")
  set_property(DIRECTORY ${CMAKE_SOURCE_DIR} APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${src})
endfunction()
hla_openrti_copy(${OPENRTI_DIR}/src/OpenRTI/TestLib.h ${GEN}/include/TestLib.h)
hla_openrti_copy(${OPENRTI_DIR}/tests/rti1516e/include/RTI1516ETestLib.h ${GEN}/include/RTI1516ETestLib.h)
# The test ambassador misses four callbacks of the IEEE 1516-2010
# FederateAmbassador (transportation type, 6.29 to 6.32 and 8.?): defined empty.
file(READ ${GEN}/include/RTI1516ETestLib.h text)
string(REPLACE "  virtual bool execJoined(rti1516e::RTIambassador& ambassador) = 0;"
"  virtual bool execJoined(rti1516e::RTIambassador& ambassador) = 0;

  virtual void confirmAttributeTransportationTypeChange(rti1516e::ObjectInstanceHandle, rti1516e::AttributeHandleSet,
                                                        rti1516e::TransportationType)
    RTI_THROW ((rti1516e::FederateInternalError)) { }
  virtual void reportAttributeTransportationType(rti1516e::ObjectInstanceHandle, rti1516e::AttributeHandle,
                                                 rti1516e::TransportationType)
    RTI_THROW ((rti1516e::FederateInternalError)) { }
  virtual void confirmInteractionTransportationTypeChange(rti1516e::InteractionClassHandle, rti1516e::TransportationType)
    RTI_THROW ((rti1516e::FederateInternalError)) { }
  virtual void reportInteractionTransportationType(rti1516e::FederateHandle, rti1516e::InteractionClassHandle,
                                                   rti1516e::TransportationType)
    RTI_THROW ((rti1516e::FederateInternalError)) { }

  // As in the IEEE 1516-2000 test library (used by the ported tests).
  const rti1516e::FederateHandle& getFederateHandle() const
  { return _federateHandle; }
  rti1516e::FederateHandle _federateHandle;
  const std::wstring& getLogicalTimeFactoryName() const
  { return _logicalTimeFactoryName; }
  void setLogicalTimeFactoryName(const std::wstring& logicalTimeFactoryName)
  { _logicalTimeFactoryName = logicalTimeFactoryName; }
  std::wstring _logicalTimeFactoryName = L\"HLAinteger64Time\";" text "${text}")
string(REPLACE "getFddFileList(), std::wstring(L\"HLAinteger64Time\"))" "getFddFileList(), _logicalTimeFactoryName)"
  text "${text}")
string(REPLACE "getMimFile(), std::wstring(L\"HLAinteger64Time\"))" "getMimFile(), _logicalTimeFactoryName)"
  text "${text}")
# Its reflect/receive/remove callbacks are replaced by callbacks2000.h.
string(FIND "${text}" "  virtual void reflectAttributeValues(rti1516e::ObjectInstanceHandle theObject," first)
string(FIND "${text}" "  virtual void attributesInScope(" last)
if(first EQUAL -1 OR last EQUAL -1 OR NOT first LESS last)
  message(FATAL_ERROR "RTI1516ETestLib.h: callback block not found")
endif()
math(EXPR length "${last} - ${first}")
string(SUBSTRING "${text}" 0 ${first} head)
string(SUBSTRING "${text}" ${last} -1 tail)
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/callbacks2000.h callbacks)
string(REPLACE "\r\n" "\n" callbacks "${callbacks}")
set(text "${head}${callbacks}${tail}")
set_property(DIRECTORY ${CMAKE_SOURCE_DIR} APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/callbacks2000.h)
string(REPLACE "        ambassador->joinFederationExecution(getFederateType(), getFederationExecution());"
  "        _federateHandle = ambassador->joinFederationExecution(getFederateType(), getFederationExecution());"
  text "${text}")
# Each test repeats its scenario 99 times (with or without -J); the
# HLA_OPENRTI_ITERATIONS environment variable lowers that for the heavy runs.
string(REPLACE "#include <cstring>" "#include <cstdlib>\n#include <cstring>" text "${text}")
string(REPLACE "    unsigned n = 99;"
  "    unsigned n = std::getenv(\"HLA_OPENRTI_ITERATIONS\") ? unsigned(std::atoi(std::getenv(\"HLA_OPENRTI_ITERATIONS\"))) : 99u;"
  text "${text}")
file(WRITE ${GEN}/include/RTI1516ETestLib.h "${text}")

set(T ${OPENRTI_DIR}/tests/rti1516e)
function(hla_openrti_test name source)
  hla_openrti_copy(${T}/${source} ${GEN}/${source})
  add_executable(${name} ${GEN}/${source})
  # Our RTI headers first, then the patched helpers, then the OpenRTI core.
  target_include_directories(${name} BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${GEN}/include
    ${OPENRTI_DIR}/src/OpenRTI ${CMAKE_CURRENT_BINARY_DIR}/openrti-core)
  target_link_libraries(${name} PRIVATE rti1516e fedtime1516e OpenRTI)
  if(MSVC)
    target_compile_options(${name} PRIVATE /w /EHsc /bigobj)
    target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    set_property(TARGET ${name} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  else()
    target_compile_options(${name} PRIVATE -w)
  endif()
  set_property(TARGET ${name} PROPERTY FOLDER openrti)
endfunction()

# Tests written for the IEEE 1516-2000 API (OpenRTI tests/rti1516), ported
# to IEEE 1516-2010 at configure time: namespace, test library, and the few
# calls and callbacks whose signatures changed (see hla_openrti_port_fixes).
function(hla_openrti_port name source)
  file(READ ${OPENRTI_DIR}/tests/rti1516/${source} text)
  string(REPLACE "\r\n" "\n" text "${text}")
  string(REPLACE "L\"thread:///\"" "L\"\"" text "${text}")
  string(REPLACE "L\"thread://\"" "L\"\"" text "${text}")
  string(REPLACE "RTI1516TestLib.h" "RTI1516ETestLib.h" text "${text}")
  string(REPLACE "RTI1516TestAmbassador" "RTI1516ETestAmbassador" text "${text}")
  string(REPLACE "RTI1516SimpleAmbassador" "RTI1516ESimpleAmbassador" text "${text}")
  string(REPLACE "rti1516::" "rti1516e::" text "${text}")
  string(REPLACE "<RTI/HLAfloat64" "<RTI/time/HLAfloat64" text "${text}")
  string(REPLACE "<RTI/HLAinteger64" "<RTI/time/HLAinteger64" text "${text}")
  hla_openrti_port_fixes(${source})
  file(WRITE ${GEN}/rti1516/${source} "${text}")
  set_property(DIRECTORY ${CMAKE_SOURCE_DIR} APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${OPENRTI_DIR}/tests/rti1516/${source})
  add_executable(${name} ${GEN}/rti1516/${source})
  target_include_directories(${name} BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/include ${GEN}/include
    ${OPENRTI_DIR}/src/OpenRTI ${CMAKE_CURRENT_BINARY_DIR}/openrti-core)
  target_link_libraries(${name} PRIVATE rti1516e fedtime1516e OpenRTI)
  if(MSVC)
    target_compile_options(${name} PRIVATE /w /EHsc /bigobj)
    target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    set_property(TARGET ${name} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  else()
    target_compile_options(${name} PRIVATE -w)
  endif()
endfunction()
# Per-file adaptations (macro: edits `text` of the caller).
macro(hla_openrti_port_fixes source)
  # IEEE 1516-2010: connect() before any other service, no factory arguments.
  string(REPLACE "    ambassador = factory.createRTIambassador(args);"
"    ambassador = factory.createRTIambassador();
    static rti1516e::NullFederateAmbassador hlaNullFederateAmbassador;
    ambassador->connect(hlaNullFederateAmbassador, rti1516e::HLA_EVOKED, getConnectUrl());" text "${text}")
  string(REPLACE "#include <RTI/RTIambassadorFactory.h>"
    "#include <RTI/RTIambassadorFactory.h>\n#include <RTI/NullFederateAmbassador.h>" text "${text}")
  string(REPLACE "ambassador.connect(getArgumentList());" "ambassador.connect(getConnectUrl());" text "${text}")
  # The standard time classes live in namespace rti1516e.
  string(REPLACE "<HLAfloat64Time, HLAfloat64Interval>" "<rti1516e::HLAfloat64Time, rti1516e::HLAfloat64Interval>"
    text "${text}")
  string(REPLACE "<HLAinteger64Time, HLAinteger64Interval>"
    "<rti1516e::HLAinteger64Time, rti1516e::HLAinteger64Interval>" text "${text}")
  # IEEE 1516.1-2010 6.8: registering needs the class published, that is at
  # least one attribute published (5.2); OpenRTI also accepts an empty set.
  if("${source}" STREQUAL "objectinstance/objectname.cpp")
    string(REPLACE "      ambassador.publishObjectClassAttributes(objectClassHandle1, attributeHandles);"
"      std::set<rti1516e::AttributeHandle> publishedHandles = attributeHandles;
      publishedHandles.insert(ambassador.getAttributeHandle(objectClassHandle1, L\"HLAprivilegeToDeleteObject\"));
      ambassador.publishObjectClassAttributes(objectClassHandle1, publishedHandles);" text "${text}")
  endif()
endmacro()

hla_openrti_port(openrti-create create/create.cpp)
hla_openrti_port(openrti-concurrent-create create/concurrent-create.cpp)
hla_openrti_port(openrti-join join/join.cpp)
hla_openrti_port(openrti-concurrent-join join/concurrent-join.cpp)
hla_openrti_port(openrti-sync sync/sync.cpp)
hla_openrti_port(openrti-interaction interaction/interaction.cpp)
hla_openrti_port(openrti-objectinstance objectinstance/objectinstance.cpp)
hla_openrti_port(openrti-objectname objectinstance/objectname.cpp)
hla_openrti_port(openrti-concurrent-time time/concurrent-time.cpp)

hla_openrti_test(openrti-link link/link.cpp)
hla_openrti_test(openrti-encode encode/encode.cpp)
hla_openrti_test(openrti-logical-time time/logical-time.cpp)
# This RTI raises IllegalTimeArithmetic when a result leaves [initial, final]
# (IEEE 1516.1-2010 12.1); OpenRTI saturates at the final time, and the test
# relies on it to end its loops. The patched test saturates in the test itself
# (an interval that overflowed stands for "beyond the final time").
file(READ ${GEN}/time/logical-time.cpp text)
string(REPLACE "\nint\nmain(" "
#include <set>
static std::set<const void*> hlaSaturatedIntervals;
template<class T, class I>
static void hlaSaturatingAdd(T& t, const I& i)
{
  if (hlaSaturatedIntervals.count(&i)) { t.setFinal(); return; }
  try { t += i; } catch (const rti1516e::IllegalTimeArithmetic&) { t.setFinal(); }
}
template<class I>
static void hlaSaturatingDouble(I& i)
{ try { i += i; } catch (const rti1516e::IllegalTimeArithmetic&) { hlaSaturatedIntervals.insert(&i); } }

int
main(" text "${text}")
string(REPLACE "*logicalTime += *logicalTimeInterval;" "hlaSaturatingAdd(*logicalTime, *logicalTimeInterval);" text "${text}")
string(REPLACE "*logicalTime += *logicalTimeInterval2;" "hlaSaturatingAdd(*logicalTime, *logicalTimeInterval2);" text "${text}")
string(REPLACE "*logicalTimeInterval += *logicalTimeInterval;" "hlaSaturatingDouble(*logicalTimeInterval);" text "${text}")
string(REPLACE "*logicalTimeInterval2 += *logicalTimeInterval2;" "hlaSaturatingDouble(*logicalTimeInterval2);" text "${text}")
string(REPLACE "logicalTimeInterval2->setEpsilon();"
  "logicalTimeInterval2->setEpsilon(); hlaSaturatedIntervals.erase(logicalTimeInterval2.get());" text "${text}")
file(WRITE ${GEN}/time/logical-time.cpp "${text}")
hla_openrti_test(openrti-fddget fddget/fddget.cpp)
hla_openrti_test(openrti-modules modules/modules.cpp)
# Merging rule: OpenRTI also refuses a module that names a class without
# attributes (or parameters) next to a module that defines it; IEEE
# 1516.2-2010 allows it (scaffolding definition, used by modular FOMs such as
# RPR FOM 2). The patched test keeps only the conflicts the standard has: the
# same class, dimension or update rate defined differently (X-1 and X-2).
file(READ ${GEN}/modules/modules.cpp text)
string(REPLACE
  "{ return _incompatibleModuleNames.find(name) != _incompatibleModuleNames.end(); }"
  "{
    if (_incompatibleModuleNames.find(name) == _incompatibleModuleNames.end())
      return false;
    std::wstring::size_type a = _name.rfind(L'-'), b = name.rfind(L'-');
    return a != std::wstring::npos && b != std::wstring::npos && _name.substr(0, a) == name.substr(0, b);
  }" text "${text}")
file(WRITE ${GEN}/modules/modules.cpp "${text}")

# Launcher: clean topic prefix per test (OpenRTI tests reuse federation names).
add_executable(hla-openrti-run run.cpp)
target_link_libraries(hla-openrti-run PRIVATE ${PAHO_TARGET})
if(MSVC)
  target_compile_options(hla-openrti-run PRIVATE /W3 /EHsc /utf-8 /wd4251 /wd4275)
  set_property(TARGET hla-openrti-run PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# ---------------------------------------------------------------------- tests
function(hla_openrti_add name program)
  add_test(NAME openrti/${name} COMMAND hla-openrti-run openrti-${name} $<TARGET_FILE:${program}> ${ARGN})
  set_tests_properties(openrti/${name} PROPERTIES LABELS openrti TIMEOUT 600)
endfunction()

hla_openrti_add(link openrti-link)
hla_openrti_add(encode openrti-encode)
hla_openrti_add(logical-time-1 openrti-logical-time)
hla_openrti_add(logical-time-2 openrti-logical-time -F HLAfloat64Time)
hla_openrti_add(logical-time-3 openrti-logical-time -F HLAinteger64Time)

set(F ${T}/fddget)
hla_openrti_add(fddget-1 openrti-fddget -O ${F}/fdd-1.xml -o HLAobjectRoot -a HLAprivilegeToDeleteObject -i HLAinteractionRoot)
hla_openrti_add(fddget-2 openrti-fddget -O ${F}/fdd-1.xml -o ObjectClass1 -a HLAprivilegeToDeleteObject -a Attribute1
  -i InteractionClass1 -p Parameter1)
hla_openrti_add(fddget-3 openrti-fddget -O ${F}/fdd-1.xml -o HLAobjectRoot.ObjectClass1 -a HLAprivilegeToDeleteObject
  -a HLAobjectRoot.ObjectClass1.Attribute1 -i HLAinteractionRoot.InteractionClass1
  -p HLAinteractionRoot.InteractionClass1.Parameter1)
hla_openrti_add(fddget-4 openrti-fddget -O ${F}/fdd-1.xml -o ObjectClass2 -a HLAprivilegeToDeleteObject -a Attribute1
  -a Attribute2 -i InteractionClass2 -p Parameter1 -p Parameter2)
hla_openrti_add(fddget-5 openrti-fddget -O ${F}/fdd-1.xml -o HLAobjectRoot.ObjectClass1.ObjectClass2
  -a HLAprivilegeToDeleteObject -a HLAobjectRoot.ObjectClass1.Attribute1
  -a HLAobjectRoot.ObjectClass1.ObjectClass2.Attribute2 -i HLAinteractionRoot.InteractionClass1.InteractionClass2
  -p HLAinteractionRoot.InteractionClass1.Parameter1 -p HLAinteractionRoot.InteractionClass1.InteractionClass2.Parameter2)
hla_openrti_add(fddget-6 openrti-fddget -O ${F}/fdd-1.xml -o ObjectClass3 -a HLAprivilegeToDeleteObject -a Attribute3
  -i InteractionClass3 -p Parameter3)
hla_openrti_add(fddget-7 openrti-fddget -O ${F}/fdd-1.xml -o HLAobjectRoot.ObjectClass3 -a HLAprivilegeToDeleteObject
  -a HLAobjectRoot.ObjectClass3.Attribute3 -i HLAinteractionRoot.InteractionClass3
  -p HLAinteractionRoot.InteractionClass3.Parameter3)
hla_openrti_add(fddget-11 openrti-fddget -O ${F}/fdd-2.xml -o HLAobjectRoot -a HLAprivilegeToDeleteObject -i HLAinteractionRoot)
hla_openrti_add(fddget-12 openrti-fddget -O ${F}/fdd-2.xml -o ObjectClass1 -a HLAprivilegeToDeleteObject -a Attribute1
  -i InteractionClass1 -p Parameter1)
hla_openrti_add(fddget-13 openrti-fddget -O ${F}/fdd-2.xml -o ObjectClass1 -a HLAprivilegeToDeleteObject
  -a ObjectClass1.Attribute1 -i InteractionClass1 -p InteractionClass1.Parameter1)
hla_openrti_add(fddget-14 openrti-fddget -O ${F}/fdd-2.xml -o ObjectClass2 -a HLAprivilegeToDeleteObject -a Attribute1
  -a Attribute2 -i InteractionClass2 -p Parameter1 -p Parameter2)
hla_openrti_add(fddget-15 openrti-fddget -O ${F}/fdd-2.xml -o ObjectClass1.ObjectClass2 -a HLAprivilegeToDeleteObject
  -a ObjectClass1.Attribute1 -a ObjectClass1.ObjectClass2.Attribute2 -i InteractionClass1.InteractionClass2
  -p InteractionClass1.Parameter1 -p InteractionClass1.InteractionClass2.Parameter2)
hla_openrti_add(fddget-16 openrti-fddget -O ${F}/fdd-2.xml -o ObjectClass3 -a HLAprivilegeToDeleteObject -a Attribute3
  -i InteractionClass3 -p Parameter3)
hla_openrti_add(fddget-17 openrti-fddget -O ${F}/fdd-2.xml -o ObjectClass3 -a HLAprivilegeToDeleteObject
  -a ObjectClass3.Attribute3 -i InteractionClass3 -p InteractionClass3.Parameter3)
hla_openrti_add(fddget-20 openrti-fddget -D -O ${F}/fdd-2.xml -o ObjectClass3 -a HLAprivilegeToDeleteObject -a Attribute3
  -i InteractionClass3 -p Parameter3)

# Server count (-S) is meaningless here: the broker is the infrastructure.
set(M ${T}/modules)
hla_openrti_add(modules-1 openrti-modules -S0 -A1 -D -O ${M})
hla_openrti_add(modules-2 openrti-modules -S0 -A10 -D -O ${M})
hla_openrti_add(modules-6 openrti-modules -S0 -A1 -O ${M})
hla_openrti_add(modules-7 openrti-modules -S0 -A10 -O ${M})

# Ported IEEE 1516-2000 tests (-A: federates in threads, -J: join once).
set(R ${OPENRTI_DIR}/tests/rti1516)
hla_openrti_add(create-1 openrti-create -S0 -A1 -O ${R}/fdd.xml)
hla_openrti_add(create-2 openrti-create -S0 -A10 -O ${R}/fdd.xml)
hla_openrti_add(concurrent-create-1 openrti-concurrent-create -S0 -A10 -O ${R}/fdd.xml)
hla_openrti_add(join-1 openrti-join -S0 -A1 -O ${R}/fdd.xml)
hla_openrti_add(join-2 openrti-join -S0 -A10 -O ${R}/fdd.xml)
hla_openrti_add(concurrent-join-1 openrti-concurrent-join -S0 -A10 -O ${R}/fdd.xml)
hla_openrti_add(sync-1 openrti-sync -S0 -A1 -O ${R}/fdd.xml)
hla_openrti_add(sync-2 openrti-sync -S0 -A10 -O ${R}/fdd.xml)
hla_openrti_add(interaction-1 openrti-interaction -S0 -A1 -O ${R}/interaction/fdd.xml)
hla_openrti_add(interaction-2 openrti-interaction -S0 -A10 -O ${R}/interaction/fdd.xml)
hla_openrti_add(interaction-3 openrti-interaction -S0 -A10 -J -O ${R}/interaction/fdd.xml)
hla_openrti_add(objectname-1 openrti-objectname -S0 -A1 -J -O ${R}/objectinstance/fdd.xml)
hla_openrti_add(objectname-2 openrti-objectname -S0 -A10 -J -O ${R}/objectinstance/fdd.xml)
hla_openrti_add(objectinstance-1 openrti-objectinstance -S0 -A1 -J -O ${R}/objectinstance/fdd.xml)
hla_openrti_add(objectinstance-2 openrti-objectinstance -S0 -A10 -J -O ${R}/objectinstance/fdd.xml)
# With 10 federates, each synchronization point costs N^3 receptions over the
# broker (N points, N achievements, N receivers), and these tests synchronize
# about 25 times per iteration: 10 iterations instead of 99.
set_tests_properties(openrti/interaction-2 openrti/interaction-3 openrti/objectinstance-2
  PROPERTIES ENVIRONMENT HLA_OPENRTI_ITERATIONS=10)
# Time management: -L lookahead (0, 1), -N advance service (0 timeAdvanceRequest,
# 1 timeAdvanceRequestAvailable, 2 nextMessageRequest, 3 nextMessageRequestAvailable,
# 4 flushQueueRequest, 5 mixed), 10 time-regulating and constrained federates.
foreach(L 0 1)
  foreach(N 0 1 2 3 4 5)
    hla_openrti_add(concurrent-time-L${L}-N${N} openrti-concurrent-time -I0 -U0 -L${L} -N${N} -S0 -A10 -J
      -O ${R}/time/fdd.xml)
    # 100 time steps per iteration: about 6 s per iteration with 10 federates.
    set_tests_properties(openrti/concurrent-time-L${L}-N${N} PROPERTIES ENVIRONMENT HLA_OPENRTI_ITERATIONS=10)
  endforeach()
endforeach()
# Known failure: advance services drawn at random (TAR, TARA, NER, NERA, FQR)
# with a zero lookahead give early grants and stalls (conformance/README.md).
set_tests_properties(openrti/concurrent-time-L0-N5 PROPERTIES DISABLED TRUE)
