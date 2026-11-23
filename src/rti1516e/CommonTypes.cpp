// Small exported classes of the standard API that have no MQTT logic:
// Exception base, ambassador base classes, RangeBounds, Typedefs classes,
// logical time base classes, rtiName/rtiVersion.

#include <RTI/RTI1516.h>
// NullFederateAmbassador is declared RTI_EXPORT with inline members: under
// MSVC a federate imports them (and the vtable) from the RTI DLL, so the DLL
// must compile the class definition to export them, like vendor RTIs do.
#include <RTI/NullFederateAmbassador.h>

#include <ostream>

#include "Version.h"

namespace rti1516e
{
// ---------------------------------------------------------------- Exception
Exception::Exception() {}
Exception::Exception(Exception const&) {}
Exception& Exception::operator=(Exception const&) { return *this; }
Exception::~Exception() {}

std::wostream& operator<<(std::wostream& os, Exception const& e)
{
   return os << e.what();
}

// ------------------------------------------------------ Ambassador bases
RTIambassador::RTIambassador() RTI_NOEXCEPT {}
RTIambassador::~RTIambassador() {}

FederateAmbassador::FederateAmbassador() {}
FederateAmbassador::~FederateAmbassador() RTI_NOEXCEPT {}

// ---------------------------------------------------------------- RangeBounds
RangeBounds::RangeBounds() : _lowerBound(0), _upperBound(0) {}
RangeBounds::RangeBounds(unsigned long lowerBound, unsigned long upperBound)
   : _lowerBound(lowerBound), _upperBound(upperBound) {}
RangeBounds::~RangeBounds() RTI_NOEXCEPT {}
RangeBounds::RangeBounds(RangeBounds const& rhs)
   : _lowerBound(rhs._lowerBound), _upperBound(rhs._upperBound) {}
RangeBounds& RangeBounds::operator=(RangeBounds const& rhs)
{
   _lowerBound = rhs._lowerBound;
   _upperBound = rhs._upperBound;
   return *this;
}
unsigned long RangeBounds::getLowerBound() const { return _lowerBound; }
unsigned long RangeBounds::getUpperBound() const { return _upperBound; }
void RangeBounds::setLowerBound(unsigned long lowerBound) { _lowerBound = lowerBound; }
void RangeBounds::setUpperBound(unsigned long upperBound) { _upperBound = upperBound; }

// ---------------------------------------------------------------- Typedefs
FederateRestoreStatus::FederateRestoreStatus(FederateHandle const& thePreHandle,
                                             FederateHandle const& thePostHandle,
                                             RestoreStatus theStatus)
   : preRestoreHandle(thePreHandle), postRestoreHandle(thePostHandle), status(theStatus) {}

FederationExecutionInformation::FederationExecutionInformation(
   std::wstring const& theFederationExecutionName,
   std::wstring const& theLogicalTimeImplementationName)
   : federationExecutionName(theFederationExecutionName),
     logicalTimeImplementationName(theLogicalTimeImplementationName) {}

SupplementalReflectInfo::SupplementalReflectInfo()
   : hasProducingFederate(false), hasSentRegions(false) {}
SupplementalReflectInfo::SupplementalReflectInfo(FederateHandle const& f)
   : hasProducingFederate(true), hasSentRegions(false), producingFederate(f) {}
SupplementalReflectInfo::SupplementalReflectInfo(RegionHandleSet const& r)
   : hasProducingFederate(false), hasSentRegions(true), sentRegions(r) {}
SupplementalReflectInfo::SupplementalReflectInfo(FederateHandle const& f, RegionHandleSet const& r)
   : hasProducingFederate(true), hasSentRegions(true), producingFederate(f), sentRegions(r) {}

SupplementalReceiveInfo::SupplementalReceiveInfo()
   : hasProducingFederate(false), hasSentRegions(false) {}
SupplementalReceiveInfo::SupplementalReceiveInfo(FederateHandle const& f)
   : hasProducingFederate(true), hasSentRegions(false), producingFederate(f) {}
SupplementalReceiveInfo::SupplementalReceiveInfo(RegionHandleSet const& r)
   : hasProducingFederate(false), hasSentRegions(true), sentRegions(r) {}
SupplementalReceiveInfo::SupplementalReceiveInfo(FederateHandle const& f, RegionHandleSet const& r)
   : hasProducingFederate(true), hasSentRegions(true), producingFederate(f), sentRegions(r) {}

SupplementalRemoveInfo::SupplementalRemoveInfo() : hasProducingFederate(false) {}
SupplementalRemoveInfo::SupplementalRemoveInfo(FederateHandle const& f)
   : hasProducingFederate(true), producingFederate(f) {}

// ------------------------------------------------------- Logical time bases
LogicalTime::~LogicalTime() RTI_NOEXCEPT {}
LogicalTimeInterval::~LogicalTimeInterval() RTI_NOEXCEPT {}
LogicalTimeFactory::~LogicalTimeFactory() RTI_NOEXCEPT {}

std::wostream& operator<<(std::wostream& os, LogicalTime const& t)
{
   return os << t.toString();
}

std::wostream& operator<<(std::wostream& os, LogicalTimeInterval const& i)
{
   return os << i.toString();
}

// HLAlogicalTimeFactoryFactory: time/StandardLogicalTimes.cpp

// ---------------------------------------------------------------- RTI info
std::wstring rtiName() { return HLA_MQTT_RTI_NAME; }
std::wstring rtiVersion() { return HLA_MQTT_RTI_VERSION; }
}
