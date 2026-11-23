// Constructed data elements: HLAfixedArray, HLAvariableArray, HLAfixedRecord,
// HLAvariantRecord, HLAopaqueData (RTI/encoding/*.h).
//
// Encodings (IEEE 1516.2-2010, 4.13):
//   HLAfixedArray    : the elements in sequence; boundary = element boundary
//   HLAvariableArray : HLAinteger32BE count, then the elements;
//                      boundary = max(4, element boundary)
//   HLAfixedRecord   : the fields in sequence; boundary = max of the fields
//   HLAvariantRecord : discriminant, then the selected alternative (if any);
//                      boundary = max of discriminant and all alternatives
//   HLAopaqueData    : HLAvariableArray of HLAbyte
// Each element is preceded by the padding that aligns it on its own boundary
// (EncodingHelpers.h); a constructed element also aligns its own start.

#include <RTI/encoding/HLAfixedArray.h>
#include <RTI/encoding/HLAfixedRecord.h>
#include <RTI/encoding/HLAopaqueData.h>
#include <RTI/encoding/HLAvariableArray.h>
#include <RTI/encoding/HLAvariantRecord.h>

#include "EncodingHelpers.h"

#include <algorithm>
#include <typeinfo>

namespace rti1516e
{
using namespace encoding;

namespace
{
// Ordered list of elements, each owned or caller-provided.
class ElementList
{
public:
   ElementList() = default;
   ElementList(ElementList const&) = delete;
   ElementList& operator=(ElementList const&) = delete;
   ~ElementList() { resize(0, nullptr); }

   // Deep copy: every element of `rhs` is cloned (internal memory).
   void copyFrom(ElementList const& rhs)
   {
      resize(0, nullptr);
      for (Slot const& s : rhs.slots)
         appendOwned(cloneOf(*s.element));
   }

   size_t size() const { return slots.size(); }
   DataElement& at(size_t index, wchar_t const* type) const
   {
      if (index >= slots.size())
         fail(std::wstring(type) + L": index " + std::to_wstring(index) + L" out of range (size " +
              std::to_wstring(slots.size()) + L")");
      return *slots[index].element;
   }

   void appendOwned(DataElement* e)
   {
      slots.push_back(Slot());
      slots.back().reset(e, true);
   }
   void appendPointer(DataElement* e)
   {
      slots.push_back(Slot());
      slots.back().reset(e, false);
   }

   // Copy of `value` into the slot: an owned slot gets a new copy, a slot
   // bound to external memory keeps it and receives the value.
   void assign(size_t index, DataElement const& value, wchar_t const* type)
   {
      at(index, type);
      Slot& s = slots[index];
      if (s.owned)
         s.reset(cloneOf(value), true);
      else
         copyValue(value, *s.element);
   }

   void setPointer(size_t index, DataElement* e, wchar_t const* type)
   {
      at(index, type);
      slots[index].reset(e, false);
   }

   // Shrinks (deleting owned elements) or grows with clones of `prototype`.
   void resize(size_t count, DataElement const* prototype)
   {
      while (slots.size() > count)
      {
         slots.back().reset(nullptr, false);
         slots.pop_back();
      }
      while (slots.size() < count)
         appendOwned(cloneOf(*prototype));
   }

   void encodeInto(Buffer& buffer) const
   {
      for (Slot const& s : slots)
         s.element->encodeInto(buffer);
   }

   size_t decodeFrom(Buffer const& buffer, size_t index)
   {
      for (Slot& s : slots)
         index = s.element->decodeFrom(buffer, index);
      return index;
   }

   unsigned int maxBoundary(unsigned int floor) const
   {
      unsigned int b = floor;
      for (Slot const& s : slots)
         b = std::max(b, s.element->getOctetBoundary());
      return b;
   }

