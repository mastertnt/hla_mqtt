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
  src/rti1516e/Trace.cpp
  src/rti1516e/Fdd.cpp
  src/rti1516e/Xml.cpp
  src/rti1516e/RTIambassadorFactory.cpp
  src/rti1516e/encoding/BasicDataElements.cpp
  src/rti1516e/encoding/ConstructedDataElements.cpp
  src/rti1516e/time/StandardLogicalTimes.cpp
  src/rti1516e/generated/Exceptions.cpp
  src/rti1516e/generated/RTIambassadorStubs.cpp
)
target_include_directories(rti1516e
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/rti1516e)
target_compile_definitions(rti1516e PRIVATE BUILDING_RTI RTI_DISABLE_WARNINGS)
if(HLA_MQTT_RTI_USE_UNIQUE_PTR)
  target_compile_definitions(rti1516e PUBLIC HLA_MQTT_RTI_USE_UNIQUE_PTR)
endif()
target_link_libraries(rti1516e PRIVATE ${PAHO_TARGET})
set_target_properties(rti1516e PROPERTIES
  OUTPUT_NAME  ${HLA_MQTT_RTI_LIBRARY_NAME}
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
  OUTPUT_NAME  ${HLA_MQTT_FEDTIME_LIBRARY_NAME}
  DEBUG_POSTFIX "${HLA_MQTT_RTI_DEBUG_POSTFIX}"
  VERSION ${PROJECT_VERSION}
  SOVERSION 1)
if(MSVC)
  target_compile_options(fedtime1516e PRIVATE /W3 /EHsc /utf-8 /wd4251 /wd4275)
  set_property(TARGET fedtime1516e PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
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
endif()

# ---------------------------------------------------------------- install
include(GNUInstallDirs)
install(TARGETS rti1516e fedtime1516e
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/RTI DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(HLA_MQTT_RTI_BUILD_TESTS)
  install(TARGETS hla-test-federate hla-encoding-test hla-federation-test hla-objects-test hla-time-test RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
