// Minimal JSON value, writer and parser for the small records exchanged on
// MQTT (federation and federate records). UTF-8 strings, no external deps.
#pragma once

#include <cctype>
#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace hla_mqtt
{
class Json
{
public:
   enum class Type { Null, Bool, Number, String, Array, Object };

   Json() = default;
   Json(bool b) : _type(Type::Bool), _bool(b) {}
   Json(double n) : _type(Type::Number), _number(n) {}
   Json(int n) : _type(Type::Number), _number(n) {}
   Json(std::string s) : _type(Type::String), _string(std::move(s)) {}
   Json(char const* s) : _type(Type::String), _string(s) {}

   static Json array() { Json j; j._type = Type::Array; return j; }
   static Json object() { Json j; j._type = Type::Object; return j; }

   Type type() const { return _type; }
   bool isNull() const { return _type == Type::Null; }

   // Object access: missing keys give a Null value.
   Json const& operator[](std::string const& key) const
   {
      static Json const null;
      auto it = _object.find(key);
      return it == _object.end() ? null : it->second;
   }
   Json& set(std::string const& key, Json value)
   {
      _type = Type::Object;
      _object[key] = std::move(value);
      return *this;
   }
   Json& push(Json value)
   {
      _type = Type::Array;
      _array.push_back(std::move(value));
      return *this;
   }

   std::string const& str() const { return _string; }
   double num() const { return _number; }
   bool boolean() const { return _bool; }
   std::vector<Json> const& items() const { return _array; }
   std::map<std::string, Json> const& members() const { return _object; }

   std::vector<std::string> strings() const
   {
      std::vector<std::string> out;
      for (Json const& j : _array)
         if (j._type == Type::String)
            out.push_back(j._string);
      return out;
   }

   std::string dump() const
   {
      std::string out;
      write(out);
      return out;
   }

   // Throws std::runtime_error on malformed input.
   static Json parse(std::string const& text)
   {
      size_t i = 0;
      Json j = parseValue(text, i);
      skipWs(text, i);
      if (i != text.size())
         throw std::runtime_error("JSON: trailing characters");
      return j;
   }

private:
   void write(std::string& out) const
   {
      switch (_type)
      {
      case Type::Null: out += "null"; break;
      case Type::Bool: out += _bool ? "true" : "false"; break;
      case Type::Number:
      {
         char b[32];
         std::snprintf(b, sizeof b, "%.17g", _number);
         out += b;
         break;
      }
      case Type::String: writeString(out, _string); break;
      case Type::Array:
         out += '[';
         for (size_t k = 0; k < _array.size(); ++k)
         {
            if (k)
               out += ',';
            _array[k].write(out);
         }
         out += ']';
         break;
      case Type::Object:
      {
         out += '{';
         bool first = true;
         for (auto const& kv : _object)
         {
            if (!first)
               out += ',';
            first = false;
            writeString(out, kv.first);
            out += ':';
            kv.second.write(out);
         }
         out += '}';
         break;
      }
      }
   }

   static void writeString(std::string& out, std::string const& s)
   {
      out += '"';
      for (unsigned char c : s)
      {
         switch (c)
         {
         case '"': out += "\\\""; break;
         case '\\': out += "\\\\"; break;
         case '\n': out += "\\n"; break;
         case '\r': out += "\\r"; break;
         case '\t': out += "\\t"; break;
         default:
            if (c < 0x20)
            {
               char b[8];
               std::snprintf(b, sizeof b, "\\u%04x", c);
               out += b;
            }
            else
               out += char(c);
         }
      }
      out += '"';
   }

   static void skipWs(std::string const& t, size_t& i)
   {
      while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t'))
         ++i;
   }

   [[noreturn]] static void fail(char const* what) { throw std::runtime_error(std::string("JSON: ") + what); }

   static void appendUtf8(std::string& out, unsigned cp)
   {
      if (cp < 0x80)
         out += char(cp);
      else if (cp < 0x800)
      {
         out += char(0xC0 | (cp >> 6));
         out += char(0x80 | (cp & 0x3F));
      }
      else if (cp < 0x10000)
      {
         out += char(0xE0 | (cp >> 12));
         out += char(0x80 | ((cp >> 6) & 0x3F));
         out += char(0x80 | (cp & 0x3F));
      }
      else
      {
         out += char(0xF0 | (cp >> 18));
         out += char(0x80 | ((cp >> 12) & 0x3F));
         out += char(0x80 | ((cp >> 6) & 0x3F));
         out += char(0x80 | (cp & 0x3F));
      }
   }

   static unsigned hex4(std::string const& t, size_t i)
   {
      if (i + 4 > t.size())
         fail("truncated \\u escape");
      unsigned v = 0;
      for (size_t k = i; k < i + 4; ++k)
      {
         char c = t[k];
         v <<= 4;
         if (c >= '0' && c <= '9') v |= unsigned(c - '0');
         else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
         else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
         else fail("bad \\u escape");
      }
      return v;
   }

   static std::string parseString(std::string const& t, size_t& i)
   {
      if (t[i] != '"')
         fail("string expected");
      ++i;
      std::string out;
      while (i < t.size() && t[i] != '"')
      {
         char c = t[i++];
         if (c != '\\')
         {
            out += c;
            continue;
         }
         if (i >= t.size())
            fail("truncated escape");
         char e = t[i++];
         switch (e)
         {
         case 'n': out += '\n'; break;
         case 'r': out += '\r'; break;
         case 't': out += '\t'; break;
         case 'b': out += '\b'; break;
         case 'f': out += '\f'; break;
         case 'u':
         {
            unsigned cp = hex4(t, i);
            i += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u')
            {
               unsigned lo = hex4(t, i + 2);
               if (lo >= 0xDC00 && lo < 0xE000)
               {
                  cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                  i += 6;
               }
            }
            appendUtf8(out, cp);
            break;
         }
         default: out += e; break;
         }
      }
      if (i >= t.size())
         fail("unterminated string");
      ++i;
      return out;
   }

   static Json parseValue(std::string const& t, size_t& i)
   {
      skipWs(t, i);
      if (i >= t.size())
         fail("unexpected end");
      char c = t[i];
      if (c == '{')
      {
         Json j = object();
         ++i;
         skipWs(t, i);
         if (i < t.size() && t[i] == '}')
         {
            ++i;
            return j;
         }
         for (;;)
         {
            skipWs(t, i);
            std::string key = parseString(t, i);
            skipWs(t, i);
            if (i >= t.size() || t[i] != ':')
               fail("':' expected");
            ++i;
            j._object[key] = parseValue(t, i);
            skipWs(t, i);
            if (i < t.size() && t[i] == ',')
            {
               ++i;
               continue;
            }
            if (i < t.size() && t[i] == '}')
            {
               ++i;
               return j;
            }
            fail("',' or '}' expected");
         }
      }
      if (c == '[')
      {
         Json j = array();
         ++i;
         skipWs(t, i);
         if (i < t.size() && t[i] == ']')
         {
            ++i;
            return j;
         }
         for (;;)
         {
            j._array.push_back(parseValue(t, i));
            skipWs(t, i);
            if (i < t.size() && t[i] == ',')
            {
               ++i;
               continue;
            }
            if (i < t.size() && t[i] == ']')
            {
               ++i;
               return j;
            }
            fail("',' or ']' expected");
         }
      }
      if (c == '"')
         return Json(parseString(t, i));
      if (t.compare(i, 4, "true") == 0)
      {
         i += 4;
         return Json(true);
      }
      if (t.compare(i, 5, "false") == 0)
      {
         i += 5;
         return Json(false);
      }
      if (t.compare(i, 4, "null") == 0)
      {
         i += 4;
         return Json();
      }
      size_t start = i;
      while (i < t.size() && (std::isdigit(static_cast<unsigned char>(t[i])) || t[i] == '-' || t[i] == '+' ||
                              t[i] == '.' || t[i] == 'e' || t[i] == 'E'))
         ++i;
      if (start == i)
         fail("unexpected character");
      return Json(std::stod(t.substr(start, i - start)));
   }

   Type _type = Type::Null;
   bool _bool = false;
   double _number = 0;
   std::string _string;
   std::vector<Json> _array;
   std::map<std::string, Json> _object;
};
} // namespace hla_mqtt
