/***********************************************************************
   The IEEE hereby grants a general, royalty-free license to copy, distribute,
   display and make derivative works from this material, for all purposes,
   provided that any use of the material contains the following
   attribution: "Reprinted with permission from IEEE 1516.1(TM)-2010".
   Should you require additional information, contact the Manager, Standards
   Intellectual Property, IEEE Standards Association (stds-ipr@ieee.org).
***********************************************************************/
/***********************************************************************
   IEEE 1516.1 High Level Architecture Interface Specification C++ API
   File: RTI/SpecificConfig.h

   hla-mqtt-rti note:
   The IEEE 1516.1-2010 API uses std::auto_ptr in its signatures
   (RTIambassadorFactory::createRTIambassador, getTimeFactory, ...).
   std::auto_ptr is part of the mangled names, so to stay *binary*
   compatible with federates built against other 1516e RTIs, this file
   pins RTI_UNIQUE_PTR to std::auto_ptr whatever the C++ standard used.

   Define HLA_MQTT_RTI_USE_UNIQUE_PTR only if the target federates were
   built with headers that map these signatures to std::unique_ptr
   (e.g. OpenRTI headers compiled in C++17 mode with GCC/Clang).
***********************************************************************/

#ifndef RTI_SpecificConfig_h
#define RTI_SpecificConfig_h

#if defined(_MSC_VER) && !defined(_HAS_AUTO_PTR_ETC)
// MSVC removes std::auto_ptr in /std:c++17 unless this is set.
// Must be defined before any standard header is included: the build
// system also passes it on the command line.
#define _HAS_AUTO_PTR_ETC 1
#endif

#if defined(_WIN32)
#if defined(_MSC_VER) && defined(RTI_DISABLE_WARNINGS)
#pragma warning(disable: 4786)
#pragma warning(disable: 4290)
#pragma warning(disable: 4251)
#pragma warning(disable: 4275)
#endif
//
// BUILDING_RTI is defined only when compiling the RTI DLL.
// BUILDING_FEDTIME is defined only when building a libfedtime DLL.
// STATIC_RTI / STATIC_FEDTIME for static builds.
//
#if defined(STATIC_RTI)
#define RTI_EXPORT
#else
#if defined(BUILDING_RTI)
#define RTI_EXPORT __declspec(dllexport)
#else
#define RTI_EXPORT __declspec(dllimport)
#endif
#endif

#if defined(STATIC_FEDTIME)
#define RTI_EXPORT_FEDTIME
#else
#if defined(BUILDING_FEDTIME)
#define RTI_EXPORT_FEDTIME __declspec(dllexport)
#else
#define RTI_EXPORT_FEDTIME __declspec(dllimport)
#endif
#endif

#else // !_WIN32
#if defined(__GNUC__) && defined(RTI_DISABLE_WARNINGS)
#pragma GCC diagnostic ignored "-Wdeprecated"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
// no special qualifiers on non-WIN32 platforms (default visibility)
#define RTI_EXPORT
#define RTI_EXPORT_FEDTIME
#endif

// Dynamic exception specifications are not part of the mangled names
// (neither MSVC nor Itanium ABI), so dropping them in C++17 is ABI-neutral.
#if defined(_MSC_VER)
#define RTI_CPLUSPLUS _MSVC_LANG
#else
#define RTI_CPLUSPLUS __cplusplus
#endif

#if RTI_CPLUSPLUS < 201703L
#define RTI_NOEXCEPT throw()
#define RTI_THROW(e) throw e
#else
#define RTI_NOEXCEPT noexcept
#define RTI_THROW(e)
#endif

#if defined(HLA_MQTT_RTI_USE_UNIQUE_PTR)
#define RTI_UNIQUE_PTR std::unique_ptr
#else
#define RTI_UNIQUE_PTR std::auto_ptr
#endif

#endif // RTI_SpecificConfig_h
