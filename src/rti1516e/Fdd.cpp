#include "Fdd.h"

#include "Xml.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace hla_mqtt
{
std::uint64_t fnv1a64(std::string const& s)
{
   std::uint64_t h = 1469598103934665603ull;
   for (unsigned char c : s)
   {
      h ^= c;
      h *= 1099511628211ull;
   }
   return h == 0 ? 1 : h; // 0 is the invalid handle
}

namespace
{
std::string const kObjectRoot = "HLAobjectRoot";
std::string const kInteractionRoot = "HLAinteractionRoot";

[[noreturn]] void reading(std::string const& label, std::string const& what)
{
   throw FddError(FddError::ErrorReading, label + ": " + what);
}

[[noreturn]] void inconsistent(std::string const& label, std::string const& what)
{
   throw FddError(FddError::Inconsistent, label + ": " + what);
}

std::string lower(std::string s)
{
   std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
   return s;
}

// "TimeStamp" / "Receive" ("NA" and empty: Receive)
std::string normalizeOrder(std::string const& raw, std::string const& label)
{
   std::string const l = lower(trim(raw));
   if (l.empty() || l == "receive" || l == "na")
      return "Receive";
   if (l == "timestamp")
      return "TimeStamp";
   reading(label, "unknown order type '" + raw + "'");
}

std::string normalizeTransportation(std::string const& raw)
{
   std::string const t = trim(raw);
   if (t.empty() || lower(t) == "na")
      return "HLAreliable";
   if (lower(t) == "hlareliable")
      return "HLAreliable";
   if (lower(t) == "hlabesteffort")
      return "HLAbestEffort";
   return t;
}

// 2010: <dimensions><dimension>X</dimension></dimensions>; 2000: dimensions="X, Y",
// "X Y" or "NA".
std::vector<std::string> readDimensionList(XmlElement const& e)
{
   std::vector<std::string> out;
   if (XmlElement const* d = e.child("dimensions"))
   {
      for (XmlElement const* x : d->childrenNamed("dimension"))
      {
         std::string const n = trim(x->text);
         if (!n.empty() && n != "NA")
            out.push_back(n);
      }
   }
   else
   {
      auto it = e.attributes.find("dimensions");
      if (it != e.attributes.end())
      {
         std::string const s = it->second;
         size_t start = 0;
         while (start <= s.size())
         {
            size_t sep = s.find_first_of(", \t\r\n", start);
            if (sep == std::string::npos)
               sep = s.size();
            std::string const n = trim(s.substr(start, sep - start));
            if (!n.empty() && n != "NA")
               out.push_back(n);
            start = sep + 1;
         }
      }
   }
   std::sort(out.begin(), out.end());
   out.erase(std::unique(out.begin(), out.end()), out.end());
   return out;
}

template <class Map>
void checkCollision(Map const& map, std::uint64_t handle, std::string const& qualified, std::string const& label)
{
   auto it = map.find(handle);
   if (it != map.end() && it->second.qualified != qualified)
      inconsistent(label, "handle collision between '" + qualified + "' and '" + it->second.qualified + "'");
}
} // namespace

// The root class and the MOM classes (HLAmanager subtree).
bool isStandardClass(std::string const& qualified, std::string const& root)
{
   return qualified == root || qualified.compare(0, root.size() + 10, root + ".HLAmanager") == 0;
}

struct FddBuilder
{
   Fdd& fdd;
   std::string const& label;

   void objectClass(XmlElement const& e, Fdd::ObjectClass const* parent)
   {
      std::string const name = e.value("name");
      if (name.empty())
         reading(label, "objectClass without a name");
      if (name.find('.') != std::string::npos)
         reading(label, "object class name '" + name + "' contains a '.'");

      std::string qualified;
      std::uint64_t parentHandle = 0;
      if (parent == nullptr)
      {
         if (name != kObjectRoot)
         {
            // Module without the root (tolerated, like other RTIs): the
            // class is a child of HLAobjectRoot, known from the MIM.
            auto root = fdd._objectClassByName.find(kObjectRoot);
            if (root == fdd._objectClassByName.end())
               reading(label, "top-level object class must be HLAobjectRoot, not '" + name + "'");
            objectClass(e, &fdd._objectClasses[root->second]);
            return;
         }
         qualified = kObjectRoot;
      }
      else
      {
         qualified = parent->qualified + "." + name;
         parentHandle = parent->handle;
      }

      std::uint64_t const handle = fnv1a64("O|" + qualified);
      checkCollision(fdd._objectClasses, handle, qualified, label);
      Fdd::ObjectClass& c = fdd._objectClasses[handle];
      if (c.handle == 0)
      {
         c.handle = handle;
         c.name = name;
         c.qualified = qualified;
         c.parent = parentHandle;
         fdd._objectClassByName[qualified] = handle;
      }

      std::vector<XmlElement const*> const attributes = e.childrenNamed("attribute");
      if (!attributes.empty() && !isStandardClass(qualified, kObjectRoot))
      {
         // IEEE 1516.2-2010: a class defined (with attributes) in several
         // modules has the same definition in each; a module naming it
         // without attributes is a scaffolding definition. Not applied to
         // the root and MOM classes, which IEEE 1516-2000 FOMs extend
         // (privilegeToDelete next to HLAprivilegeToDeleteObject).
         if (!c.definedBy.empty())
         {
            std::vector<std::string> mine, theirs;
            for (XmlElement const* a : attributes)
               mine.push_back(a->value("name"));
            for (std::uint64_t h : c.attributes)
               theirs.push_back(fdd._attributes[h].name);
            std::sort(mine.begin(), mine.end());
            std::sort(theirs.begin(), theirs.end());
            if (mine != theirs)
               inconsistent(label, "object class " + qualified + " defined with other attributes in " + c.definedBy);
         }
         else
            c.definedBy = label;
      }
      for (XmlElement const* a : attributes)
         attribute(*a, c);
      for (XmlElement const* sub : e.childrenNamed("objectClass"))
         objectClass(*sub, &fdd._objectClasses[handle]);
   }

   void attribute(XmlElement const& e, Fdd::ObjectClass& c)
   {
      std::string const name = e.value("name");
      if (name.empty())
         reading(label, "attribute without a name in " + c.qualified);
      Fdd::Attribute a;
      a.name = name;
      a.declaringClass = c.handle;
      a.transportation = normalizeTransportation(e.value("transportation"));
      a.order = normalizeOrder(e.value("order"), label);
      a.dimensions = readDimensionList(e);
      a.handle = fnv1a64("A|" + c.qualified + "|" + name);

      // Already declared by an ancestor: redefinition.
      for (std::uint64_t p = c.parent; p != 0; p = fdd._objectClasses[p].parent)
         for (std::uint64_t h : fdd._objectClasses[p].attributes)
            if (fdd._attributes[h].name == name)
               inconsistent(label, "attribute '" + name + "' of " + c.qualified + " already declared by " +
                                      fdd._objectClasses[p].qualified);

      auto it = fdd._attributes.find(a.handle);
      if (it != fdd._attributes.end())
      {
         Fdd::Attribute const& old = it->second;
         if (old.declaringClass != c.handle || old.name != name)
            inconsistent(label, "handle collision for attribute '" + name + "' of " + c.qualified);
         if (!old.builtin && (old.transportation != a.transportation || old.order != a.order ||
                              old.dimensions != a.dimensions))
            inconsistent(label, "attribute '" + name + "' of " + c.qualified +
                                   " defined differently in another module");
         return;
      }
      fdd._attributes[a.handle] = a;
      c.attributes.push_back(a.handle);
   }

   void interactionClass(XmlElement const& e, Fdd::InteractionClass const* parent)
   {
      std::string const name = e.value("name");
      if (name.empty())
         reading(label, "interactionClass without a name");
      if (name.find('.') != std::string::npos)
         reading(label, "interaction class name '" + name + "' contains a '.'");

      std::string qualified;
      std::uint64_t parentHandle = 0;
      if (parent == nullptr)
      {
         if (name != kInteractionRoot)
         {
            auto root = fdd._interactionClassByName.find(kInteractionRoot);
            if (root == fdd._interactionClassByName.end())
               reading(label, "top-level interaction class must be HLAinteractionRoot, not '" + name + "'");
            interactionClass(e, &fdd._interactionClasses[root->second]);
            return;
         }
         qualified = kInteractionRoot;
      }
      else
      {
         qualified = parent->qualified + "." + name;
         parentHandle = parent->handle;
      }

      std::uint64_t const handle = fnv1a64("I|" + qualified);
      checkCollision(fdd._interactionClasses, handle, qualified, label);

      std::string const transportationRaw = e.value("transportation");
      bool const definesProperties = !transportationRaw.empty() || !e.childrenNamed("parameter").empty();

      Fdd::InteractionClass& c = fdd._interactionClasses[handle];
      if (c.handle == 0)
      {
         c.handle = handle;
         c.name = name;
         c.qualified = qualified;
         c.parent = parentHandle;
         fdd._interactionClassByName[qualified] = handle;
      }
      if (definesProperties && handle != fnv1a64("I|" + kInteractionRoot))
      {
         std::string const transportation = normalizeTransportation(transportationRaw);
         std::string const order = normalizeOrder(e.value("order"), label);
         std::vector<std::string> const dims = readDimensionList(e);
         if (c.transportationSet &&
             (c.transportation != transportation || c.order != order || c.dimensions != dims))
            inconsistent(label, "interaction class " + qualified + " defined differently in another module");
         c.transportation = transportation;
         c.order = order;
         c.dimensions = dims;
         c.transportationSet = true;
      }

      std::vector<XmlElement const*> const parameters = e.childrenNamed("parameter");
      if (!parameters.empty() && !isStandardClass(qualified, kInteractionRoot))
      {
         if (!c.definedBy.empty())
         {
            std::vector<std::string> mine, theirs;
            for (XmlElement const* p : parameters)
               mine.push_back(p->value("name"));
            for (std::uint64_t h : c.parameters)
               theirs.push_back(fdd._parameters[h].name);
            std::sort(mine.begin(), mine.end());
            std::sort(theirs.begin(), theirs.end());
            if (mine != theirs)
               inconsistent(label,
                            "interaction class " + qualified + " defined with other parameters in " + c.definedBy);
         }
         else
            c.definedBy = label;
      }
      for (XmlElement const* p : parameters)
         parameter(*p, c);
      for (XmlElement const* sub : e.childrenNamed("interactionClass"))
         interactionClass(*sub, &fdd._interactionClasses[handle]);
   }

   void parameter(XmlElement const& e, Fdd::InteractionClass& c)
   {
      std::string const name = e.value("name");
      if (name.empty())
         reading(label, "parameter without a name in " + c.qualified);
      std::uint64_t const handle = fnv1a64("P|" + c.qualified + "|" + name);
      for (std::uint64_t p = c.parent; p != 0; p = fdd._interactionClasses[p].parent)
         for (std::uint64_t h : fdd._interactionClasses[p].parameters)
            if (fdd._parameters[h].name == name)
               inconsistent(label, "parameter '" + name + "' of " + c.qualified + " already declared by " +
                                      fdd._interactionClasses[p].qualified);
      auto it = fdd._parameters.find(handle);
      if (it != fdd._parameters.end())
      {
         if (it->second.declaringClass != c.handle || it->second.name != name)
            inconsistent(label, "handle collision for parameter '" + name + "' of " + c.qualified);
         return;
      }
      Fdd::Parameter p;
      p.handle = handle;
      p.name = name;
      p.declaringClass = c.handle;
      fdd._parameters[handle] = p;
      c.parameters.push_back(handle);
   }

   void dimension(XmlElement const& e)
   {
      std::string const name = e.value("name");
      if (name.empty())
         reading(label, "dimension without a name");
      std::string const ub = e.value("upperBound");
      // No upper bound (HLAfederate of the standard MIM, normalized by
      // normalizeFederateHandle): the whole 32-bit range.
      unsigned long upper = 0xFFFFFFFFul;
      if (!ub.empty() && ub != "NA")
      {
         char* end = nullptr;
         upper = std::strtoul(ub.c_str(), &end, 10);
         if (end == ub.c_str())
            reading(label, "bad upperBound '" + ub + "' for dimension " + name);
      }
      std::uint64_t const handle = fnv1a64("D|" + name);
      auto it = fdd._dimensions.find(handle);
      if (it != fdd._dimensions.end())
      {
         if (it->second.name != name)
            inconsistent(label, "handle collision for dimension " + name);
         if (it->second.upperBound != upper)
            inconsistent(label, "dimension " + name + " defined with another upper bound");
         return;
      }
      Fdd::Dimension d;
      d.handle = handle;
      d.name = name;
      d.upperBound = upper;
      fdd._dimensions[handle] = d;
      fdd._dimensionByName[name] = handle;
   }

   void updateRate(XmlElement const& e)
   {
      std::string const name = e.value("name");
      std::string const rate = e.value("rate");
      if (name.empty())
         reading(label, "updateRate without a name");
      char* end = nullptr;
      double const r = std::strtod(rate.c_str(), &end);
      if (rate.empty() || end == rate.c_str())
         reading(label, "bad rate '" + rate + "' for update rate " + name);
      auto it = fdd._updateRates.find(name);
      if (it != fdd._updateRates.end() && it->second != r)
         inconsistent(label, "update rate " + name + " defined with another rate");
      fdd._updateRates[name] = r;
   }
};

Fdd::Fdd()
{
   ObjectClass root;
   root.handle = fnv1a64("O|" + kObjectRoot);
   root.name = root.qualified = kObjectRoot;
   Attribute priv;
   priv.name = "HLAprivilegeToDeleteObject";
   priv.declaringClass = root.handle;
   priv.order = "TimeStamp";
   priv.builtin = true;
   priv.handle = fnv1a64("A|" + kObjectRoot + "|" + priv.name);
   root.attributes.push_back(priv.handle);
   _attributes[priv.handle] = priv;
   _objectClasses[root.handle] = root;
   _objectClassByName[kObjectRoot] = root.handle;

   InteractionClass iroot;
   iroot.handle = fnv1a64("I|" + kInteractionRoot);
   iroot.name = iroot.qualified = kInteractionRoot;
   _interactionClasses[iroot.handle] = iroot;
   _interactionClassByName[kInteractionRoot] = iroot.handle;
}

void Fdd::merge(std::string const& xmlDocument, std::string const& label)
{
   Fdd copy(*this);
   copy.mergeInPlace(xmlDocument, label);
   *this = std::move(copy);
}

void Fdd::mergeInPlace(std::string const& xmlDocument, std::string const& label)
{
   std::unique_ptr<XmlElement> root;
   try
   {
      root = parseXml(xmlDocument);
   }
   catch (XmlError const& e)
   {
      reading(label, e.what());
   }
   if (root->name != "objectModel")
      reading(label, "root element is <" + root->name + ">, <objectModel> expected");

   FddBuilder b{*this, label};
   if (XmlElement const* dims = root->child("dimensions"))
      for (XmlElement const* d : dims->childrenNamed("dimension"))
         b.dimension(*d);
   if (XmlElement const* switches = root->child("switches"))
      for (auto const& sw : switches->children)
      {
         auto it = sw->attributes.find("isEnabled");
         if (it != sw->attributes.end() && !_switches.count(sw->name))
            _switches[sw->name] = lower(trim(it->second)) == "true";
      }
   if (XmlElement const* rates = root->child("updateRates"))
      for (XmlElement const* r : rates->childrenNamed("updateRate"))
         b.updateRate(*r);
   if (XmlElement const* objects = root->child("objects"))
      for (XmlElement const* c : objects->childrenNamed("objectClass"))
         b.objectClass(*c, nullptr);
   if (XmlElement const* interactions = root->child("interactions"))
      for (XmlElement const* c : interactions->childrenNamed("interactionClass"))
         b.interactionClass(*c, nullptr);

   // Dimensions used by attributes and interactions must be defined.
   for (auto const& kv : _attributes)
      for (std::string const& d : kv.second.dimensions)
         if (!_dimensionByName.count(d))
            inconsistent(label, "attribute " + kv.second.name + " uses undefined dimension " + d);
   for (auto const& kv : _interactionClasses)
      for (std::string const& d : kv.second.dimensions)
         if (!_dimensionByName.count(d))
            inconsistent(label, "interaction " + kv.second.qualified + " uses undefined dimension " + d);
}

// ---------------------------------------------------------------- lookups
template <class Map>
static typename Map::mapped_type const* findIn(Map const& m, std::uint64_t h)
{
   auto it = m.find(h);
   return it == m.end() ? nullptr : &it->second;
}

// Qualified name -> handle: the only class whose qualified name ends with
// "." + name (0 when none or several).
static std::uint64_t uniqueSuffix(std::map<std::string, std::uint64_t> const& byName, std::string const& name)
{
   if (name.empty())
      return 0;
   std::string const suffix = "." + name;
   std::uint64_t found = 0;
   for (auto const& kv : byName)
      if (kv.first.size() > suffix.size() &&
          kv.first.compare(kv.first.size() - suffix.size(), suffix.size(), suffix) == 0)
      {
         if (found != 0)
            return 0;
         found = kv.second;
      }
   return found;
}

Fdd::ObjectClass const* Fdd::objectClass(std::uint64_t handle) const
{
   return findIn(_objectClasses, handle);
}

Fdd::ObjectClass const* Fdd::objectClassByName(std::string const& name) const
{
   auto it = _objectClassByName.find(name);
   if (it == _objectClassByName.end())
      it = _objectClassByName.find(kObjectRoot + "." + name);
   if (it != _objectClassByName.end())
      return objectClass(it->second);
   return objectClass(uniqueSuffix(_objectClassByName, name));
}

Fdd::Attribute const* Fdd::attribute(std::uint64_t handle) const
{
   return findIn(_attributes, handle);
}

Fdd::Attribute const* Fdd::findAttribute(std::uint64_t classHandle, std::string const& name) const
{
   for (ObjectClass const* c = objectClass(classHandle); c; c = c->parent ? objectClass(c->parent) : nullptr)
      for (std::uint64_t h : c->attributes)
         if (_attributes.at(h).name == name)
            return &_attributes.at(h);
   // Qualified by its declaring class.
   size_t const dot = name.rfind('.');
   if (dot == std::string::npos)
      return nullptr;
   ObjectClass const* declaring = objectClassByName(name.substr(0, dot));
   if (declaring == nullptr)
      return nullptr;
   for (ObjectClass const* c = objectClass(classHandle); c; c = c->parent ? objectClass(c->parent) : nullptr)
      if (c == declaring)
         for (std::uint64_t h : c->attributes)
            if (_attributes.at(h).name == name.substr(dot + 1))
               return &_attributes.at(h);
   return nullptr;
}

bool Fdd::classHasAttribute(std::uint64_t classHandle, std::uint64_t attributeHandle) const
{
   for (ObjectClass const* c = objectClass(classHandle); c; c = c->parent ? objectClass(c->parent) : nullptr)
      if (std::find(c->attributes.begin(), c->attributes.end(), attributeHandle) != c->attributes.end())
         return true;
   return false;
}

Fdd::InteractionClass const* Fdd::interactionClass(std::uint64_t handle) const
{
   return findIn(_interactionClasses, handle);
}

Fdd::InteractionClass const* Fdd::interactionClassByName(std::string const& name) const
{
   auto it = _interactionClassByName.find(name);
   if (it == _interactionClassByName.end())
      it = _interactionClassByName.find(kInteractionRoot + "." + name);
   if (it != _interactionClassByName.end())
      return interactionClass(it->second);
   return interactionClass(uniqueSuffix(_interactionClassByName, name));
}

Fdd::Parameter const* Fdd::parameter(std::uint64_t handle) const
{
   return findIn(_parameters, handle);
}

Fdd::Parameter const* Fdd::findParameter(std::uint64_t classHandle, std::string const& name) const
{
   for (InteractionClass const* c = interactionClass(classHandle); c;
        c = c->parent ? interactionClass(c->parent) : nullptr)
      for (std::uint64_t h : c->parameters)
         if (_parameters.at(h).name == name)
            return &_parameters.at(h);
   size_t const dot = name.rfind('.');
   if (dot == std::string::npos)
      return nullptr;
   InteractionClass const* declaring = interactionClassByName(name.substr(0, dot));
   if (declaring == nullptr)
      return nullptr;
   for (InteractionClass const* c = interactionClass(classHandle); c;
        c = c->parent ? interactionClass(c->parent) : nullptr)
      if (c == declaring)
         for (std::uint64_t h : c->parameters)
            if (_parameters.at(h).name == name.substr(dot + 1))
               return &_parameters.at(h);
   return nullptr;
}

bool Fdd::classHasParameter(std::uint64_t classHandle, std::uint64_t parameterHandle) const
{
   for (InteractionClass const* c = interactionClass(classHandle); c;
        c = c->parent ? interactionClass(c->parent) : nullptr)
      if (std::find(c->parameters.begin(), c->parameters.end(), parameterHandle) != c->parameters.end())
         return true;
   return false;
}

Fdd::Dimension const* Fdd::dimension(std::uint64_t handle) const
{
   return findIn(_dimensions, handle);
}

Fdd::Dimension const* Fdd::dimensionByName(std::string const& name) const
{
   auto it = _dimensionByName.find(name);
   return it == _dimensionByName.end() ? nullptr : dimension(it->second);
}

bool Fdd::updateRate(std::string const& name, double& rate) const
{
   auto it = _updateRates.find(name);
   if (it == _updateRates.end())
      return false;
   rate = it->second;
   return true;
}
} // namespace hla_mqtt
