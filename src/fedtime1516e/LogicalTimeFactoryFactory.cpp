// fedtime1516e: the "fedtime" library of IEEE 1516.1-2010.
// LogicalTimeFactoryFactory is declared RTI_EXPORT_FEDTIME, so federates
// import it from this separate DLL. As suggested by the standard header, it
// forwards to HLAlogicalTimeFactoryFactory (rti1516e), which provides
// HLAinteger64Time and HLAfloat64Time.

#include <RTI/LogicalTimeFactory.h>

namespace rti1516e
{
RTI_UNIQUE_PTR<LogicalTimeFactory>
LogicalTimeFactoryFactory::makeLogicalTimeFactory(std::wstring const& implementationName)
{
   return HLAlogicalTimeFactoryFactory::makeLogicalTimeFactory(implementationName);
}
} // namespace rti1516e
