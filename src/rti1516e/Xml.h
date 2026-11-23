// Minimal non-validating XML parser for FOM modules (OMT DIF).
// Supports elements, attributes, text, CDATA, comments, processing
// instructions, DOCTYPE (skipped), the 5 predefined entities and character
// references. Namespace prefixes are dropped from element and attribute names.
// Input and output strings are UTF-8 (a UTF-8 BOM is skipped; UTF-16 input
// with a BOM is converted).
#pragma once

#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace hla_mqtt
{
struct XmlError : std::runtime_error
{
   using std::runtime_error::runtime_error;
};

struct XmlElement
{
   std::string name;                              // local name, prefix removed
   std::map<std::string, std::string> attributes; // local names
   std::string text;                              // concatenated direct text
   std::vector<std::unique_ptr<XmlElement>> children;

   // First child with this local name, or nullptr.
   XmlElement const* child(std::string const& n) const;
   std::vector<XmlElement const*> childrenNamed(std::string const& n) const;
   // Trimmed text of the child `n`, else the attribute `n`, else "".
   std::string value(std::string const& n) const;
};

// Throws XmlError with a line number on malformed input.
std::unique_ptr<XmlElement> parseXml(std::string const& document);

std::string trim(std::string const& s);
} // namespace hla_mqtt