   std::vector<Slot> slots;
};

void checkNotNull(DataElement const* e, wchar_t const* where)
{
   if (e == nullptr)
      fail(std::wstring(where) + L": null pointer");
}

// encode()/encode(VLD&)/decode(VLD)/getEncodedLength() from encodeInto/decodeFrom.
#define HLA_COMMON_ENCODING_MEMBERS(Cls) \
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
   void Cls::decode(VariableLengthData const& inData) RTI_THROW((EncoderException)) \
   { \
      decodeFrom(fromVariableLengthData(inData), 0); \
   } \
   size_t Cls::getEncodedLength() const RTI_THROW((EncoderException)) \
   { \
      Buffer b; \
      encodeInto(b); \
      return b.size(); \
   } \
   RTI_UNIQUE_PTR<DataElement> Cls::clone() const \
   { \
      return RTI_UNIQUE_PTR<DataElement>(new Cls(*this)); \
   }
} // namespace

// ============================================================ HLAfixedArray
class HLAfixedArrayImplementation
{
public:
   DataElement* prototype = nullptr;
   ElementList elements;
   ~HLAfixedArrayImplementation() { delete prototype; }
};

HLAfixedArray::HLAfixedArray(DataElement const& protoType, size_t length)
   : _impl(new HLAfixedArrayImplementation)
{
   _impl->prototype = cloneOf(protoType);
   _impl->elements.resize(length, _impl->prototype);
}

HLAfixedArray::HLAfixedArray(HLAfixedArray const& rhs) : DataElement(rhs), _impl(new HLAfixedArrayImplementation)
{
   _impl->prototype = cloneOf(*rhs._impl->prototype);
   _impl->elements.copyFrom(rhs._impl->elements);
}

HLAfixedArray::~HLAfixedArray()
{
   delete _impl;
}

HLA_COMMON_ENCODING_MEMBERS(HLAfixedArray)

void HLAfixedArray::encodeInto(std::vector<Octet>& buffer) const RTI_THROW((EncoderException))
{
   pad(buffer, getOctetBoundary());
   _impl->elements.encodeInto(buffer);
}

size_t HLAfixedArray::decodeFrom(std::vector<Octet> const& buffer, size_t index) RTI_THROW((EncoderException))
{
   index = alignAndCheck(buffer, index, getOctetBoundary(), 0, L"HLAfixedArray");
   return _impl->elements.decodeFrom(buffer, index);
}

unsigned int HLAfixedArray::getOctetBoundary() const
{
   return _impl->prototype->getOctetBoundary();
}

bool HLAfixedArray::isSameTypeAs(DataElement const& inData) const
{
   if (!DataElement::isSameTypeAs(inData))
      return false;
   HLAfixedArray const& other = static_cast<HLAfixedArray const&>(inData);
   return size() == other.size() && _impl->prototype->isSameTypeAs(*other._impl->prototype);
}

bool HLAfixedArray::hasPrototypeSameTypeAs(DataElement const& dataElement) const
{
   return _impl->prototype->isSameTypeAs(dataElement);
}

size_t HLAfixedArray::size() const
{
   return _impl->elements.size();
}

void HLAfixedArray::set(size_t index, DataElement const& dataElement) RTI_THROW((EncoderException))
{
   if (!hasPrototypeSameTypeAs(dataElement))
      fail(L"HLAfixedArray::set: element type differs from the prototype");
   _impl->elements.assign(index, dataElement, L"HLAfixedArray::set");
}

void HLAfixedArray::setElementPointer(size_t index, DataElement* dataElement) RTI_THROW((EncoderException))
{
   checkNotNull(dataElement, L"HLAfixedArray::setElementPointer");
   if (!hasPrototypeSameTypeAs(*dataElement))
      fail(L"HLAfixedArray::setElementPointer: element type differs from the prototype");
   _impl->elements.setPointer(index, dataElement, L"HLAfixedArray::setElementPointer");
}

DataElement const& HLAfixedArray::get(size_t index) const RTI_THROW((EncoderException))
{
   return _impl->elements.at(index, L"HLAfixedArray::get");
}

DataElement const& HLAfixedArray::operator[](size_t index) const RTI_THROW((EncoderException))
{
   return get(index);
}

// ========================================================= HLAvariableArray
class HLAvariableArrayImplementation
{
public:
   DataElement* prototype = nullptr;
   ElementList elements;
   ~HLAvariableArrayImplementation() { delete prototype; }
};

HLAvariableArray::HLAvariableArray(DataElement const& prototype) : _impl(new HLAvariableArrayImplementation)
{
   _impl->prototype = cloneOf(prototype);
}

HLAvariableArray::HLAvariableArray(HLAvariableArray const& rhs)
   : DataElement(rhs), _impl(new HLAvariableArrayImplementation)
{
   _impl->prototype = cloneOf(*rhs._impl->prototype);
   _impl->elements.copyFrom(rhs._impl->elements);
}

HLAvariableArray::~HLAvariableArray()
{
   delete _impl;
}

HLA_COMMON_ENCODING_MEMBERS(HLAvariableArray)

void HLAvariableArray::encodeInto(std::vector<Octet>& buffer) const RTI_THROW((EncoderException))
{
   pad(buffer, getOctetBoundary());
   putCount(buffer, _impl->elements.size());
   _impl->elements.encodeInto(buffer);
}

size_t HLAvariableArray::decodeFrom(std::vector<Octet> const& buffer, size_t index)
   RTI_THROW((EncoderException))
{
   index = alignAndCheck(buffer, index, getOctetBoundary(), 0, L"HLAvariableArray");
   size_t const count = getCount(buffer, index, L"HLAvariableArray");
   // Garbage guard: every element of a non-empty type takes at least one octet.
   if (count > buffer.size() - index && _impl->prototype->getEncodedLength() > 0)
      fail(L"not enough data to decode HLAvariableArray (count " + std::to_wstring(count) + L")");
   _impl->elements.resize(count, _impl->prototype);
   return _impl->elements.decodeFrom(buffer, index);
}

unsigned int HLAvariableArray::getOctetBoundary() const
{
   return std::max(4u, _impl->prototype->getOctetBoundary());
}

size_t HLAvariableArray::size() const
{
   return _impl->elements.size();
}

bool HLAvariableArray::isSameTypeAs(DataElement const& inData) const
{
   if (!DataElement::isSameTypeAs(inData))
      return false;
   HLAvariableArray const& other = static_cast<HLAvariableArray const&>(inData);
   return _impl->prototype->isSameTypeAs(*other._impl->prototype);
}

bool HLAvariableArray::hasPrototypeSameTypeAs(DataElement const& dataElement) const
{
   return _impl->prototype->isSameTypeAs(dataElement);
}

void HLAvariableArray::addElement(DataElement const& dataElement) RTI_THROW((EncoderException))
{
   if (!hasPrototypeSameTypeAs(dataElement))
      fail(L"HLAvariableArray::addElement: element type differs from the prototype");
   _impl->elements.appendOwned(cloneOf(dataElement));
}

void HLAvariableArray::addElementPointer(DataElement* dataElement) RTI_THROW((EncoderException))
{
   checkNotNull(dataElement, L"HLAvariableArray::addElementPointer");
   if (!hasPrototypeSameTypeAs(*dataElement))
      fail(L"HLAvariableArray::addElementPointer: element type differs from the prototype");
   _impl->elements.appendPointer(dataElement);
}

void HLAvariableArray::set(size_t index, DataElement const& dataElement) RTI_THROW((EncoderException))
{
   if (!hasPrototypeSameTypeAs(dataElement))
      fail(L"HLAvariableArray::set: element type differs from the prototype");
   _impl->elements.assign(index, dataElement, L"HLAvariableArray::set");
}

void HLAvariableArray::setElementPointer(size_t index, DataElement* dataElement) RTI_THROW((EncoderException))
{
   checkNotNull(dataElement, L"HLAvariableArray::setElementPointer");
   if (!hasPrototypeSameTypeAs(*dataElement))
      fail(L"HLAvariableArray::setElementPointer: element type differs from the prototype");
   _impl->elements.setPointer(index, dataElement, L"HLAvariableArray::setElementPointer");
}

DataElement const& HLAvariableArray::get(size_t index) const RTI_THROW((EncoderException))
{
   return _impl->elements.at(index, L"HLAvariableArray::get");
}

DataElement const& HLAvariableArray::operator[](size_t index) const RTI_THROW((EncoderException))
{
   return get(index);
}

// =========================================================== HLAfixedRecord
class HLAfixedRecordImplementation
{
public:
   ElementList elements;
};

HLAfixedRecord::HLAfixedRecord() : _impl(new HLAfixedRecordImplementation) {}

HLAfixedRecord::HLAfixedRecord(HLAfixedRecord const& rhs) : DataElement(rhs), _impl(new HLAfixedRecordImplementation)
{
   _impl->elements.copyFrom(rhs._impl->elements);
}

HLAfixedRecord::~HLAfixedRecord()
{
   delete _impl;
}

HLA_COMMON_ENCODING_MEMBERS(HLAfixedRecord)

void HLAfixedRecord::encodeInto(std::vector<Octet>& buffer) const RTI_THROW((EncoderException))
{
   pad(buffer, getOctetBoundary());
   _impl->elements.encodeInto(buffer);
}

size_t HLAfixedRecord::decodeFrom(std::vector<Octet> const& buffer, size_t index) RTI_THROW((EncoderException))
{
   index = alignAndCheck(buffer, index, getOctetBoundary(), 0, L"HLAfixedRecord");
   return _impl->elements.decodeFrom(buffer, index);
}

unsigned int HLAfixedRecord::getOctetBoundary() const
{
   return _impl->elements.maxBoundary(1);
}

bool HLAfixedRecord::isSameTypeAs(DataElement const& inData) const
{
   if (!DataElement::isSameTypeAs(inData))
      return false;
   HLAfixedRecord const& other = static_cast<HLAfixedRecord const&>(inData);
   if (size() != other.size())
      return false;
   for (size_t i = 0; i < size(); ++i)
      if (!_impl->elements.slots[i].element->isSameTypeAs(*other._impl->elements.slots[i].element))
         return false;
   return true;
}

bool HLAfixedRecord::hasElementSameTypeAs(size_t index, DataElement const& inData) const
{
   return index < size() && _impl->elements.slots[index].element->isSameTypeAs(inData);
}

size_t HLAfixedRecord::size() const
{
   return _impl->elements.size();
}

void HLAfixedRecord::appendElement(DataElement const& dataElement)
{
   _impl->elements.appendOwned(cloneOf(dataElement));
}

void HLAfixedRecord::appendElementPointer(DataElement* dataElement)
{
   checkNotNull(dataElement, L"HLAfixedRecord::appendElementPointer");
   _impl->elements.appendPointer(dataElement);
}

void HLAfixedRecord::set(size_t index, DataElement const& dataElement) RTI_THROW((EncoderException))
{
   _impl->elements.at(index, L"HLAfixedRecord::set");
   if (!hasElementSameTypeAs(index, dataElement))
      fail(L"HLAfixedRecord::set: element type differs from field " + std::to_wstring(index));
   _impl->elements.assign(index, dataElement, L"HLAfixedRecord::set");
}

void HLAfixedRecord::setElementPointer(size_t index, DataElement* dataElement) RTI_THROW((EncoderException))
{
   checkNotNull(dataElement, L"HLAfixedRecord::setElementPointer");
   _impl->elements.at(index, L"HLAfixedRecord::setElementPointer");
   if (!hasElementSameTypeAs(index, *dataElement))
      fail(L"HLAfixedRecord::setElementPointer: element type differs from field " + std::to_wstring(index));
   _impl->elements.setPointer(index, dataElement, L"HLAfixedRecord::setElementPointer");
}

DataElement const& HLAfixedRecord::get(size_t index) const RTI_THROW((EncoderException))
{
   return _impl->elements.at(index, L"HLAfixedRecord::get");
}

DataElement const& HLAfixedRecord::operator[](size_t index) const RTI_THROW((EncoderException))
{
   return get(index);
}

// ========================================================= HLAvariantRecord
class HLAvariantRecordImplementation
{
public:
   struct Variant
   {
      Buffer key;           // encoding of the discriminant value
      Slot value;
   };

