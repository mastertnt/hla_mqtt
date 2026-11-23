// Implementation of the 9 standard handle classes (RTI/Handle.h).
// Binary layout imposed by the header: a single pointer `_impl`.
// Internal value: 64-bit unsigned integer, 0 = invalid.
// Wire encoding: 8 bytes, big-endian.

#include "HandleFriends.h"

#include <ostream>
#include <sstream>

namespace
{
void encodeU64(std::uint64_t v, unsigned char* out)
{
   for (int i = 7; i >= 0; --i)
   {
      out[i] = static_cast<unsigned char>(v & 0xFF);
      v >>= 8;
   }
}

std::uint64_t decodeU64(unsigned char const* in)
{
   std::uint64_t v = 0;
   for (int i = 0; i < 8; ++i)
      v = (v << 8) | in[i];
   return v;
}
}

#define HLA_MQTT_IMPLEMENT_HANDLE(Kind)                                          \
   class Kind##Implementation                                                    \
   {                                                                             \
   public:                                                                       \
      explicit Kind##Implementation(std::uint64_t v) : value(v) {}               \
      std::uint64_t value;                                                       \
   };                                                                            \
                                                                                 \
   Kind::Kind() : _impl(nullptr) {}                                              \
   Kind::~Kind() RTI_NOEXCEPT { delete _impl; }                                  \
   Kind::Kind(Kind const & rhs)                                                  \
      : _impl(rhs._impl ? new Kind##Implementation(*rhs._impl) : nullptr) {}     \
   Kind & Kind::operator=(Kind const & rhs)                                      \
   {                                                                             \
      if (this != &rhs)                                                          \
      {                                                                          \
         Kind##Implementation* copy =                                            \
            rhs._impl ? new Kind##Implementation(*rhs._impl) : nullptr;          \
         delete _impl;                                                           \
         _impl = copy;                                                           \
      }                                                                          \
      return *this;                                                              \
   }                                                                             \
   Kind::Kind(Kind##Implementation* impl) : _impl(impl) {}                       \
   Kind::Kind(VariableLengthData const & encodedValue) : _impl(nullptr)          \
   {                                                                             \
      if (encodedValue.size() != 8)                                              \
         throw CouldNotDecode(L"" #Kind ": expected 8 bytes");                   \
      std::uint64_t v =                                                          \
         decodeU64(static_cast<unsigned char const*>(encodedValue.data()));      \
      if (v != 0)                                                                \
         _impl = new Kind##Implementation(v);                                    \
   }                                                                             \
   bool Kind::isValid() const { return _impl != nullptr; }                       \
   bool Kind::operator==(Kind const & rhs) const                                 \
   {                                                                             \
      return Kind##Friend::value(*this) == Kind##Friend::value(rhs);             \
   }                                                                             \
   bool Kind::operator!=(Kind const & rhs) const { return !(*this == rhs); }     \
   bool Kind::operator<(Kind const & rhs) const                                  \
   {                                                                             \
      return Kind##Friend::value(*this) < Kind##Friend::value(rhs);              \
   }                                                                             \
   long Kind::hash() const                                                       \
   {                                                                             \
      std::uint64_t v = Kind##Friend::value(*this);                              \
      return static_cast<long>(v ^ (v >> 32));                                   \
   }                                                                             \
   VariableLengthData Kind::encode() const                                       \
   {                                                                             \
      VariableLengthData data;                                                   \
      encode(data);                                                              \
      return data;                                                               \
   }                                                                             \
   void Kind::encode(VariableLengthData & buffer) const                          \
   {                                                                             \
      unsigned char bytes[8];                                                    \
      encodeU64(Kind##Friend::value(*this), bytes);                              \
      buffer.setData(bytes, sizeof(bytes));                                      \
   }                                                                             \
   size_t Kind::encode(void* buffer, size_t bufferSize) const                    \
   {                                                                             \
      if (buffer == nullptr || bufferSize < 8)                                   \
         throw CouldNotEncode(L"" #Kind ": buffer too small");                   \
      encodeU64(Kind##Friend::value(*this), static_cast<unsigned char*>(buffer));\
      return 8;                                                                  \
   }                                                                             \
   size_t Kind::encodedLength() const { return 8; }                              \
   std::wstring Kind::toString() const                                           \
   {                                                                             \
      if (!_impl)                                                                \
         return L"" #Kind "(invalid)";                                           \
      std::wostringstream s;                                                     \
      s << L"" #Kind "(" << _impl->value << L")";                                \
      return s.str();                                                            \
   }                                                                             \
   const Kind##Implementation* Kind::getImplementation() const { return _impl; } \
   Kind##Implementation* Kind::getImplementation() { return _impl; }             \
   std::wostream & operator<<(std::wostream & os, Kind const & h)                \
   {                                                                             \
      return os << h.toString();                                                 \
   }                                                                             \
   Kind Kind##Friend::create(std::uint64_t value)                                \
   {                                                                             \
      return value == 0 ? Kind() : Kind(new Kind##Implementation(value));       \
   }                                                                             \
   std::uint64_t Kind##Friend::value(Kind const & handle)                        \
   {                                                                             \
      return handle._impl ? handle._impl->value : 0;                             \
   }

namespace rti1516e
{
HLA_MQTT_IMPLEMENT_HANDLE(FederateHandle)
HLA_MQTT_IMPLEMENT_HANDLE(ObjectClassHandle)
HLA_MQTT_IMPLEMENT_HANDLE(InteractionClassHandle)
HLA_MQTT_IMPLEMENT_HANDLE(ObjectInstanceHandle)
HLA_MQTT_IMPLEMENT_HANDLE(AttributeHandle)
HLA_MQTT_IMPLEMENT_HANDLE(ParameterHandle)
HLA_MQTT_IMPLEMENT_HANDLE(DimensionHandle)
HLA_MQTT_IMPLEMENT_HANDLE(MessageRetractionHandle)
HLA_MQTT_IMPLEMENT_HANDLE(RegionHandle)
}
