// Standard logical time representations (RTI/time/*.h):
// HLAinteger64Time/Interval/TimeFactory and HLAfloat64Time/Interval/TimeFactory,
// plus HLAlogicalTimeFactoryFactory.
//
// IEEE 1516.1-2010 (standard time representations):
//   HLAinteger64Time : HLAinteger64BE, initial 0, final 2^63-1, epsilon 1
//   HLAfloat64Time   : HLAfloat64BE,   initial 0, final DBL_MAX, epsilon = smallest positive double
// Times and intervals are non-negative: an arithmetic result outside
// [initial, final] raises IllegalTimeArithmetic.

#include <RTI/time/HLAfloat64Interval.h>
#include <RTI/time/HLAfloat64Time.h>
#include <RTI/time/HLAfloat64TimeFactory.h>
#include <RTI/time/HLAinteger64Interval.h>
#include <RTI/time/HLAinteger64Time.h>
#include <RTI/time/HLAinteger64TimeFactory.h>
#include <RTI/VariableLengthData.h>

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>

namespace rti1516e
{
namespace
{
const size_t kEncodedLength = 8;

void putBE64(void* out, uint64_t u)
{
   unsigned char* p = static_cast<unsigned char*>(out);
   for (int i = 7; i >= 0; --i, u >>= 8)
      p[i] = static_cast<unsigned char>(u & 0xFF);
}

uint64_t getBE64(void const* in)
{
   unsigned char const* p = static_cast<unsigned char const*>(in);
   uint64_t u = 0;
   for (int i = 0; i < 8; ++i)
      u = (u << 8) | p[i];
   return u;
}

struct IntegerPolicy
{
   typedef Integer64 T;
   static T initial() { return 0; }
   static T finalValue() { return std::numeric_limits<Integer64>::max(); }
   static T zero() { return 0; }
   static T epsilon() { return 1; }
   static uint64_t toBits(T v) { return uint64_t(v); }
   static T fromBits(uint64_t u) { return T(u); }
   static std::wstring str(T v) { return std::to_wstring(v); }
   static bool valid(T v) { return v >= 0; }
   // Result in [0, final], no overflow.
   static bool add(T a, T b, T& r)
   {
      if (b > 0 && a > finalValue() - b)
         return false;
      r = a + b;
      return r >= 0;
   }
   static bool sub(T a, T b, T& r)
   {
      if (b < 0 && a > finalValue() + b)
         return false;
      r = a - b;
      return r >= 0;
   }
};

struct FloatPolicy
{
   typedef double T;
   static T initial() { return 0.0; }
   static T finalValue() { return DBL_MAX; }
   static T zero() { return 0.0; }
   static T epsilon() { return std::numeric_limits<double>::denorm_min(); }
   static uint64_t toBits(T v)
   {
      uint64_t u;
      std::memcpy(&u, &v, 8);
      return u;
   }
   static T fromBits(uint64_t u)
   {
      T v;
      std::memcpy(&v, &u, 8);
      return v;
   }
   static std::wstring str(T v)
   {
      std::wostringstream os;
      os.precision(17);
      os << v;
      return os.str();
   }
   static bool valid(T v) { return v >= 0.0 && v <= DBL_MAX; } // false for NaN
   // T + I > T and T - I < T for every I > 0 (IEEE 1516.1-2010 12.1): when the
   // increment is lost in rounding, the result is the next representable value.
   static bool add(T a, T b, T& r)
   {
      r = a + b;
      if (b > 0.0 && r <= a)
         r = std::nextafter(a, std::numeric_limits<double>::infinity());
      return valid(r);
   }
   static bool sub(T a, T b, T& r)
   {
      r = a - b;
      if (b > 0.0 && r >= a)
         r = std::nextafter(a, -std::numeric_limits<double>::infinity());
      return valid(r);
   }
};

template <class Policy>
VariableLengthData encodeValue(typename Policy::T v)
{
   unsigned char bytes[kEncodedLength];
   putBE64(bytes, Policy::toBits(v));
   return VariableLengthData(bytes, kEncodedLength);
}

template <class Policy, class CouldNotEncodeT>
size_t encodeValue(typename Policy::T v, void* buffer, size_t bufferSize, wchar_t const* type)
{
   if (buffer == nullptr || bufferSize < kEncodedLength)
      throw CouldNotEncodeT(std::wstring(type) + L": buffer smaller than 8 octets");
   putBE64(buffer, Policy::toBits(v));
   return kEncodedLength;
}

template <class Policy>
typename Policy::T decodeValue(void const* buffer, size_t size, wchar_t const* type)
{
   if (buffer == nullptr || size < kEncodedLength)
      throw CouldNotDecode(std::wstring(type) + L": expected 8 octets, got " + std::to_wstring(size));
   typename Policy::T const v = Policy::fromBits(getBE64(buffer));
   if (!Policy::valid(v))
      throw CouldNotDecode(std::wstring(type) + L": value out of range");
   return v;
}

template <class Target, class Exc, class Source>
Target const& castTo(Source const& value, wchar_t const* expected)
{
   Target const* t = dynamic_cast<Target const*>(&value);
   if (t == nullptr)
      throw Exc(std::wstring(L"expected an ") + expected + L", got " + value.implementationName());
   return *t;
}
} // namespace

#define HLA_WIDEN2(x) L##x
#define HLA_WIDEN(x) HLA_WIDEN2(x)

// Implementation classes forward-declared by the headers.
class HLAinteger64TimeImpl
{
public:
   explicit HLAinteger64TimeImpl(Integer64 v) : value(v) {}
   Integer64 value;
};
class HLAinteger64IntervalImpl
{
public:
   explicit HLAinteger64IntervalImpl(Integer64 v) : value(v) {}
   Integer64 value;
};
class HLAfloat64TimeImpl
{
public:
   explicit HLAfloat64TimeImpl(double v) : value(v) {}
   double value;
};
class HLAfloat64IntervalImpl
{
public:
   explicit HLAfloat64IntervalImpl(double v) : value(v) {}
   double value;
};

// ============================================================ time / interval
// Members common to HLAinteger64Time and HLAfloat64Time.
#define HLA_TIME_DEFINITION(Time, Interval, Policy, ValueT, NameConst)                         \
   Time::Time() : _impl(new Time##Impl(Policy::initial())) {}                                  \
   Time::Time(Time const& value) : LogicalTime(value), _impl(new Time##Impl(value._impl->value)) {} \
   Time::Time(LogicalTime const& value)                                                         \
      : _impl(new Time##Impl(castTo<Time, InvalidLogicalTime>(value, HLA_WIDEN(#Time))._impl->value)) {} \
   Time::~Time() RTI_NOEXCEPT { delete _impl; }                                                 \
   void Time::setInitial() { _impl->value = Policy::initial(); }                                \
   bool Time::isInitial() const { return _impl->value == Policy::initial(); }                   \
   void Time::setFinal() { _impl->value = Policy::finalValue(); }                                    \
   bool Time::isFinal() const { return _impl->value == Policy::finalValue(); }                       \
   LogicalTime& Time::operator=(LogicalTime const& value) RTI_THROW((InvalidLogicalTime))       \
   {                                                                                            \
      _impl->value = castTo<Time, InvalidLogicalTime>(value, NameConst.c_str())._impl->value;   \
      return *this;                                                                             \
   }                                                                                            \
   Time& Time::operator=(Time const& value) RTI_THROW((InvalidLogicalTime))                     \
   {                                                                                            \
      _impl->value = value._impl->value;                                                        \
      return *this;                                                                             \
   }                                                                                            \
   LogicalTime& Time::operator+=(LogicalTimeInterval const& addend)                             \
      RTI_THROW((IllegalTimeArithmetic, InvalidLogicalTimeInterval))                            \
   {                                                                                            \
      ValueT r;                                                                                 \
      ValueT const b =                                                                          \
         castTo<Interval, InvalidLogicalTimeInterval>(addend, HLA_WIDEN(#Interval)).getInterval();     \
      if (!Policy::add(_impl->value, b, r))                                                     \
         throw IllegalTimeArithmetic(toString() + L" + " + addend.toString() + L" out of range"); \
      _impl->value = r;                                                                         \
      return *this;                                                                             \
   }                                                                                            \
   LogicalTime& Time::operator-=(LogicalTimeInterval const& subtrahend)                         \
      RTI_THROW((IllegalTimeArithmetic, InvalidLogicalTimeInterval))                            \
   {                                                                                            \
      ValueT r;                                                                                 \
      ValueT const b =                                                                          \
         castTo<Interval, InvalidLogicalTimeInterval>(subtrahend, HLA_WIDEN(#Interval)).getInterval(); \
      if (!Policy::sub(_impl->value, b, r))                                                     \
         throw IllegalTimeArithmetic(toString() + L" - " + subtrahend.toString() + L" out of range"); \
      _impl->value = r;                                                                         \
      return *this;                                                                             \
   }                                                                                            \
   bool Time::operator>(LogicalTime const& value) const RTI_THROW((InvalidLogicalTime))         \
   {                                                                                            \
      return _impl->value > castTo<Time, InvalidLogicalTime>(value, HLA_WIDEN(#Time))._impl->value;    \
   }                                                                                            \
   bool Time::operator<(LogicalTime const& value) const RTI_THROW((InvalidLogicalTime))         \
   {                                                                                            \
      return _impl->value < castTo<Time, InvalidLogicalTime>(value, HLA_WIDEN(#Time))._impl->value;    \
   }                                                                                            \
   bool Time::operator==(LogicalTime const& value) const RTI_THROW((InvalidLogicalTime))        \
   {                                                                                            \
      return _impl->value == castTo<Time, InvalidLogicalTime>(value, HLA_WIDEN(#Time))._impl->value;   \
   }                                                                                            \
   bool Time::operator>=(LogicalTime const& value) const RTI_THROW((InvalidLogicalTime))        \
   {                                                                                            \
      return _impl->value >= castTo<Time, InvalidLogicalTime>(value, HLA_WIDEN(#Time))._impl->value;   \
   }                                                                                            \
   bool Time::operator<=(LogicalTime const& value) const RTI_THROW((InvalidLogicalTime))        \
   {                                                                                            \
      return _impl->value <= castTo<Time, InvalidLogicalTime>(value, HLA_WIDEN(#Time))._impl->value;   \
   }                                                                                            \
   VariableLengthData Time::encode() const { return encodeValue<Policy>(_impl->value); }       \
   size_t Time::encode(void* buffer, size_t bufferSize) const RTI_THROW((CouldNotEncode))      \
   {                                                                                            \
      return encodeValue<Policy, CouldNotEncode>(_impl->value, buffer, bufferSize, HLA_WIDEN(#Time));  \
   }                                                                                            \
   size_t Time::encodedLength() const { return kEncodedLength; }                                \
   void Time::decode(VariableLengthData const& data) RTI_THROW((InternalError, CouldNotDecode)) \
   {                                                                                            \
      _impl->value = decodeValue<Policy>(data.data(), data.size(), HLA_WIDEN(#Time));                  \
   }                                                                                            \
   void Time::decode(void* buffer, size_t bufferSize) RTI_THROW((InternalError, CouldNotDecode)) \
   {                                                                                            \
      _impl->value = decodeValue<Policy>(buffer, bufferSize, HLA_WIDEN(#Time));                        \
   }                                                                                            \
   std::wstring Time::toString() const { return Policy::str(_impl->value); }                    \
   std::wstring Time::implementationName() const { return NameConst; }                          \
   ValueT Time::getTime() const { return _impl->value; }                                        \
   void Time::setTime(ValueT value) { _impl->value = value; }                                   \
   Time::operator ValueT() const { return _impl->value; }

#define HLA_INTERVAL_DEFINITION(Interval, Time, Policy, ValueT, NameConst)                      \
   Interval::Interval() : _impl(new Interval##Impl(Policy::zero())) {}                          \
   Interval::Interval(Interval const& rhs)                                                      \
      : LogicalTimeInterval(rhs), _impl(new Interval##Impl(rhs._impl->value)) {}               \
   Interval::Interval(LogicalTimeInterval const& rhs)                                           \
      : _impl(new Interval##Impl(                                                               \
           castTo<Interval, InvalidLogicalTimeInterval>(rhs, HLA_WIDEN(#Interval))._impl->value)) {}   \
   Interval::~Interval() RTI_NOEXCEPT { delete _impl; }                                         \
   void Interval::setZero() { _impl->value = Policy::zero(); }                                  \
   bool Interval::isZero() const { return _impl->value == Policy::zero(); }                     \
   void Interval::setEpsilon() { _impl->value = Policy::epsilon(); }                            \
   bool Interval::isEpsilon() const { return _impl->value == Policy::epsilon(); }               \
   LogicalTimeInterval& Interval::operator=(LogicalTimeInterval const& value)                   \
      RTI_THROW((InvalidLogicalTimeInterval))                                                   \
   {                                                                                            \
      _impl->value = castTo<Interval, InvalidLogicalTimeInterval>(value, HLA_WIDEN(#Interval))._impl->value; \
      return *this;                                                                             \
   }                                                                                            \
   Interval& Interval::operator=(Interval const& value) RTI_THROW((InvalidLogicalTimeInterval)) \
   {                                                                                            \
      _impl->value = value._impl->value;                                                        \
      return *this;                                                                             \
   }                                                                                            \
   LogicalTimeInterval& Interval::operator+=(LogicalTimeInterval const& addend)                 \
      RTI_THROW((IllegalTimeArithmetic, InvalidLogicalTimeInterval))                            \
   {                                                                                            \
      ValueT r;                                                                                 \
      ValueT const b =                                                                          \
         castTo<Interval, InvalidLogicalTimeInterval>(addend, HLA_WIDEN(#Interval))._impl->value;      \
      if (!Policy::add(_impl->value, b, r))                                                     \
         throw IllegalTimeArithmetic(toString() + L" + " + addend.toString() + L" out of range"); \
      _impl->value = r;                                                                         \
      return *this;                                                                             \
   }                                                                                            \
   LogicalTimeInterval& Interval::operator-=(LogicalTimeInterval const& subtrahend)             \
      RTI_THROW((IllegalTimeArithmetic, InvalidLogicalTimeInterval))                            \
   {                                                                                            \
      ValueT r;                                                                                 \
      ValueT const b =                                                                          \
         castTo<Interval, InvalidLogicalTimeInterval>(subtrahend, HLA_WIDEN(#Interval))._impl->value;  \
      if (!Policy::sub(_impl->value, b, r))                                                     \
         throw IllegalTimeArithmetic(toString() + L" - " + subtrahend.toString() + L" out of range"); \
      _impl->value = r;                                                                         \
      return *this;                                                                             \
   }                                                                                            \
   bool Interval::operator>(LogicalTimeInterval const& value) const                             \
      RTI_THROW((InvalidLogicalTimeInterval))                                                   \
   {                                                                                            \
      return _impl->value > castTo<Interval, InvalidLogicalTimeInterval>(value, HLA_WIDEN(#Interval))._impl->value; \
   }                                                                                            \
   bool Interval::operator<(LogicalTimeInterval const& value) const                             \
      RTI_THROW((InvalidLogicalTimeInterval))                                                   \
   {                                                                                            \
      return _impl->value < castTo<Interval, InvalidLogicalTimeInterval>(value, HLA_WIDEN(#Interval))._impl->value; \
   }                                                                                            \
   bool Interval::operator==(LogicalTimeInterval const& value) const                            \
      RTI_THROW((InvalidLogicalTimeInterval))                                                   \
   {                                                                                            \
      return _impl->value == castTo<Interval, InvalidLogicalTimeInterval>(value, HLA_WIDEN(#Interval))._impl->value; \
   }                                                                                            \
   bool Interval::operator>=(LogicalTimeInterval const& value) const                            \
      RTI_THROW((InvalidLogicalTimeInterval))                                                   \
   {                                                                                            \
      return _impl->value >= castTo<Interval, InvalidLogicalTimeInterval>(value, HLA_WIDEN(#Interval))._impl->value; \
   }                                                                                            \
   bool Interval::operator<=(LogicalTimeInterval const& value) const                            \
      RTI_THROW((InvalidLogicalTimeInterval))                                                   \
   {                                                                                            \
      return _impl->value <= castTo<Interval, InvalidLogicalTimeInterval>(value, HLA_WIDEN(#Interval))._impl->value; \
   }                                                                                            \
   void Interval::setToDifference(LogicalTime const& minuend, LogicalTime const& subtrahend)    \
      RTI_THROW((IllegalTimeArithmetic, InvalidLogicalTime))                                    \
   {                                                                                            \
      ValueT r;                                                                                 \
      ValueT const a = castTo<Time, InvalidLogicalTime>(minuend, HLA_WIDEN(#Time)).getTime();          \
      ValueT const b = castTo<Time, InvalidLogicalTime>(subtrahend, HLA_WIDEN(#Time)).getTime();       \
      if (!Policy::sub(a, b, r))                                                                \
         throw IllegalTimeArithmetic(minuend.toString() + L" - " + subtrahend.toString() +      \
                                     L" is not a valid interval");                              \
      _impl->value = r;                                                                         \
   }                                                                                            \
   VariableLengthData Interval::encode() const { return encodeValue<Policy>(_impl->value); }   \
   size_t Interval::encode(void* buffer, size_t bufferSize) const RTI_THROW((CouldNotEncode))  \
   {                                                                                            \
      return encodeValue<Policy, CouldNotEncode>(_impl->value, buffer, bufferSize, HLA_WIDEN(#Interval)); \
   }                                                                                            \
   size_t Interval::encodedLength() const { return kEncodedLength; }                            \
   void Interval::decode(VariableLengthData const& data) RTI_THROW((InternalError, CouldNotDecode)) \
   {                                                                                            \
      _impl->value = decodeValue<Policy>(data.data(), data.size(), HLA_WIDEN(#Interval));              \
   }                                                                                            \
   void Interval::decode(void* buffer, size_t bufferSize) RTI_THROW((InternalError, CouldNotDecode)) \
   {                                                                                            \
      _impl->value = decodeValue<Policy>(buffer, bufferSize, HLA_WIDEN(#Interval));                    \
   }                                                                                            \
   std::wstring Interval::toString() const { return Policy::str(_impl->value); }                \
   std::wstring Interval::implementationName() const { return NameConst; }                      \
   ValueT Interval::getInterval() const { return _impl->value; }                                \
   void Interval::setInterval(ValueT value) { _impl->value = value; }                           \
   Interval::operator ValueT() const { return _impl->value; }

#define HLA_FACTORY_DEFINITION(Factory, Time, Interval, Policy, ValueT, NameConst)              \
   Factory::Factory() {}                                                                        \
   Factory::~Factory() RTI_NOEXCEPT {}                                                          \
   RTI_UNIQUE_PTR<LogicalTime> Factory::makeInitial() RTI_THROW((InternalError))                \
   {                                                                                            \
      return RTI_UNIQUE_PTR<LogicalTime>(new Time(Policy::initial()));                          \
   }                                                                                            \
   RTI_UNIQUE_PTR<LogicalTime> Factory::makeFinal() RTI_THROW((InternalError))                  \
   {                                                                                            \
      return RTI_UNIQUE_PTR<LogicalTime>(new Time(Policy::finalValue()));                            \
   }                                                                                            \
   RTI_UNIQUE_PTR<LogicalTimeInterval> Factory::makeZero() RTI_THROW((InternalError))           \
   {                                                                                            \
      return RTI_UNIQUE_PTR<LogicalTimeInterval>(new Interval(Policy::zero()));                 \
   }                                                                                            \
   RTI_UNIQUE_PTR<LogicalTimeInterval> Factory::makeEpsilon() RTI_THROW((InternalError))        \
   {                                                                                            \
      return RTI_UNIQUE_PTR<LogicalTimeInterval>(new Interval(Policy::epsilon()));              \
   }                                                                                            \
   RTI_UNIQUE_PTR<Time> Factory::makeLogicalTime(ValueT value) RTI_THROW((InternalError))       \
   {                                                                                            \
      return RTI_UNIQUE_PTR<Time>(new Time(value));                                             \
   }                                                                                            \
   RTI_UNIQUE_PTR<Interval> Factory::makeLogicalTimeInterval(ValueT value) RTI_THROW((InternalError)) \
   {                                                                                            \
      return RTI_UNIQUE_PTR<Interval>(new Interval(value));                                     \
   }                                                                                            \
   RTI_UNIQUE_PTR<LogicalTime> Factory::decodeLogicalTime(VariableLengthData const& encoded)    \
      RTI_THROW((InternalError, CouldNotDecode))                                                \
   {                                                                                            \
      RTI_UNIQUE_PTR<LogicalTime> t(new Time());                                                \
      t->decode(encoded);                                                                       \
      return t;                                                                                 \
   }                                                                                            \
   RTI_UNIQUE_PTR<LogicalTime> Factory::decodeLogicalTime(void* buffer, size_t bufferSize)      \
      RTI_THROW((InternalError, CouldNotDecode))                                                \
   {                                                                                            \
      RTI_UNIQUE_PTR<LogicalTime> t(new Time());                                                \
      t->decode(buffer, bufferSize);                                                            \
      return t;                                                                                 \
   }                                                                                            \
   RTI_UNIQUE_PTR<LogicalTimeInterval> Factory::decodeLogicalTimeInterval(                      \
      VariableLengthData const& encoded) RTI_THROW((InternalError, CouldNotDecode))             \
   {                                                                                            \
      RTI_UNIQUE_PTR<LogicalTimeInterval> i(new Interval());                                    \
      i->decode(encoded);                                                                       \
      return i;                                                                                 \
   }                                                                                            \
   RTI_UNIQUE_PTR<LogicalTimeInterval> Factory::decodeLogicalTimeInterval(void* buffer,         \
                                                                          size_t bufferSize)    \
      RTI_THROW((InternalError, CouldNotDecode))                                                \
   {                                                                                            \
      RTI_UNIQUE_PTR<LogicalTimeInterval> i(new Interval());                                    \
      i->decode(buffer, bufferSize);                                                            \
      return i;                                                                                 \
   }                                                                                            \
   std::wstring Factory::getName() const { return NameConst; }

// ------------------------------------------------------------ HLAinteger64
HLAinteger64Time::HLAinteger64Time(Integer64 value) : _impl(new HLAinteger64TimeImpl(value)) {}
HLAinteger64Interval::HLAinteger64Interval(Integer64 value) : _impl(new HLAinteger64IntervalImpl(value)) {}

HLA_TIME_DEFINITION(HLAinteger64Time, HLAinteger64Interval, IntegerPolicy, Integer64, HLAinteger64TimeName)
HLA_INTERVAL_DEFINITION(HLAinteger64Interval, HLAinteger64Time, IntegerPolicy, Integer64, HLAinteger64TimeName)
HLA_FACTORY_DEFINITION(HLAinteger64TimeFactory, HLAinteger64Time, HLAinteger64Interval, IntegerPolicy,
                       Integer64, HLAinteger64TimeName)

// -------------------------------------------------------------- HLAfloat64
HLAfloat64Time::HLAfloat64Time(double const& value) : _impl(new HLAfloat64TimeImpl(value)) {}
HLAfloat64Interval::HLAfloat64Interval(double value) : _impl(new HLAfloat64IntervalImpl(value)) {}

HLA_TIME_DEFINITION(HLAfloat64Time, HLAfloat64Interval, FloatPolicy, double, HLAfloat64TimeName)
HLA_INTERVAL_DEFINITION(HLAfloat64Interval, HLAfloat64Time, FloatPolicy, double, HLAfloat64TimeName)
HLA_FACTORY_DEFINITION(HLAfloat64TimeFactory, HLAfloat64Time, HLAfloat64Interval, FloatPolicy, double,
                       HLAfloat64TimeName)

// ------------------------------------------- HLAlogicalTimeFactoryFactory
// Empty name: HLAfloat64Time (default time representation of this RTI).
// Unknown name: null pointer, as specified for LogicalTimeFactoryFactory.
RTI_UNIQUE_PTR<LogicalTimeFactory>
HLAlogicalTimeFactoryFactory::makeLogicalTimeFactory(std::wstring const& implementationName)
{
   if (implementationName == HLAinteger64TimeName)
      return RTI_UNIQUE_PTR<LogicalTimeFactory>(new HLAinteger64TimeFactory());
   if (implementationName.empty() || implementationName == HLAfloat64TimeName)
      return RTI_UNIQUE_PTR<LogicalTimeFactory>(new HLAfloat64TimeFactory());
   return RTI_UNIQUE_PTR<LogicalTimeFactory>();
}
} // namespace rti1516e
