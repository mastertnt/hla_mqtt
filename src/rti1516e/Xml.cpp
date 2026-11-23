#include "Xml.h"

#include <cstdint>

namespace hla_mqtt
{
std::string trim(std::string const& s)
{
   size_t b = 0, e = s.size();
   while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
      ++b;
   while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
      --e;
   return s.substr(b, e - b);
}

XmlElement const* XmlElement::child(std::string const& n) const
{
   for (auto const& c : children)
      if (c->name == n)
         return c.get();
   return nullptr;
}

std::vector<XmlElement const*> XmlElement::childrenNamed(std::string const& n) const
{
   std::vector<XmlElement const*> out;
   for (auto const& c : children)
      if (c->name == n)
         out.push_back(c.get());
   return out;
}

std::string XmlElement::value(std::string const& n) const
{
   if (XmlElement const* c = child(n))
      return trim(c->text);
   auto it = attributes.find(n);
   return it == attributes.end() ? std::string() : trim(it->second);
}

namespace
{
void appendUtf8(std::string& out, std::uint32_t cp)
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

// UTF-16 (with BOM) -> UTF-8
std::string fromUtf16(std::string const& in, bool bigEndian)
{
   std::string out;
   size_t i = 2;
   auto unit = [&](size_t k) -> std::uint32_t {
      unsigned char a = static_cast<unsigned char>(in[k]), b = static_cast<unsigned char>(in[k + 1]);
      return bigEndian ? (std::uint32_t(a) << 8 | b) : (std::uint32_t(b) << 8 | a);
   };
   while (i + 1 < in.size())
   {
      std::uint32_t cp = unit(i);
      i += 2;
      if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < in.size())
      {
         std::uint32_t lo = unit(i);
         if (lo >= 0xDC00 && lo < 0xE000)
         {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 2;
         }
      }
      appendUtf8(out, cp);
   }
   return out;
}

std::string localName(std::string const& qname)
{
   size_t colon = qname.find(':');
   return colon == std::string::npos ? qname : qname.substr(colon + 1);
}

class Parser
{
public:
   explicit Parser(std::string const& doc) : t(doc) {}

   std::unique_ptr<XmlElement> document()
   {
      std::unique_ptr<XmlElement> root;
      for (;;)
      {
         skipWs();
         if (i >= t.size())
            break;
         if (starts("<?"))
            skipPast("?>");
         else if (starts("<!--"))
            skipPast("-->");
         else if (starts("<!DOCTYPE"))
            skipDoctype();
         else if (t[i] == '<')
         {
            if (root)
               fail("more than one root element");
            root = element();
         }
         else
            fail("text outside the root element");
      }
      if (!root)
         fail("no root element");
      return root;
   }

private:
   [[noreturn]] void fail(std::string const& what) const
   {
      size_t line = 1;
      for (size_t k = 0; k < i && k < t.size(); ++k)
         if (t[k] == '\n')
            ++line;
      throw XmlError("XML line " + std::to_string(line) + ": " + what);
   }

   bool starts(char const* s) const { return t.compare(i, std::char_traits<char>::length(s), s) == 0; }

   void skipWs()
   {
      while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n'))
         ++i;
   }

   void skipPast(char const* end)
   {
      size_t p = t.find(end, i);
      if (p == std::string::npos)
         fail(std::string("missing '") + end + "'");
      i = p + std::char_traits<char>::length(end);
   }

   void skipDoctype()
   {
      int depth = 0;
      for (; i < t.size(); ++i)
      {
         if (t[i] == '[')
            ++depth;
         else if (t[i] == ']')
            --depth;
         else if (t[i] == '>' && depth <= 0)
         {
            ++i;
            return;
         }
      }
      fail("unterminated DOCTYPE");
   }

   std::string name()
   {
      size_t start = i;
      while (i < t.size() && !(t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n' || t[i] == '>' ||
                               t[i] == '/' || t[i] == '=' || t[i] == '<'))
         ++i;
      if (start == i)
         fail("name expected");
      return t.substr(start, i - start);
   }