   DataElement* discriminant = nullptr; // current discriminant value
   std::vector<Variant> variants;
   long selected = -1;                  // index in variants, -1 = no alternative

   ~HLAvariantRecordImplementation()
   {
      for (Variant& v : variants)
         v.value.reset(nullptr, false);
      delete discriminant;
   }

   static Buffer keyOf(DataElement const& d)
   {
      Buffer b;
      d.encodeInto(b);
      return b;
   }

   long find(Buffer const& key) const
   {
      for (size_t i = 0; i < variants.size(); ++i)
         if (variants[i].key == key)
            return long(i);
      return -1;
   }

   void checkDiscriminantType(DataElement const& d, wchar_t const* where) const
   {
      if (!discriminant->isSameTypeAs(d))
         fail(std::wstring(where) + L": discriminant type differs from the record's discriminant");
   }

   Variant& variantFor(DataElement const& d, wchar_t const* where)
   {
      checkDiscriminantType(d, where);
      long const i = find(keyOf(d));
      if (i < 0)
         fail(std::wstring(where) + L": no alternative for this discriminant value");
      return variants[size_t(i)];
   }

   void addVariant(DataElement const& d, DataElement* value, bool owned, wchar_t const* where)
   {
      checkDiscriminantType(d, where);
      Buffer key = keyOf(d);
      if (find(key) >= 0)
      {
         if (owned)
            delete value;
         fail(std::wstring(where) + L": an alternative already exists for this discriminant value");
      }
      variants.push_back(Variant());
      variants.back().key.swap(key);
      variants.back().value.reset(value, owned);
      selected = find(keyOf(*discriminant));
   }
};

HLAvariantRecord::HLAvariantRecord(DataElement const& discriminantPrototype)
   : _impl(new HLAvariantRecordImplementation)
{
   _impl->discriminant = cloneOf(discriminantPrototype);
}

HLAvariantRecord::HLAvariantRecord(HLAvariantRecord const& rhs)
   : DataElement(rhs), _impl(new HLAvariantRecordImplementation)
{
   _impl->discriminant = cloneOf(*rhs._impl->discriminant);
   for (auto const& v : rhs._impl->variants)
   {
      _impl->variants.push_back(HLAvariantRecordImplementation::Variant());
      _impl->variants.back().key = v.key;
      _impl->variants.back().value.reset(cloneOf(*v.value.element), true);
   }
   _impl->selected = rhs._impl->selected;
}

HLAvariantRecord::~HLAvariantRecord()
{
   delete _impl;
}

HLA_COMMON_ENCODING_MEMBERS(HLAvariantRecord)

void HLAvariantRecord::encodeInto(std::vector<Octet>& buffer) const RTI_THROW((EncoderException))
{
   pad(buffer, getOctetBoundary());
   _impl->discriminant->encodeInto(buffer);
   if (_impl->selected >= 0)
      _impl->variants[size_t(_impl->selected)].value.element->encodeInto(buffer);
}

size_t HLAvariantRecord::decodeFrom(std::vector<Octet> const& buffer, size_t index)
   RTI_THROW((EncoderException))
{
   index = alignAndCheck(buffer, index, getOctetBoundary(), 0, L"HLAvariantRecord");
   index = _impl->discriminant->decodeFrom(buffer, index);
   _impl->selected = _impl->find(HLAvariantRecordImplementation::keyOf(*_impl->discriminant));
   if (_impl->selected >= 0)
      index = _impl->variants[size_t(_impl->selected)].value.element->decodeFrom(buffer, index);
   return index;
}

unsigned int HLAvariantRecord::getOctetBoundary() const
{
   unsigned int b = _impl->discriminant->getOctetBoundary();
   for (auto const& v : _impl->variants)
      b = std::max(b, v.value.element->getOctetBoundary());
   return b;
}

bool HLAvariantRecord::isSameTypeAs(DataElement const& inData) const
{
   if (!DataElement::isSameTypeAs(inData))
      return false;
   HLAvariantRecord const& other = static_cast<HLAvariantRecord const&>(inData);
   if (!_impl->discriminant->isSameTypeAs(*other._impl->discriminant) ||
       _impl->variants.size() != other._impl->variants.size())
      return false;
   for (auto const& v : _impl->variants)
   {
      long const j = other._impl->find(v.key);
      if (j < 0 || !v.value.element->isSameTypeAs(*other._impl->variants[size_t(j)].value.element))
         return false;
   }
   return true;
}

bool HLAvariantRecord::isSameTypeAs(DataElement const& discriminant, DataElement const& inData) const
   RTI_THROW((EncoderException))
{
   return _impl->variantFor(discriminant, L"HLAvariantRecord::isSameTypeAs").value.element->isSameTypeAs(inData);
}

bool HLAvariantRecord::hasMatchingDiscriminantTypeAs(DataElement const& dataElement) const
{
   return _impl->discriminant->isSameTypeAs(dataElement);
}

void HLAvariantRecord::addVariant(DataElement const& discriminant, DataElement const& valuePrototype)
   RTI_THROW((EncoderException))
{
   _impl->addVariant(discriminant, cloneOf(valuePrototype), true, L"HLAvariantRecord::addVariant");
}

void HLAvariantRecord::addVariantPointer(DataElement const& discriminant, DataElement* valuePtr)
   RTI_THROW((EncoderException))
{
   checkNotNull(valuePtr, L"HLAvariantRecord::addVariantPointer");
   _impl->addVariant(discriminant, valuePtr, false, L"HLAvariantRecord::addVariantPointer");
}

void HLAvariantRecord::setDiscriminant(DataElement const& discriminant) RTI_THROW((EncoderException))
{
   _impl->checkDiscriminantType(discriminant, L"HLAvariantRecord::setDiscriminant");
   copyValue(discriminant, *_impl->discriminant);
   _impl->selected = _impl->find(HLAvariantRecordImplementation::keyOf(*_impl->discriminant));
}

void HLAvariantRecord::setVariant(DataElement const& discriminant, DataElement const& value)
   RTI_THROW((EncoderException))
{
   auto& variant = _impl->variantFor(discriminant, L"HLAvariantRecord::setVariant");
   if (!variant.value.element->isSameTypeAs(value))
      fail(L"HLAvariantRecord::setVariant: value type differs from the alternative");
   if (variant.value.owned)
      variant.value.reset(cloneOf(value), true);
   else
      copyValue(value, *variant.value.element);
   setDiscriminant(discriminant);
}

void HLAvariantRecord::setVariantPointer(DataElement const& discriminant, DataElement* valuePtr)
   RTI_THROW((EncoderException))
{
   checkNotNull(valuePtr, L"HLAvariantRecord::setVariantPointer");
   auto& variant = _impl->variantFor(discriminant, L"HLAvariantRecord::setVariantPointer");
   if (!variant.value.element->isSameTypeAs(*valuePtr))
      fail(L"HLAvariantRecord::setVariantPointer: value type differs from the alternative");
   variant.value.reset(valuePtr, false);
   setDiscriminant(discriminant);
}

DataElement const& HLAvariantRecord::getDiscriminant() const
{
   return *_impl->discriminant;
}

DataElement const& HLAvariantRecord::getVariant() const RTI_THROW((EncoderException))
{
   if (_impl->selected < 0)
      fail(L"HLAvariantRecord::getVariant: no alternative for the current discriminant");
   return *_impl->variants[size_t(_impl->selected)].value.element;
}

// ============================================================ HLAopaqueData
class HLAopaqueDataImplementation
{
public:
   Buffer internal;
   Octet** external = nullptr; // caller's buffer pointer (setDataPointer)
   size_t externalBufferSize = 0;
   size_t externalDataSize = 0;

