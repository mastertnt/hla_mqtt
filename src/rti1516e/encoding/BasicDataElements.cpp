// Basic data elements (RTI/encoding/BasicDataElements.h), EncoderException
// and the non-pure members of DataElement.
//
// Encodings (IEEE 1516.2-2010, table 24 and 4.13):
//   HLAinteger16/32/64BE|LE, HLAfloat32/64BE|LE : two's complement / IEEE 754
//   HLAoctet, HLAbyte, HLAASCIIchar             : 1 octet
//   HLAoctetPairBE|LE, HLAunicodeChar           : 2 octets (HLAunicodeChar = HLAoctetPairBE, UTF-16)
//   HLAboolean                                  : HLAinteger32BE, HLAfalse = 0, HLAtrue = 1
//   HLAASCIIstring                              : HLAvariableArray of HLAASCIIchar
//   HLAunicodeString                            : HLAvariableArray of HLAunicodeChar (UTF-16BE)

#include <RTI/encoding/BasicDataElements.h>
#include <RTI/encoding/DataElement.h>
#include <RTI/encoding/EncodingExceptions.h>

#include "EncodingHelpers.h"

#include <typeinfo>

namespace rti1516e
{
using namespace encoding;

// ---------------------------------------------------------- EncoderException
EncoderException::EncoderException(std::wstring const& message) RTI_NOEXCEPT : _msg(message) {}

std::wstring EncoderException::what() const RTI_NOEXCEPT
{
   return _msg;
}

// --------------------------------------------------------------- DataElement
DataElement::~DataElement() {}

bool DataElement::isSameTypeAs(DataElement const& inData) const
{
   return typeid(*this) == typeid(inData);
}

Integer64 DataElement::hash() const
{
   return hashEncoding(*this);
}

namespace
{
// ------------------------------------------------------------------ codecs
// Each codec: boundary, put (aligns, then appends) and get (aligns, reads,
// returns the index after the value).

template <typename T, typename U, bool BigEndian>
struct NumberCodec
{
   static unsigned int boundary() { return sizeof(T); }
   static void put(Buffer& b, T const& v)
   {
      U u;
      std::memcpy(&u, &v, sizeof(T));
      pad(b, sizeof(T));
      if (BigEndian)
         putBE<U>(b, u);
      else
         putLE<U>(b, u);
   }
   static size_t get(Buffer const& b, size_t index, T& v, wchar_t const* type)
   {
      index = alignAndCheck(b, index, sizeof(T), sizeof(T), type);
      U const u = BigEndian ? getBE<U>(b, index) : getLE<U>(b, index);
      std::memcpy(&v, &u, sizeof(T));
      return index + sizeof(T);
   }
};

template <typename T>
struct OctetCodec
{
   static unsigned int boundary() { return 1; }
   static void put(Buffer& b, T const& v) { b.push_back(Octet(v)); }
   static size_t get(Buffer const& b, size_t index, T& v, wchar_t const* type)
   {
      index = alignAndCheck(b, index, 1, 1, type);
      v = T(b[index]);
      return index + 1;
   }
};

struct BooleanCodec
{
   static unsigned int boundary() { return 4; }
   static void put(Buffer& b, bool const& v)
   {
      pad(b, 4);
      putBE<uint32_t>(b, v ? 1u : 0u);
   }
   static size_t get(Buffer const& b, size_t index, bool& v, wchar_t const* type)
   {
      index = alignAndCheck(b, index, 4, 4, type);
      v = getBE<uint32_t>(b, index) != 0;
      return index + 4;
   }
};

template <bool BigEndian>
struct OctetPairCodec
{
   static unsigned int boundary() { return 2; }
   static void put(Buffer& b, OctetPair const& v)
   {
      pad(b, 2);
      if (BigEndian)
      {
         b.push_back(v.first);
         b.push_back(v.second);
      }
      else
      {
         b.push_back(v.second);
         b.push_back(v.first);
      }
   }
   static size_t get(Buffer const& b, size_t index, OctetPair& v, wchar_t const* type)
   {
      index = alignAndCheck(b, index, 2, 2, type);
      if (BigEndian)
         v = OctetPair(b[index], b[index + 1]);
      else
         v = OctetPair(b[index + 1], b[index]);
      return index + 2;
   }
};

struct UnicodeCharCodec
{
   static unsigned int boundary() { return 2; }
   static void put(Buffer& b, wchar_t const& v)
   {
      pad(b, 2);
      putBE<uint16_t>(b, uint16_t(v));
   }
   static size_t get(Buffer const& b, size_t index, wchar_t& v, wchar_t const* type)
   {
      index = alignAndCheck(b, index, 2, 2, type);
      v = wchar_t(getBE<uint16_t>(b, index));
      return index + 2;
   }
};

struct ASCIIstringCodec
{
   static unsigned int boundary() { return 4; }
   static void put(Buffer& b, std::string const& v)
   {
      putCount(b, v.size());
      b.insert(b.end(), v.begin(), v.end());
   }
   static size_t get(Buffer const& b, size_t index, std::string& v, wchar_t const* type)
   {
      size_t const count = getCount(b, index, type);
      if (b.size() - index < count)
         fail(std::wstring(L"not enough data to decode ") + type);
      v.assign(reinterpret_cast<char const*>(b.data()) + index, count);
      return index + count;
   }
};

// UTF-16BE code units. wchar_t is 16 bits on Windows (already UTF-16) and
// 32 bits elsewhere (UTF-32: surrogate pairs are built and combined here).
struct UnicodeStringCodec
{
   static unsigned int boundary() { return 4; }
   static void put(Buffer& b, std::wstring const& v)
   {
      std::vector<uint16_t> units;
      units.reserve(v.size());
      for (wchar_t c : v)
      {
         uint32_t const cp = uint32_t(c);
         if (sizeof(wchar_t) > 2 && cp > 0xFFFF && cp <= 0x10FFFF)
         {
            uint32_t const x = cp - 0x10000;
            units.push_back(uint16_t(0xD800 + (x >> 10)));
            units.push_back(uint16_t(0xDC00 + (x & 0x3FF)));
         }
         else
            units.push_back(uint16_t(cp));
      }
      putCount(b, units.size());
      for (uint16_t u : units)
         putBE<uint16_t>(b, u);
   }
   static size_t get(Buffer const& b, size_t index, std::wstring& v, wchar_t const* type)
   {
      size_t const count = getCount(b, index, type);
      if ((b.size() - index) / 2 < count)
         fail(std::wstring(L"not enough data to decode ") + type);
      std::wstring out;
      out.reserve(count);
      for (size_t i = 0; i < count; ++i)
      {
         uint32_t u = getBE<uint16_t>(b, index + 2 * i);
         if (sizeof(wchar_t) > 2 && u >= 0xD800 && u < 0xDC00 && i + 1 < count)
         {
            uint32_t const low = getBE<uint16_t>(b, index + 2 * (i + 1));
            if (low >= 0xDC00 && low < 0xE000)
            {
               u = 0x10000 + ((u - 0xD800) << 10) + (low - 0xDC00);
               ++i;
            }
         }
         out.push_back(wchar_t(u));
      }
      v.swap(out);
      return index + 2 * count;
   }
};

using CodecHLAASCIIchar = OctetCodec<char>;
using CodecHLAASCIIstring = ASCIIstringCodec;
using CodecHLAboolean = BooleanCodec;
using CodecHLAbyte = OctetCodec<Octet>;
using CodecHLAfloat32BE = NumberCodec<float, uint32_t, true>;
using CodecHLAfloat32LE = NumberCodec<float, uint32_t, false>;
using CodecHLAfloat64BE = NumberCodec<double, uint64_t, true>;
using CodecHLAfloat64LE = NumberCodec<double, uint64_t, false>;
using CodecHLAinteger16LE = NumberCodec<Integer16, uint16_t, false>;
using CodecHLAinteger16BE = NumberCodec<Integer16, uint16_t, true>;
using CodecHLAinteger32BE = NumberCodec<Integer32, uint32_t, true>;
using CodecHLAinteger32LE = NumberCodec<Integer32, uint32_t, false>;
using CodecHLAinteger64BE = NumberCodec<Integer64, uint64_t, true>;
using CodecHLAinteger64LE = NumberCodec<Integer64, uint64_t, false>;
using CodecHLAoctet = OctetCodec<Octet>;
using CodecHLAoctetPairBE = OctetPairCodec<true>;
using CodecHLAoctetPairLE = OctetPairCodec<false>;
using CodecHLAunicodeChar = UnicodeCharCodec;
using CodecHLAunicodeString = UnicodeStringCodec;

// Value storage: internal value or caller-provided external memory.
template <typename T>
struct BasicValue
{
   T internal{};
   T* external = nullptr;

