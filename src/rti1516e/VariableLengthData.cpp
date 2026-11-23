// rti1516e::VariableLengthData (RTI/VariableLengthData.h).
// Binary layout imposed by the header: a single pointer `_impl`.

#include <RTI/VariableLengthData.h>

#include <cstring>
#include <vector>

namespace rti1516e
{
class VariableLengthDataImplementation
{
public:
   enum class Mode
   {
      Owned,    // copy held in `buffer`
      Borrowed, // setDataPointer: caller keeps ownership
      Taken     // takeDataPointer: we delete `external` with `deleter`
   };

   VariableLengthDataImplementation() = default;
   VariableLengthDataImplementation(VariableLengthDataImplementation const&) = delete;
   VariableLengthDataImplementation& operator=(VariableLengthDataImplementation const&) = delete;
   ~VariableLengthDataImplementation() { release(); }

   void release()
   {
      if (mode == Mode::Taken && external != nullptr)
      {
         if (deleter)
            deleter(external);
         else
            delete[] static_cast<char*>(external); // standard: array allocator assumed
      }
      external = nullptr;
      deleter = nullptr;
      externalSize = 0;
      buffer.clear();
      mode = Mode::Owned;
   }

   void const* data() const
   {
      if (mode == Mode::Owned)
         return buffer.empty() ? nullptr : buffer.data();
      return external;
   }

   size_t size() const { return mode == Mode::Owned ? buffer.size() : externalSize; }

   void copyFrom(void const* inData, size_t inSize)
   {
      // inData may point into our own storage: copy first, release after.
      std::vector<char> tmp;
      if (inData != nullptr && inSize > 0)
         tmp.assign(static_cast<char const*>(inData), static_cast<char const*>(inData) + inSize);
      release();
      buffer.swap(tmp);
   }

   Mode mode = Mode::Owned;
   std::vector<char> buffer;
   void* external = nullptr;
   size_t externalSize = 0;
   VariableLengthDataDeleteFunction deleter = nullptr;
};

VariableLengthData::VariableLengthData()
   : _impl(new VariableLengthDataImplementation)
{
}

VariableLengthData::VariableLengthData(void const* inData, size_t inSize)
   : _impl(new VariableLengthDataImplementation)
{
   _impl->copyFrom(inData, inSize);
}

VariableLengthData::VariableLengthData(VariableLengthData const& rhs)
   : _impl(new VariableLengthDataImplementation)
{
   _impl->copyFrom(rhs.data(), rhs.size());
}

VariableLengthData::~VariableLengthData()
{
   delete _impl;
}

VariableLengthData& VariableLengthData::operator=(VariableLengthData const& rhs)
{
   if (this != &rhs)
      _impl->copyFrom(rhs.data(), rhs.size());
   return *this;
}

void const* VariableLengthData::data() const
{
   return _impl->data();
}

size_t VariableLengthData::size() const
{
   return _impl->size();
}

void VariableLengthData::setData(void const* inData, size_t inSize)
{
   _impl->copyFrom(inData, inSize);
}

void VariableLengthData::setDataPointer(void* inData, size_t inSize)
{
   _impl->release();
   _impl->mode = VariableLengthDataImplementation::Mode::Borrowed;
   _impl->external = inData;
   _impl->externalSize = inSize;
}

void VariableLengthData::takeDataPointer(void* inData, size_t inSize,
                                         VariableLengthDataDeleteFunction func)
{
   _impl->release();
   _impl->mode = VariableLengthDataImplementation::Mode::Taken;
   _impl->external = inData;
   _impl->externalSize = inSize;
   _impl->deleter = func;
}
}
