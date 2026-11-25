cmake_minimum_required(VERSION 3.20)
project(hla_mqtt_rti VERSION 0.1.0 LANGUAGES C CXX)

# ---------------------------------------------------------------- options
set(HLA_MQTT_RTI_LIBRARY_NAME "rti1516e" CACHE STRING
    "File name of the RTI library (without extension). Must match what the federates import (dumpbin /dependents).")
set(HLA_MQTT_FEDTIME_LIBRARY_NAME "fedtime1516e" CACHE STRING
    "File name of the fedtime library (without extension), which exports LogicalTimeFactoryFactory.")
set(HLA_MQTT_RTI_DEBUG_POSTFIX "d" CACHE STRING
    "Postfix of the Debug library (rti1516ed.dll). Empty to disable.")
option(HLA_MQTT_RTI_USE_UNIQUE_PTR
    "Map RTI_UNIQUE_PTR to std::unique_ptr (ABI of OpenRTI headers built in C++17 with GCC/Clang). OFF = std::auto_ptr, IEEE ABI."
    OFF)
option(HLA_MQTT_RTI_BUILD_TESTS "Build the test federate" ON)
option(HLA_MQTT_MAK_NAMES
    "MAK RTI file names (librti1516e64 / libfedtime1516e64 in 64 bits, without 64 in 32 bits) and PE linker version 14.00 (VR-Link). Overrides the two library names."
    OFF)
set(HLA_MQTT_PE_LINKER_VERSION "" CACHE STRING
    "Windows: linker version written into the PE header of the DLLs after the link (<major>.<minor>, e.g. 14.00, expected by VR-Link). Empty: unchanged. 14.00 by default with HLA_MQTT_MAK_NAMES.")

set(HLA_MQTT_RTI_OUTPUT_NAME ${HLA_MQTT_RTI_LIBRARY_NAME})
set(HLA_MQTT_FEDTIME_OUTPUT_NAME ${HLA_MQTT_FEDTIME_LIBRARY_NAME})
set(HLA_MQTT_PE_VERSION "${HLA_MQTT_PE_LINKER_VERSION}")
if(HLA_MQTT_MAK_NAMES)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_mak_bits 64)
  else()
    set(_mak_bits "")
  endif()
  set(HLA_MQTT_RTI_OUTPUT_NAME librti1516e${_mak_bits})
  set(HLA_MQTT_FEDTIME_OUTPUT_NAME libfedtime1516e${_mak_bits})
  if("${HLA_MQTT_PE_VERSION}" STREQUAL "")
    set(HLA_MQTT_PE_VERSION "14.00")
  endif()
endif()
message(STATUS "RTI libraries: ${HLA_MQTT_RTI_OUTPUT_NAME}, ${HLA_MQTT_FEDTIME_OUTPUT_NAME}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Put the DLL and the test executable side by side (Windows DLL lookup).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ---------------------------------------------------------------- dependencies
find_package(PahoMqttCpp REQUIRED)
if(TARGET PahoMqttCpp::paho-mqttpp3-static)
  set(PAHO_TARGET PahoMqttCpp::paho-mqttpp3-static)
elseif(TARGET PahoMqttCpp::paho-mqttpp3)
  set(PAHO_TARGET PahoMqttCpp::paho-mqttpp3)
else()
  set(PAHO_TARGET PahoMqttCpp::PahoMqttCpp)
endif()
message(STATUS "Paho MQTT C++ target: ${PAHO_TARGET}")

# ---------------------------------------------------------------- generated sources
set(GEN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/rti1516e/generated)
if(NOT EXISTS ${GEN_DIR}/RTIambassadorStubs.cpp)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_rti_sources.py
                  COMMAND_ERROR_IS_FATAL ANY)
endif()