   T& ref() { return external ? *external : internal; }
   T const& ref() const { return external ? *external : internal; }
};
} // namespace

#define HLA_WIDEN2(x) L##x
#define HLA_WIDEN(x) HLA_WIDEN2(x)

// The classes forward-declared by DEFINE_ENCODING_HELPER_CLASS.
#define HLA_BASIC_IMPLEMENTATION(Cls, T) \
   class Cls##Implementation : public BasicValue<T> \
   { \
   };

#define HLA_BASIC_DEFINITION(Cls, T) \
   HLA_BASIC_IMPLEMENTATION(Cls, T) \
   \
   Cls::Cls() : _impl(new Cls##Implementation) {} \
   Cls::Cls(T const& inData) : _impl(new Cls##Implementation) { _impl->internal = inData; } \
   Cls::Cls(T* inData) : _impl(new Cls##Implementation) { _impl->external = inData; } \
   Cls::Cls(Cls const& rhs) : DataElement(rhs), _impl(new Cls##Implementation) \
   { \
      _impl->internal = rhs._impl->ref(); \
   } \
   Cls::~Cls() { delete _impl; } \
   Cls& Cls::operator=(Cls const& rhs) \
   { \
      if (this != &rhs) \
         _impl->ref() = rhs._impl->ref(); \
      return *this; \
   } \
   RTI_UNIQUE_PTR<DataElement> Cls::clone() const \
   { \
      return RTI_UNIQUE_PTR<DataElement>(new Cls(*this)); \
   } \
   VariableLengthData Cls::encode() const RTI_THROW((EncoderException)) \
   { \
      Buffer b; \
      encodeInto(b); \
      return toVariableLengthData(b); \
   } \
   void Cls::encode(VariableLengthData& inData) const RTI_THROW((EncoderException)) \
   { \
      Buffer b; \
      encodeInto(b); \
      inData.setData(b.empty() ? nullptr : b.data(), b.size()); \
   } \
   void Cls::encodeInto(std::vector<Octet>& buffer) const RTI_THROW((EncoderException)) \
   { \
      Codec##Cls::put(buffer, _impl->ref()); \
   } \
   void Cls::decode(VariableLengthData const& inData) RTI_THROW((EncoderException)) \
   { \
      decodeFrom(fromVariableLengthData(inData), 0); \
   } \
   size_t Cls::decodeFrom(std::vector<Octet> const& buffer, size_t index) \
      RTI_THROW((EncoderException)) \
   { \
      T value{}; \
      size_t const next = Codec##Cls::get(buffer, index, value, HLA_WIDEN(#Cls)); \
      _impl->ref() = value; \
      return next; \
   } \
   size_t Cls::getEncodedLength() const RTI_THROW((EncoderException)) \
   { \
      Buffer b; \
      encodeInto(b); \
      return b.size(); \
   } \
   unsigned int Cls::getOctetBoundary() const { return Codec##Cls::boundary(); } \
   Integer64 Cls::hash() const { return hashEncoding(*this); } \
   void Cls::setDataPointer(T* inData) RTI_THROW((EncoderException)) \
   { \
      if (inData == nullptr) \
         fail(HLA_WIDEN(#Cls) L"::setDataPointer: null pointer"); \
      _impl->external = inData; \
   } \
   void Cls::set(T inData) { _impl->ref() = inData; } \
   T Cls::get() const { return _impl->ref(); } \
   Cls& Cls::operator=(T rhs) \
   { \
      _impl->ref() = rhs; \
      return *this; \
   } \
   Cls::operator T() const { return _impl->ref(); }

HLA_BASIC_DEFINITION(HLAASCIIchar, char)
HLA_BASIC_DEFINITION(HLAASCIIstring, std::string)
HLA_BASIC_DEFINITION(HLAboolean, bool)
HLA_BASIC_DEFINITION(HLAbyte, Octet)
HLA_BASIC_DEFINITION(HLAfloat32BE, float)
HLA_BASIC_DEFINITION(HLAfloat32LE, float)
HLA_BASIC_DEFINITION(HLAfloat64BE, double)
HLA_BASIC_DEFINITION(HLAfloat64LE, double)
HLA_BASIC_DEFINITION(HLAinteger16LE, Integer16)
HLA_BASIC_DEFINITION(HLAinteger16BE, Integer16)
HLA_BASIC_DEFINITION(HLAinteger32BE, Integer32)
HLA_BASIC_DEFINITION(HLAinteger32LE, Integer32)
HLA_BASIC_DEFINITION(HLAinteger64BE, Integer64)
HLA_BASIC_DEFINITION(HLAinteger64LE, Integer64)
HLA_BASIC_DEFINITION(HLAoctet, Octet)
HLA_BASIC_DEFINITION(HLAoctetPairBE, OctetPair)
HLA_BASIC_DEFINITION(HLAoctetPairLE, OctetPair)
HLA_BASIC_DEFINITION(HLAunicodeChar, wchar_t)
HLA_BASIC_DEFINITION(HLAunicodeString, std::wstring)
} // namespace rti1516e