   Octet const* data() const
   {
      if (external)
         return *external;
      return internal.empty() ? nullptr : internal.data();
   }
   size_t size() const { return external ? externalDataSize : internal.size(); }

   // External memory: a buffer too small is replaced by a new Octet[]
   // (delete[] on the old one), and *external is updated. This is why the
   // API takes an Octet**.
   void assign(Octet const* in, size_t n)
   {
      if (!external)
      {
         internal.assign(in, in + n);
         return;
      }
      if (n > externalBufferSize || *external == nullptr)
      {
         Octet* fresh = new Octet[n > 0 ? n : 1];
         delete[] *external;
         *external = fresh;
         externalBufferSize = n;
      }
      if (n > 0)
         std::memmove(*external, in, n);
      externalDataSize = n;
   }

   static void checkExternal(Octet** inData, size_t bufferSize, size_t dataSize, wchar_t const* where)
   {
      if (inData == nullptr)
         fail(std::wstring(where) + L": null pointer");
      if (dataSize > bufferSize)
         fail(std::wstring(where) + L": dataSize larger than bufferSize");
      if (*inData == nullptr && bufferSize > 0)
         fail(std::wstring(where) + L": null buffer with a non-zero bufferSize");
   }
};

HLAopaqueData::HLAopaqueData() : _impl(new HLAopaqueDataImplementation) {}

HLAopaqueData::HLAopaqueData(Octet const* inData, size_t dataSize) : _impl(new HLAopaqueDataImplementation)
{
   if (inData != nullptr)
      _impl->internal.assign(inData, inData + dataSize);
}

HLAopaqueData::HLAopaqueData(Octet** inData, size_t bufferSize, size_t dataSize) RTI_THROW((EncoderException))
   : _impl(new HLAopaqueDataImplementation)
{
   try
   {
      HLAopaqueDataImplementation::checkExternal(inData, bufferSize, dataSize, L"HLAopaqueData");
   }
   catch (...)
   {
      delete _impl;
      throw;
   }
   _impl->external = inData;
   _impl->externalBufferSize = bufferSize;
   _impl->externalDataSize = dataSize;
}

HLAopaqueData::HLAopaqueData(HLAopaqueData const& rhs) : DataElement(rhs), _impl(new HLAopaqueDataImplementation)
{
   Octet const* p = rhs._impl->data();
   if (p != nullptr)
      _impl->internal.assign(p, p + rhs._impl->size());
}

HLAopaqueData::~HLAopaqueData()
{
   delete _impl;
}

HLA_COMMON_ENCODING_MEMBERS(HLAopaqueData)

void HLAopaqueData::encodeInto(std::vector<Octet>& buffer) const RTI_THROW((EncoderException))
{
   putCount(buffer, _impl->size());
   Octet const* p = _impl->data();
   if (p != nullptr)
      buffer.insert(buffer.end(), p, p + _impl->size());
}

size_t HLAopaqueData::decodeFrom(std::vector<Octet> const& buffer, size_t index) RTI_THROW((EncoderException))
{
   size_t const count = getCount(buffer, index, L"HLAopaqueData");
   if (buffer.size() - index < count)
      fail(L"not enough data to decode HLAopaqueData (count " + std::to_wstring(count) + L")");
   _impl->assign(buffer.data() + index, count);
   return index + count;
}

unsigned int HLAopaqueData::getOctetBoundary() const
{
   return 4;
}

size_t HLAopaqueData::bufferLength() const
{
   return _impl->external ? _impl->externalBufferSize : _impl->internal.size();
}

size_t HLAopaqueData::dataLength() const
{
   return _impl->size();
}

void HLAopaqueData::setDataPointer(Octet** inData, size_t bufferSize, size_t dataSize) RTI_THROW((EncoderException))
{
   HLAopaqueDataImplementation::checkExternal(inData, bufferSize, dataSize, L"HLAopaqueData::setDataPointer");
   _impl->internal.clear();
   _impl->external = inData;
   _impl->externalBufferSize = bufferSize;
   _impl->externalDataSize = dataSize;
}

void HLAopaqueData::set(Octet const* inData, size_t dataSize)
{
   if (inData == nullptr)
      dataSize = 0;
   _impl->assign(inData, dataSize);
}

Octet const* HLAopaqueData::get() const
{
   return _impl->data();
}

HLAopaqueData::operator Octet const*() const
{
   return _impl->data();
}
} // namespace rti1516e
