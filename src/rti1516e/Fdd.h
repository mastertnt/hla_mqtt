// Federation Object Model of a federation execution (merged FOM modules).
//
// Handles are DETERMINISTIC: a 64-bit FNV-1a hash of the qualified name
// (with a kind prefix), so every federate computes the same handles from the
// same FDD without any coordination:
//   object class      "O|HLAobjectRoot.A.B"
//   attribute         "A|<declaring class qualified name>|<attribute name>"
//   interaction class "I|HLAinteractionRoot.X.Y"
//   parameter         "P|<declaring class qualified name>|<parameter name>"
//   dimension         "D|<dimension name>"
// A hash collision inside one FDD is reported as InconsistentFDD.
//
// Names are UTF-8. Both OMT DIF formats are read: IEEE 1516-2010 (properties
// as child elements) and IEEE 1516-2000 (properties as XML attributes).
#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace hla_mqtt
{
struct FddError : std::runtime_error
{
   enum Kind
   {
      ErrorReading, // malformed module (ErrorReadingFDD)
      Inconsistent  // conflicts with the modules already merged (InconsistentFDD)
   };
   FddError(Kind k, std::string const& what) : std::runtime_error(what), kind(k) {}
   Kind kind;
};

std::uint64_t fnv1a64(std::string const& s);

class Fdd
{
public:
   struct Attribute
   {
      std::uint64_t handle = 0;
      std::string name;
      std::uint64_t declaringClass = 0;
      std::string transportation = "HLAreliable";
      std::string order = "Receive";
      std::vector<std::string> dimensions;
      bool builtin = false;
   };
   struct ObjectClass
   {
      std::uint64_t handle = 0;
      std::string name;      // last component
      std::string qualified; // HLAobjectRoot.A.B
      std::uint64_t parent = 0;
      std::vector<std::uint64_t> attributes; // declared by this class
      // Module that defined the attributes ("" while only scaffolding: a
      // module may name the class without attributes, to place subclasses).
      std::string definedBy;
   };
   struct Parameter
   {
      std::uint64_t handle = 0;
      std::string name;
      std::uint64_t declaringClass = 0;
   };
   struct InteractionClass
   {
      std::uint64_t handle = 0;
      std::string name;
      std::string qualified;
      std::uint64_t parent = 0;
      std::vector<std::uint64_t> parameters; // declared by this class
      std::string definedBy;                 // module that defined the parameters
      std::string transportation = "HLAreliable";
      std::string order = "Receive";
      std::vector<std::string> dimensions;
      bool transportationSet = false;
   };
   struct Dimension
   {
      std::uint64_t handle = 0;
      std::string name;
      unsigned long upperBound = 0;
   };

   Fdd(); // HLAobjectRoot (with HLAprivilegeToDeleteObject) and HLAinteractionRoot

   // Merges one module (XML document). All or nothing: on FddError the FDD is
   // unchanged. `label` names the module in error messages.
   void merge(std::string const& xmlDocument, std::string const& label);

   ObjectClass const* objectClass(std::uint64_t handle) const;
   // Accepts "HLAobjectRoot.A.B", "A.B", or a unique suffix ("B").
   ObjectClass const* objectClassByName(std::string const& name) const;
   Attribute const* attribute(std::uint64_t handle) const;
   // Looks in the class and its ancestors. A qualified name
   // ("HLAobjectRoot.A.Attr", "A.Attr") names the declaring class, which must
   // be the class or one of its ancestors.
   Attribute const* findAttribute(std::uint64_t classHandle, std::string const& name) const;
   bool classHasAttribute(std::uint64_t classHandle, std::uint64_t attributeHandle) const;

   InteractionClass const* interactionClass(std::uint64_t handle) const;
   InteractionClass const* interactionClassByName(std::string const& name) const;
   Parameter const* parameter(std::uint64_t handle) const;
   Parameter const* findParameter(std::uint64_t classHandle, std::string const& name) const;
   bool classHasParameter(std::uint64_t classHandle, std::uint64_t parameterHandle) const;

   Dimension const* dimension(std::uint64_t handle) const;
   Dimension const* dimensionByName(std::string const& name) const;
   // FOM <switches> (IEEE 1516-2010): value of the first module defining it.
   bool switchEnabled(std::string const& name, bool defaultValue) const
   {
      auto it = _switches.find(name);
      return it == _switches.end() ? defaultValue : it->second;
   }
   // Update rate designator -> rate (Hz); false when unknown.
   bool updateRate(std::string const& name, double& rate) const;

   std::map<std::uint64_t, ObjectClass> const& objectClasses() const { return _objectClasses; }
   std::map<std::uint64_t, InteractionClass> const& interactionClasses() const { return _interactionClasses; }

private:
   void mergeInPlace(std::string const& xmlDocument, std::string const& label);

   std::map<std::uint64_t, ObjectClass> _objectClasses;
   std::map<std::string, std::uint64_t> _objectClassByName;
   std::map<std::uint64_t, Attribute> _attributes;
   std::map<std::uint64_t, InteractionClass> _interactionClasses;
   std::map<std::string, std::uint64_t> _interactionClassByName;
   std::map<std::uint64_t, Parameter> _parameters;
   std::map<std::uint64_t, Dimension> _dimensions;
   std::map<std::string, std::uint64_t> _dimensionByName;
   std::map<std::string, double> _updateRates;
   std::map<std::string, bool> _switches;

   friend struct FddBuilder;
};
} // namespace hla_mqtt
