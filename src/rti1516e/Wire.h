// Topic helpers and the binary payload format of the object messages.
//
// Binary payloads (big-endian), first octet = kind, second = format version:
//   u64  = 8 octets, u32 = 4 octets, bytes = u32 length + octets
#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace hla_mqtt
{
inline std::string hex64(std::uint64_t v)
{
   char b[17];
   std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
   return b;
}

// One MQTT topic level: everything except [A-Za-z0-9-_.~] is percent-encoded
// (so '/', '+', '#', '@', spaces and non-ASCII characters are safe).
inline std::string encodeTopicLevel(std::string const& utf8)
{
   static char const* digits = "0123456789ABCDEF";
   std::string out;
   for (unsigned char c : utf8)
   {
      if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
         out += char(c);
      else
      {
         out += '%';
         out += digits[c >> 4];
         out += digits[c & 0xF];
      }
   }
   return out;
}

// "HLAobjectRoot.A.B" -> "HLAobjectRoot/A/B" (each level encoded)
inline std::string classTopicPath(std::string const& qualified)
{
   std::string out;
   size_t start = 0;
   for (;;)
   {
      size_t dot = qualified.find('.', start);
      out += encodeTopicLevel(qualified.substr(start, dot == std::string::npos ? std::string::npos : dot - start));
      if (dot == std::string::npos)
         return out;
      out += '/';
      start = dot + 1;
   }
}

enum class WireKind : unsigned char
{
   Update = 1,
   Interaction = 2,
   Delete = 3,
   Request = 4,
   Retract = 5 // sender, seq, retraction handle (8.21)
};

unsigned char const kWireVersion = 1;

class WireWriter
{
public:
   explicit WireWriter(WireKind kind)
   {
      _buf += char(kind);
      _buf += char(kWireVersion);
   }
   WireWriter& u8(unsigned v)
   {
      _buf += char(v & 0xFF);
      return *this;
   }
   WireWriter& u32(std::uint32_t v)
   {
      for (int s = 24; s >= 0; s -= 8)
         _buf += char((v >> s) & 0xFF);
      return *this;
   }
   WireWriter& u64(std::uint64_t v)
   {
      for (int s = 56; s >= 0; s -= 8)
         _buf += char((v >> s) & 0xFF);
      return *this;
   }
   WireWriter& bytes(void const* data, size_t size)
   {
      u32(std::uint32_t(size));
      if (size)
         _buf.append(static_cast<char const*>(data), size);
      return *this;
   }
   std::string const& str() const { return _buf; }

private:
   std::string _buf;
};

struct WireError : std::runtime_error
{
   using std::runtime_error::runtime_error;
};

class WireReader
{
public:
   explicit WireReader(std::string const& data) : _d(data)
   {
      if (_d.size() < 2)
         throw WireError("message too short");
      _kind = WireKind(static_cast<unsigned char>(_d[0]));
      if (static_cast<unsigned char>(_d[1]) != kWireVersion)
         throw WireError("unsupported message version");
      _i = 2;
   }
   WireKind kind() const { return _kind; }
   bool atEnd() const { return _i >= _d.size(); } // optional trailing fields
   unsigned u8()
   {
      need(1);
      return static_cast<unsigned char>(_d[_i++]);
   }
   std::uint32_t u32()
   {
      need(4);
      std::uint32_t v = 0;
      for (int k = 0; k < 4; ++k)
         v = (v << 8) | static_cast<unsigned char>(_d[_i++]);
      return v;
   }
   std::uint64_t u64()
   {
      need(8);
      std::uint64_t v = 0;
      for (int k = 0; k < 8; ++k)
         v = (v << 8) | static_cast<unsigned char>(_d[_i++]);
      return v;
   }
   std::string bytes()
   {
      std::uint32_t n = u32();
      need(n);
      std::string out = _d.substr(_i, n);
      _i += n;
      return out;
   }

private:
   void need(size_t n) const
   {
      if (_d.size() - _i < n)
         throw WireError("truncated message");
   }
   std::string const& _d;
   size_t _i = 0;
   WireKind _kind;
};
} // namespace hla_mqtt