# ---------------------------------------------------------------- RTI library
add_library(rti1516e SHARED
  src/rti1516e/CommonTypes.cpp
  src/rti1516e/Handles.cpp
  src/rti1516e/VariableLengthData.cpp
  src/rti1516e/LocalSettings.cpp
  src/rti1516e/MqttTransport.cpp
  src/rti1516e/MqttRTIambassador.cpp
  src/rti1516e/MqttFederation.cpp
  src/rti1516e/MqttSupportServices.cpp
  src/rti1516e/MqttObjects.cpp
  src/rti1516e/MqttSync.cpp
  src/rti1516e/MqttTime.cpp
  src/rti1516e/MqttAdvisories.cpp
  src/rti1516e/MqttRegions.cpp
  src/rti1516e/MqttObjectOptions.cpp
  src/rti1516e/MqttOwnership.cpp
  src/rti1516e/MqttSaveRestore.cpp
  src/rti1516e/MqttMom.cpp
  src/rti1516e/Trace.cpp
  src/rti1516e/Fdd.cpp
  src/rti1516e/Xml.cpp
  src/rti1516e/RTIambassadorFactory.cpp
  src/rti1516e/encoding/BasicDataElements.cpp
  src/rti1516e/encoding/ConstructedDataElements.cpp
  src/rti1516e/time/StandardLogicalTimes.cpp
  src/rti1516e/generated/Exceptions.cpp
  src/rti1516e/generated/RTIambassadorStubs.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/embedded/StandardMim.cpp
)
# Standard MIM (IEEE 1516.1-2010), built into the library: merged implicitly
# when a federation is created without a MIM.
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/embedded/StandardMim.cpp
  COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/mim/HLAstandardMIM.xml
          -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/embedded/StandardMim.cpp -DSYMBOL=standardMim
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/mim/HLAstandardMIM.xml ${CMAKE_CURRENT_SOURCE_DIR}/cmake/EmbedFile.cmake
  COMMENT "Embedding the standard MIM")
target_include_directories(rti1516e
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/rti1516e)
target_compile_definitions(rti1516e PRIVATE BUILDING_RTI RTI_DISABLE_WARNINGS)
if(HLA_MQTT_RTI_USE_UNIQUE_PTR)
  target_compile_definitions(rti1516e PUBLIC HLA_MQTT_RTI_USE_UNIQUE_PTR)
endif()
target_link_libraries(rti1516e PRIVATE ${PAHO_TARGET})
set_target_properties(rti1516e PROPERTIES
  OUTPUT_NAME  ${HLA_MQTT_RTI_OUTPUT_NAME}
  DEBUG_POSTFIX "${HLA_MQTT_RTI_DEBUG_POSTFIX}"
  VERSION ${PROJECT_VERSION}
  SOVERSION 1)