   std::string decode(std::string const& raw)
   {
      std::string out;
      for (size_t k = 0; k < raw.size(); ++k)
      {
         if (raw[k] != '&')
         {
            out += raw[k];
            continue;
         }
         size_t semi = raw.find(';', k);
         if (semi == std::string::npos)
            fail("unterminated entity");
         std::string ent = raw.substr(k + 1, semi - k - 1);
         if (ent == "lt") out += '<';
         else if (ent == "gt") out += '>';
         else if (ent == "amp") out += '&';
         else if (ent == "quot") out += '"';
         else if (ent == "apos") out += '\'';
         else if (!ent.empty() && ent[0] == '#')
         {
            std::uint32_t cp = 0;
            try
            {
               cp = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                       ? std::uint32_t(std::stoul(ent.substr(2), nullptr, 16))
                       : std::uint32_t(std::stoul(ent.substr(1)));
            }
            catch (std::exception const&)
            {
               fail("bad character reference &" + ent + ";");
            }
            appendUtf8(out, cp);
         }
         else
            fail("unknown entity &" + ent + ";");
         k = semi;
      }
      return out;
   }

   std::unique_ptr<XmlElement> element()
   {
      ++i; // '<'
      std::unique_ptr<XmlElement> e(new XmlElement);
      std::string qname = name();
      e->name = localName(qname);
      for (;;)
      {
         skipWs();
         if (i >= t.size())
            fail("unterminated start tag <" + qname + ">");
         if (starts("/>"))
         {
            i += 2;
            return e;
         }
         if (t[i] == '>')
         {
            ++i;
            break;
         }
         std::string an = name();
         skipWs();
         if (i >= t.size() || t[i] != '=')
            fail("'=' expected after attribute " + an);
         ++i;
         skipWs();
         if (i >= t.size() || (t[i] != '"' && t[i] != '\''))
            fail("quoted value expected for attribute " + an);
         char q = t[i++];
         size_t end = t.find(q, i);
         if (end == std::string::npos)
            fail("unterminated attribute value");
         e->attributes[localName(an)] = decode(t.substr(i, end - i));
         i = end + 1;
      }
      // content
      for (;;)
      {
         if (i >= t.size())
            fail("missing end tag </" + qname + ">");
         if (starts("</"))
         {
            i += 2;
            std::string closing = name();
            if (closing != qname)
               fail("end tag </" + closing + "> does not match <" + qname + ">");
            skipWs();
            if (i >= t.size() || t[i] != '>')
               fail("'>' expected");
            ++i;
            return e;
         }
         if (starts("<!--"))
            skipPast("-->");
         else if (starts("<![CDATA["))
         {
            i += 9;
            size_t end = t.find("]]>", i);
            if (end == std::string::npos)
               fail("unterminated CDATA");
            e->text += t.substr(i, end - i);
            i = end + 3;
         }
         else if (starts("<?"))
            skipPast("?>");
         else if (t[i] == '<')
            e->children.push_back(element());
         else
         {
            size_t end = t.find('<', i);
            if (end == std::string::npos)
               end = t.size();
            e->text += decode(t.substr(i, end - i));
            i = end;
         }
      }
   }

   std::string const& t;
   size_t i = 0;
};
} // namespace

std::unique_ptr<XmlElement> parseXml(std::string const& document)
{
   if (document.size() >= 2 && static_cast<unsigned char>(document[0]) == 0xFF &&
       static_cast<unsigned char>(document[1]) == 0xFE)
      return parseXml(fromUtf16(document, false));
   if (document.size() >= 2 && static_cast<unsigned char>(document[0]) == 0xFE &&
       static_cast<unsigned char>(document[1]) == 0xFF)
      return parseXml(fromUtf16(document, true));
   if (document.compare(0, 3, "\xEF\xBB\xBF") == 0)
      return Parser(document.substr(3)).document();
   return Parser(document).document();
}
} // namespace hla_mqtt
