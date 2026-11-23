// Entry point used by every 1516e federate:
//   rti1516e::RTIambassadorFactory().createRTIambassador()
// Exported signature (auto_ptr) must match the vendor headers exactly.

#include <RTI/RTIambassadorFactory.h>

#include "MqttRTIambassador.h"

#include <new>

namespace rti1516e
{
RTIambassadorFactory::RTIambassadorFactory() {}

RTIambassadorFactory::~RTIambassadorFactory() RTI_NOEXCEPT {}

RTI_UNIQUE_PTR<RTIambassador> RTIambassadorFactory::createRTIambassador()
{
   try
   {
      return RTI_UNIQUE_PTR<RTIambassador>(new hla_mqtt::MqttRTIambassador);
   }
   catch (std::bad_alloc const&)
   {
      throw RTIinternalError(L"out of memory while creating the RTIambassador");
   }
}
}
