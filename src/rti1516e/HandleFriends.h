// Internal access to the standard handle classes.
// The IEEE headers declare `friend class XxxHandleFriend;` in every handle
// class: these friends are the only way for the RTI to build a handle from
// an internal value and to read that value back.
#pragma once

#include <RTI/Handle.h>
#include <cstdint>

namespace rti1516e
{
#define HLA_MQTT_DECLARE_HANDLE_FRIEND(Kind)                   \
   class Kind##Friend                                          \
   {                                                           \
   public:                                                     \
      /* value 0 is reserved: it gives an invalid handle */    \
      static Kind create(std::uint64_t value);                 \
      static std::uint64_t value(Kind const & handle);         \
   };

HLA_MQTT_DECLARE_HANDLE_FRIEND(FederateHandle)
HLA_MQTT_DECLARE_HANDLE_FRIEND(ObjectClassHandle)
HLA_MQTT_DECLARE_HANDLE_FRIEND(InteractionClassHandle)
HLA_MQTT_DECLARE_HANDLE_FRIEND(ObjectInstanceHandle)
HLA_MQTT_DECLARE_HANDLE_FRIEND(AttributeHandle)
HLA_MQTT_DECLARE_HANDLE_FRIEND(ParameterHandle)
HLA_MQTT_DECLARE_HANDLE_FRIEND(DimensionHandle)
HLA_MQTT_DECLARE_HANDLE_FRIEND(MessageRetractionHandle)
HLA_MQTT_DECLARE_HANDLE_FRIEND(RegionHandle)

#undef HLA_MQTT_DECLARE_HANDLE_FRIEND
}
