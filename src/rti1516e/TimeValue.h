// Comparable value of a standard logical time or interval (HLAinteger64Time
// or HLAfloat64Time), used by the time management engine.
//
// Bound: lower bound on the timestamps of the future TSO messages of a
// regulating federate. `open` = strictly greater than `value` (zero lookahead
// after a grant through TAR/NER, IEEE 1516.1-2010 8.1.1).
#pragma once

#include <RTI/RTI1516.h>
#include <RTI/time/HLAfloat64Interval.h>
#include <RTI/time/HLAfloat64Time.h>
#include <RTI/time/HLAinteger64Interval.h>
#include <RTI/time/HLAinteger64Time.h>

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

namespace hla_mqtt
{
struct TimeValue
{
   bool integer = false;
   bool infinite = false;
   std::int64_t i = 0;
   double d = 0;

   static TimeValue zero(bool isInteger)
   {
      TimeValue t;
      t.integer = isInteger;
      return t;
   }
   static TimeValue inf(bool isInteger)
   {
      TimeValue t;
      t.integer = isInteger;
      t.infinite = true;
      return t;
   }

   // Throws InvalidLogicalTime for another time representation.
   static TimeValue of(rti1516e::LogicalTime const& t, bool isInteger)
   {
      TimeValue v;
      v.integer = isInteger;
      if (isInteger)
      {
         auto p = dynamic_cast<rti1516e::HLAinteger64Time const*>(&t);
         if (!p)
            throw rti1516e::InvalidLogicalTime(L"HLAinteger64Time expected, got " + t.implementationName());
         v.i = p->getTime();
      }
      else
      {
         auto p = dynamic_cast<rti1516e::HLAfloat64Time const*>(&t);
         if (!p)
            throw rti1516e::InvalidLogicalTime(L"HLAfloat64Time expected, got " + t.implementationName());
         v.d = p->getTime();
      }
      return v;
   }

   // Throws InvalidLookahead for another representation or a negative value.
   static TimeValue ofInterval(rti1516e::LogicalTimeInterval const& t, bool isInteger)
   {
      TimeValue v;
      v.integer = isInteger;
      if (isInteger)
      {
         auto p = dynamic_cast<rti1516e::HLAinteger64Interval const*>(&t);
         if (!p)
            throw rti1516e::InvalidLookahead(L"HLAinteger64Interval expected");
         v.i = p->getInterval();
      }
      else
      {
         auto p = dynamic_cast<rti1516e::HLAfloat64Interval const*>(&t);
         if (!p)
            throw rti1516e::InvalidLookahead(L"HLAfloat64Interval expected");
         v.d = p->getInterval();
      }
      if (v.isNegative())
         throw rti1516e::InvalidLookahead(L"negative lookahead");
      return v;
   }

   bool isNegative() const { return !infinite && (integer ? i < 0 : !(d >= 0)); }
   bool isZero() const { return !infinite && (integer ? i == 0 : d == 0); }

   int compare(TimeValue const& o) const
   {
      if (infinite || o.infinite)
         return infinite == o.infinite ? 0 : (infinite ? 1 : -1);
      if (integer)
         return i < o.i ? -1 : (i > o.i ? 1 : 0);
      return d < o.d ? -1 : (d > o.d ? 1 : 0);
   }
   bool operator<(TimeValue const& o) const { return compare(o) < 0; }
   bool operator<=(TimeValue const& o) const { return compare(o) <= 0; }
   bool operator>(TimeValue const& o) const { return compare(o) > 0; }
   bool operator>=(TimeValue const& o) const { return compare(o) >= 0; }
   bool operator==(TimeValue const& o) const { return compare(o) == 0; }
   bool operator!=(TimeValue const& o) const { return compare(o) != 0; }

   // time + interval, saturated at the final time
   TimeValue plus(TimeValue const& interval) const
   {
      if (infinite || interval.infinite)
         return inf(integer);
      TimeValue r = *this;
      if (integer)
         r.i = (interval.i > 0 && i > std::numeric_limits<std::int64_t>::max() - interval.i) ? std::numeric_limits<std::int64_t>::max()
                                                                                              : i + interval.i;
      else
         r.d = d + interval.d > DBL_MAX ? DBL_MAX : d + interval.d;
      return r;
   }

   // Smallest representable value greater than this one (saturated).
   TimeValue next() const
   {
      if (infinite)
         return *this;
      TimeValue r = *this;
      if (integer)
         r.i = i == std::numeric_limits<std::int64_t>::max() ? i : i + 1;
      else
         r.d = d >= DBL_MAX ? d : std::nextafter(d, DBL_MAX);
      return r;
   }

   std::shared_ptr<rti1516e::LogicalTime> toTime() const
   {
      if (integer)
         return std::make_shared<rti1516e::HLAinteger64Time>(infinite ? std::numeric_limits<std::int64_t>::max() : i);
      return std::make_shared<rti1516e::HLAfloat64Time>(infinite ? DBL_MAX : d);
   }

   void toInterval(rti1516e::LogicalTimeInterval& out) const
   {
      if (integer)
         dynamic_cast<rti1516e::HLAinteger64Interval&>(out).setInterval(i);
      else
         dynamic_cast<rti1516e::HLAfloat64Interval&>(out).setInterval(d);
   }

   void assignTo(rti1516e::LogicalTime& out) const
   {
      if (integer)
         dynamic_cast<rti1516e::HLAinteger64Time&>(out).setTime(infinite ? std::numeric_limits<std::int64_t>::max() : i);
      else
         dynamic_cast<rti1516e::HLAfloat64Time&>(out).setTime(infinite ? DBL_MAX : d);
   }

   // Text form for the JSON time records: "inf", decimal integer, or the
   // 16 hex digits of the IEEE 754 bits (exact).
   std::string str() const
   {
      if (infinite)
         return "inf";
      if (integer)
         return std::to_string(i);
      std::uint64_t bits;
      std::memcpy(&bits, &d, 8);
      char b[17];
      std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(bits));
      return b;
   }
   static TimeValue parse(std::string const& s, bool isInteger)
   {
      if (s == "inf" || s.empty())
         return inf(isInteger);
      TimeValue t;
      t.integer = isInteger;
      if (isInteger)
         t.i = std::strtoll(s.c_str(), nullptr, 10);
      else
      {
         std::uint64_t bits = std::strtoull(s.c_str(), nullptr, 16);
         std::memcpy(&t.d, &bits, 8);
      }
      return t;
   }
};

struct Bound
{
   TimeValue value;
   bool open = false; // strictly greater than value

   // Order on the guarantees: (t, closed) < (t, open).
   bool operator<(Bound const& o) const
   {
      int c = value.compare(o.value);
      return c < 0 || (c == 0 && !open && o.open);
   }
   // Every message with timestamp <= t is already received.
   bool beyond(TimeValue const& t) const { return value > t || (value == t && open); }
   // Every message with timestamp < t is already received.
   bool reaches(TimeValue const& t) const { return value >= t; }
};
} // namespace hla_mqtt
