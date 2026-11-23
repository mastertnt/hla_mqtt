// wchar_t <-> UTF-8 conversions without std::wstring_convert (deprecated).
// wchar_t is UTF-16 on Windows and UTF-32 on Linux: both are handled.
#pragma once

#include <cstdint>
#include <string>

namespace hla_mqtt
{
inline std::string toUtf8(std::wstring const& in)
{
   std::string out;
   out.reserve(in.size());
   for (size_t i = 0; i < in.size(); ++i)
   {
      std::uint32_t cp = static_cast<std::uint32_t>(in[i]);
      if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < in.size())
      {
         std::uint32_t lo = static_cast<std::uint32_t>(in[i + 1]);
         if (lo >= 0xDC00 && lo <= 0xDFFF)
         {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            ++i;
         }
      }
      if (cp < 0x80)
         out += static_cast<char>(cp);
      else if (cp < 0x800)
      {
         out += static_cast<char>(0xC0 | (cp >> 6));
         out += static_cast<char>(0x80 | (cp & 0x3F));
      }
      else if (cp < 0x10000)
      {
         out += static_cast<char>(0xE0 | (cp >> 12));
         out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
         out += static_cast<char>(0x80 | (cp & 0x3F));
      }
      else
      {
         out += static_cast<char>(0xF0 | (cp >> 18));
         out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
         out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
         out += static_cast<char>(0x80 | (cp & 0x3F));
      }
   }
   return out;
}

inline std::wstring fromUtf8(std::string const& in)
{
   std::wstring out;
   out.reserve(in.size());
   for (size_t i = 0; i < in.size();)
   {
      unsigned char c = static_cast<unsigned char>(in[i]);
      std::uint32_t cp;
      size_t len;
      if (c < 0x80) { cp = c; len = 1; }
      else if ((c >> 5) == 0x6) { cp = c & 0x1F; len = 2; }
      else if ((c >> 4) == 0xE) { cp = c & 0x0F; len = 3; }
      else if ((c >> 3) == 0x1E) { cp = c & 0x07; len = 4; }
      else { out += L'?'; ++i; continue; }
      if (i + len > in.size()) { out += L'?'; break; }
      for (size_t k = 1; k < len; ++k)
         cp = (cp << 6) | (static_cast<unsigned char>(in[i + k]) & 0x3F);
      i += len;
      if (sizeof(wchar_t) == 2 && cp >= 0x10000)
      {
         cp -= 0x10000;
         out += static_cast<wchar_t>(0xD800 + (cp >> 10));
         out += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
      }
      else
         out += static_cast<wchar_t>(cp);
   }
   return out;
}
}
