// Internal helpers shared by the encoding helper classes (RTI/encoding).
//
// Encoding rules: IEEE 1516.2-2010 (OMT), section 4.13.
// - Every element aligns itself on its octet boundary, relative to the start
//   of the buffer, before writing (encodeInto) or reading (decodeFrom).
//   Containers therefore only have to encode their children in sequence.
// - getEncodedLength() is the length of the encoding starting on an aligned
//   position (index 0).

#pragma once

#include <RTI/VariableLengthData.h>
#include <RTI/encoding/DataElement.h>
#include <RTI/encoding/EncodingExceptions.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace rti1516e
{
namespace encoding
{
using Buffer = std::vector<Octet>;

inline size_t alignUp(size_t index, unsigned int boundary)
{
   if (boundary <= 1)
      return index;
   size_t const rest = index % boundary;
   return rest == 0 ? index : index + (boundary - rest);
}

// Appends zero padding so that the next octet lands on `boundary`.
inline void pad(Buffer& buffer, unsigned int boundary)
{
   buffer.resize(alignUp(buffer.size(), boundary), Octet(0));
}

[[noreturn]] inline void fail(std::wstring const& what)
{
   throw EncoderException(what);
}

// Skips the padding before an element and checks that `length` octets follow.
inline size_t alignAndCheck(Buffer const& buffer, size_t index, unsigned int boundary,
                            size_t length, wchar_t const* type)
{
   size_t const start = alignUp(index, boundary);
   if (start > buffer.size() || buffer.size() - start < length)
      fail(std::wstring(L"not enough data to decode ") + type);
   return start;
}

// ------------------------------------------------------------------ integers
template <typename U>
inline void putBE(Buffer& buffer, U value)
{
   for (int shift = int(sizeof(U) - 1) * 8; shift >= 0; shift -= 8)
      buffer.push_back(Octet((value >> shift) & 0xFF));
}

template <typename U>
inline void putLE(Buffer& buffer, U value)
{
   for (size_t i = 0; i < sizeof(U); ++i)
      buffer.push_back(Octet((value >> (i * 8)) & 0xFF));
}

template <typename U>
inline U getBE(Buffer const& buffer, size_t index)
{
   U value = 0;
   for (size_t i = 0; i < sizeof(U); ++i)
      value = U((value << 8) | static_cast<unsigned char>(buffer[index + i]));
   return value;
}

template <typename U>
inline U getLE(Buffer const& buffer, size_t index)
{
   U value = 0;
   for (size_t i = sizeof(U); i-- > 0;)
      value = U((value << 8) | static_cast<unsigned char>(buffer[index + i]));
   return value;
}

// HLAinteger32BE, used for array counts and HLAboolean.
inline void putCount(Buffer& buffer, size_t count)
{
   if (count > 0x7FFFFFFFu)
      fail(L"too many elements for an HLAinteger32BE count");
   pad(buffer, 4);
   putBE<uint32_t>(buffer, uint32_t(count));
}

inline size_t getCount(Buffer const& buffer, size_t& index, wchar_t const* type)
{
   index = alignAndCheck(buffer, index, 4, 4, type);
   int32_t const count = int32_t(getBE<uint32_t>(buffer, index));
   index += 4;
   if (count < 0)
      fail(std::wstring(L"negative element count while decoding ") + type);
   return size_t(count);
}

// ------------------------------------------------------- DataElement helpers
inline VariableLengthData toVariableLengthData(Buffer const& buffer)
{
   return VariableLengthData(buffer.empty() ? nullptr : buffer.data(), buffer.size());
}

inline Buffer fromVariableLengthData(VariableLengthData const& data)
{
   Octet const* p = static_cast<Octet const*>(data.data());
   return p ? Buffer(p, p + data.size()) : Buffer();
}

// Copies the value of `from` into `to` (same type) through its encoding:
// keeps the memory model (internal or external) of `to`.
inline void copyValue(DataElement const& from, DataElement& to)
{
   Buffer buffer;
   from.encodeInto(buffer);
   to.decodeFrom(buffer, 0);
}

inline bool sameEncoding(DataElement const& a, DataElement const& b)
{
   Buffer ba, bb;
   a.encodeInto(ba);
   b.encodeInto(bb);
   return ba == bb;
}

// FNV-1a 64 bits over the encoding.
inline Integer64 hashEncoding(DataElement const& element)
{
   Buffer buffer;
   element.encodeInto(buffer);
   uint64_t h = 1469598103934665603ull;
   for (Octet o : buffer)
   {
      h ^= static_cast<unsigned char>(o);
      h *= 1099511628211ull;
   }
   return Integer64(h);
}

// Slot of a container: either an owned copy or a caller-provided pointer.
struct Slot
{
   DataElement* element = nullptr;
   bool owned = false;

   void reset(DataElement* e, bool own)
   {
      if (owned)
         delete element;
      element = e;
      owned = own;
   }
};

inline DataElement* cloneOf(DataElement const& e)
{
   return e.clone().release();
}
} // namespace encoding
} // namespace rti1516e