if(MSVC)
  # auto_ptr is required by the IEEE 1516e signatures (ABI).
  target_compile_definitions(rti1516e PUBLIC _HAS_AUTO_PTR_ETC=1)
  target_compile_options(rti1516e PRIVATE /W3 /EHsc /utf-8 /wd4251 /wd4275)
  # Federates use the shared CRT (/MD, /MDd): never link the static CRT here.
  set_property(TARGET rti1516e PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
else()
  target_compile_options(rti1516e PRIVATE -Wall -Wno-deprecated-declarations)
  target_compile_options(rti1516e PUBLIC -Wno-deprecated-declarations)
  find_package(Threads REQUIRED)
  target_link_libraries(rti1516e PRIVATE Threads::Threads)
endif()

# ---------------------------------------------------------------- fedtime library
# LogicalTimeFactoryFactory is declared RTI_EXPORT_FEDTIME: federates import it
# from a separate DLL, which forwards to HLAlogicalTimeFactoryFactory (rti1516e).
add_library(fedtime1516e SHARED src/fedtime1516e/LogicalTimeFactoryFactory.cpp)
target_compile_definitions(fedtime1516e PRIVATE BUILDING_FEDTIME RTI_DISABLE_WARNINGS)
target_link_libraries(fedtime1516e PUBLIC rti1516e)
set_target_properties(fedtime1516e PROPERTIES
  OUTPUT_NAME  ${HLA_MQTT_FEDTIME_OUTPUT_NAME}
  DEBUG_POSTFIX "${HLA_MQTT_RTI_DEBUG_POSTFIX}"
  VERSION ${PROJECT_VERSION}
  SOVERSION 1)
if(MSVC)
  target_compile_options(fedtime1516e PRIVATE /W3 /EHsc /utf-8 /wd4251 /wd4275)
  set_property(TARGET fedtime1516e PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# ---------------------------------------------------------------- PE linker version
# VR-Link reads the linker version of the PE header as the "compiler version"
# of the RTI: written after the link by a small host tool.
if(WIN32 AND NOT "${HLA_MQTT_PE_VERSION}" STREQUAL "")
  if(NOT "${HLA_MQTT_PE_VERSION}" MATCHES "^[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "HLA_MQTT_PE_LINKER_VERSION: <major>.<minor> expected, got '${HLA_MQTT_PE_VERSION}'")
  endif()
  add_executable(hla-pe-linker-version tools/pe_linker_version.cpp)
  if(MSVC)
    target_compile_options(hla-pe-linker-version PRIVATE /W3)
  endif()
  foreach(lib rti1516e fedtime1516e)
    add_dependencies(${lib} hla-pe-linker-version)
    add_custom_command(TARGET ${lib} POST_BUILD
      COMMAND $<TARGET_FILE:hla-pe-linker-version> $<TARGET_FILE:${lib}> ${HLA_MQTT_PE_VERSION}
      COMMENT "PE linker version ${HLA_MQTT_PE_VERSION}: $<TARGET_FILE_NAME:${lib}>"
      VERBATIM)
  endforeach()
  message(STATUS "PE linker version of the DLLs: ${HLA_MQTT_PE_VERSION}")
endif()

# ---------------------------------------------------------------- test federate
if(HLA_MQTT_RTI_BUILD_TESTS)
  include(CTest)
  add_executable(hla-test-federate tests/connect_federate/main.cpp)
  target_link_libraries(hla-test-federate PRIVATE rti1516e)
  if(MSVC)
    # C4251/C4275: std:: types in exported classes, inherent to the IEEE API
    target_compile_options(hla-test-federate PRIVATE /W3 /EHsc /utf-8 /wd4251 /wd4275)
    set_property(TARGET hla-test-federate PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  endif()
  # Needs a reachable broker: HLA_MQTT_BROKER env var, default tcp://localhost:1883
  add_test(NAME selftest COMMAND hla-test-federate --selftest)

  # Encoding helpers and standard logical times: no broker needed.
  add_executable(hla-encoding-test tests/encoding/main.cpp)
  target_link_libraries(hla-encoding-test PRIVATE rti1516e fedtime1516e)
  if(MSVC)
    target_compile_options(hla-encoding-test PRIVATE /W3 /EHsc /utf-8 /wd4251 /wd4275)
    set_property(TARGET hla-encoding-test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  endif()
  add_test(NAME encoding COMMAND hla-encoding-test)

  # Federation management with several ambassadors: needs a broker.
  add_executable(hla-federation-test tests/federation/main.cpp)
  target_link_libraries(hla-federation-test PRIVATE rti1516e)
  target_compile_definitions(hla-federation-test PRIVATE
    HLA_TEST_FOM_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/federation/foms")
  if(MSVC)
    target_compile_options(hla-federation-test PRIVATE /W3 /EHsc /utf-8 /wd4251 /wd4275)
    set_property(TARGET hla-federation-test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  else()
    find_package(Threads REQUIRED)
    target_link_libraries(hla-federation-test PRIVATE Threads::Threads)
  endif()
  add_test(NAME federation COMMAND hla-federation-test)

  # Declaration and object management with several ambassadors: needs a broker.
  add_executable(hla-objects-test tests/objects/main.cpp)
  target_link_libraries(hla-objects-test PRIVATE rti1516e)
  target_compile_definitions(hla-objects-test PRIVATE
    HLA_TEST_FOM_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/federation/foms")
  if(MSVC)
    target_compile_options(hla-objects-test PRIVATE /W3 /EHsc /utf-8 /wd4251 /wd4275)
    set_property(TARGET hla-objects-test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  endif()
  add_test(NAME objects COMMAND hla-objects-test)

  # Synchronization points and time management: needs a broker.
  add_executable(hla-time-test tests/time/main.cpp)
  target_link_libraries(hla-time-test PRIVATE rti1516e)
  target_compile_definitions(hla-time-test PRIVATE
    HLA_TEST_FOM_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/federation/foms")
  if(MSVC)
    target_compile_options(hla-time-test PRIVATE /W3 /EHsc /utf-8 /wd4251 /wd4275)
    set_property(TARGET hla-time-test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  endif()
  add_test(NAME time COMMAND hla-time-test)

  # Data distribution management: needs a broker.
  add_executable(hla-ddm-test tests/ddm/main.cpp)
  target_link_libraries(hla-ddm-test PRIVATE rti1516e)
  target_compile_definitions(hla-ddm-test PRIVATE
    HLA_TEST_FOM_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/federation/foms")
  if(MSVC)
    target_compile_options(hla-ddm-test PRIVATE /W3 /EHsc /utf-8 /wd4251 /wd4275)
    set_property(TARGET hla-ddm-test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  endif()
  add_test(NAME ddm COMMAND hla-ddm-test)

  # Management Object Model: needs a broker.
  add_executable(hla-mom-test tests/mom/main.cpp)
  target_link_libraries(hla-mom-test PRIVATE rti1516e)
  target_compile_definitions(hla-mom-test PRIVATE
    HLA_TEST_FOM_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/federation/foms")
  if(MSVC)
    target_compile_options(hla-mom-test PRIVATE /W3 /EHsc /utf-8 /wd4251 /wd4275)
    set_property(TARGET hla-mom-test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  endif()
  add_test(NAME mom COMMAND hla-mom-test)

  # Ownership management (chapter 7): needs a broker.
  add_executable(hla-ownership-test tests/ownership/main.cpp)
  target_link_libraries(hla-ownership-test PRIVATE rti1516e)
  target_compile_definitions(hla-ownership-test PRIVATE
    HLA_TEST_FOM_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/federation/foms")
  if(MSVC)
    target_compile_options(hla-ownership-test PRIVATE /W3 /EHsc /utf-8 /wd4251 /wd4275)
    set_property(TARGET hla-ownership-test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  endif()
  add_test(NAME ownership COMMAND hla-ownership-test)

  # Federation save and restore (4.16 to 4.32): needs a broker.
  add_executable(hla-saverestore-test tests/saverestore/main.cpp)
  target_link_libraries(hla-saverestore-test PRIVATE rti1516e)
  target_compile_definitions(hla-saverestore-test PRIVATE
    HLA_TEST_FOM_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/federation/foms")
  if(MSVC)
    target_compile_options(hla-saverestore-test PRIVATE /W3 /EHsc /utf-8 /wd4251 /wd4275)
    set_property(TARGET hla-saverestore-test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  endif()
  add_test(NAME saverestore COMMAND hla-saverestore-test)

  # Reconnection and stale presences: needs a broker; a raw MQTT client cuts
  # the connections.
  add_executable(hla-robustness-test tests/robustness/main.cpp)
  target_link_libraries(hla-robustness-test PRIVATE rti1516e ${PAHO_TARGET})
  target_compile_definitions(hla-robustness-test PRIVATE
    HLA_TEST_FOM_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/federation/foms")
  if(MSVC)
    target_compile_options(hla-robustness-test PRIVATE /W3 /EHsc /utf-8 /wd4251 /wd4275)
    set_property(TARGET hla-robustness-test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  endif()
  add_test(NAME robustness COMMAND hla-robustness-test)

  if(TARGET hla-pe-linker-version)
    add_test(NAME pe-linker-version COMMAND hla-pe-linker-version --check $<TARGET_FILE:rti1516e> ${HLA_MQTT_PE_VERSION})
    add_test(NAME pe-linker-version-fedtime
             COMMAND hla-pe-linker-version --check $<TARGET_FILE:fedtime1516e> ${HLA_MQTT_PE_VERSION})
  endif()
endif()

# ---------------------------------------------------------------- install
include(GNUInstallDirs)
install(TARGETS rti1516e fedtime1516e
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/RTI DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(HLA_MQTT_RTI_BUILD_TESTS)
  install(TARGETS hla-test-federate hla-encoding-test hla-federation-test hla-objects-test hla-time-test hla-ddm-test hla-mom-test hla-ownership-test hla-saverestore-test hla-robustness-test RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
