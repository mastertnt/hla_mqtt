// hla-encoding-test: encoding helpers (RTI/encoding) and standard logical
// times (RTI/time) through the standard API only. No broker needed.
// Expected octets: IEEE 1516.2-2010, 4.13 (alignment on each element's
// octet boundary, counts as HLAinteger32BE).
//
// Exit code 0 = all checks passed.

#include <RTI/RTI1516.h>
#include <RTI/encoding/BasicDataElements.h>
#include <RTI/encoding/HLAfixedArray.h>
#include <RTI/encoding/HLAfixedRecord.h>
#include <RTI/encoding/HLAopaqueData.h>
#include <RTI/encoding/HLAvariableArray.h>
#include <RTI/encoding/HLAvariantRecord.h>
#include <RTI/time/HLAfloat64Interval.h>
#include <RTI/time/HLAfloat64Time.h>
#include <RTI/time/HLAfloat64TimeFactory.h>
#include <RTI/time/HLAinteger64Interval.h>
#include <RTI/time/HLAinteger64Time.h>
#include <RTI/time/HLAinteger64TimeFactory.h>

#include <cstdio>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

using namespace rti1516e;

namespace
{
int failures = 0;
int checks = 0;

void check(bool ok, std::string const& what)
{
   ++checks;
   if (!ok)
      ++failures;
   std::cout << (ok ? "[PASS] " : "[FAIL] ") << what << "\n";
}

std::string hex(std::vector<Octet> const& v)
{
   std::string s;
   char b[4];
   for (Octet o : v)
   {
      std::snprintf(b, sizeof b, "%02X", static_cast<unsigned char>(o));
      if (!s.empty())
         s += ' ';
      s += b;
   }
   return s;
}

std::vector<Octet> bytes(std::initializer_list<int> l)
{
   std::vector<Octet> v;
   for (int i : l)
      v.push_back(Octet(i));
   return v;
}

std::vector<Octet> encoded(DataElement const& e)
{
   VariableLengthData d = e.encode();
   Octet const* p = static_cast<Octet const*>(d.data());
   return p ? std::vector<Octet>(p, p + d.size()) : std::vector<Octet>();
}

// Encoding equals `expected`, getEncodedLength agrees, and decoding the
// octets into a clone gives back the same encoding.
void checkEncoding(DataElement const& e, std::vector<Octet> const& expected, std::string const& what)
{
   std::vector<Octet> const got = encoded(e);
   bool ok = got == expected && e.getEncodedLength() == expected.size();
   std::string detail;
   if (!ok)
      detail = " -- got [" + hex(got) + "] expected [" + hex(expected) + "]";
   else
   {
      RTI_UNIQUE_PTR<DataElement> copy = e.clone();
      size_t const next = copy->decodeFrom(expected, 0);
      ok = next == expected.size() && encoded(*copy) == expected;
      if (!ok)
         detail = " -- round trip failed";
   }
   check(ok, what + detail);
}

template <class E>
bool throws(std::function<void()> f)
{
   try
   {
      f();
   }
   catch (E const&)
   {
      return true;
   }
   catch (...)
   {
      return false;
   }
   return false;
}

void basicTypes()
{
   checkEncoding(HLAinteger16BE(-2), bytes({0xFF, 0xFE}), "HLAinteger16BE(-2)");
   checkEncoding(HLAinteger16LE(0x0102), bytes({0x02, 0x01}), "HLAinteger16LE(0x0102)");
   checkEncoding(HLAinteger32BE(0x01020304), bytes({1, 2, 3, 4}), "HLAinteger32BE");
   checkEncoding(HLAinteger32LE(0x01020304), bytes({4, 3, 2, 1}), "HLAinteger32LE");
   checkEncoding(HLAinteger64BE(1), bytes({0, 0, 0, 0, 0, 0, 0, 1}), "HLAinteger64BE(1)");
   checkEncoding(HLAinteger64LE(-1), bytes({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}), "HLAinteger64LE(-1)");
   checkEncoding(HLAfloat32BE(1.0f), bytes({0x3F, 0x80, 0, 0}), "HLAfloat32BE(1.0)");
   checkEncoding(HLAfloat32LE(1.0f), bytes({0, 0, 0x80, 0x3F}), "HLAfloat32LE(1.0)");
   checkEncoding(HLAfloat64BE(1.0), bytes({0x3F, 0xF0, 0, 0, 0, 0, 0, 0}), "HLAfloat64BE(1.0)");
   checkEncoding(HLAfloat64LE(-2.0), bytes({0, 0, 0, 0, 0, 0, 0, 0xC0}), "HLAfloat64LE(-2.0)");
   checkEncoding(HLAboolean(true), bytes({0, 0, 0, 1}), "HLAboolean(true) = HLAinteger32BE 1");
   checkEncoding(HLAboolean(false), bytes({0, 0, 0, 0}), "HLAboolean(false)");
   checkEncoding(HLAoctet(Octet(0x7F)), bytes({0x7F}), "HLAoctet");
   checkEncoding(HLAbyte(Octet(0x10)), bytes({0x10}), "HLAbyte");
   checkEncoding(HLAASCIIchar('A'), bytes({0x41}), "HLAASCIIchar");
   checkEncoding(HLAunicodeChar(L'é'), bytes({0x00, 0xE9}), "HLAunicodeChar (UTF-16BE)");
   checkEncoding(HLAoctetPairBE(OctetPair('a', 'b')), bytes({0x61, 0x62}), "HLAoctetPairBE");
   checkEncoding(HLAoctetPairLE(OctetPair('a', 'b')), bytes({0x62, 0x61}), "HLAoctetPairLE");
   checkEncoding(HLAASCIIstring("AB"), bytes({0, 0, 0, 2, 0x41, 0x42}), "HLAASCIIstring(\"AB\")");
   checkEncoding(HLAASCIIstring(""), bytes({0, 0, 0, 0}), "HLAASCIIstring(\"\")");
   checkEncoding(HLAunicodeString(L"aé"), bytes({0, 0, 0, 2, 0, 0x61, 0, 0xE9}), "HLAunicodeString");

   HLAinteger32BE i(42);
   check(i.get() == 42 && Integer32(i) == 42, "HLAinteger32BE get / conversion operator");
   i = 7;
   check(i.get() == 7, "HLAinteger32BE operator=(value)");
   check(HLAinteger32BE(5).getOctetBoundary() == 4 && HLAfloat64LE().getOctetBoundary() == 8 &&
            HLAASCIIstring().getOctetBoundary() == 4 && HLAoctetPairBE().getOctetBoundary() == 2 &&
            HLAoctet().getOctetBoundary() == 1,
         "basic octet boundaries");

   // External memory
   Integer32 external = 5;
   HLAinteger32BE bound(&external);
   bound.decode(VariableLengthData("\x00\x00\x00\x07", 4));
   check(external == 7, "external memory updated by decode");
   bound.set(9);
   check(external == 9, "external memory updated by set");
   HLAinteger32BE copy(bound);
   copy.set(1);
   check(external == 9 && copy.get() == 1, "copy constructor uses internal memory");
   check(throws<EncoderException>([&] { bound.setDataPointer(nullptr); }), "setDataPointer(null) -> EncoderException");

   check(throws<EncoderException>([] {
            HLAinteger32BE x;
            x.decode(VariableLengthData("\x00\x01\x02", 3));
         }),
         "decode with missing octets -> EncoderException");
   check(throws<EncoderException>([] {
            HLAASCIIstring s;
            s.decode(VariableLengthData("\x00\x00\x00\x09xy", 6));
         }),
         "string count beyond the data -> EncoderException");

   check(!HLAinteger32BE().isSameTypeAs(HLAinteger32LE()) && HLAinteger32BE(1).isSameTypeAs(HLAinteger32BE(2)),
         "isSameTypeAs on basic types");
   check(HLAinteger32BE(3).hash() == HLAinteger32BE(3).hash() && HLAinteger32BE(3).hash() != HLAinteger32BE(4).hash(),
         "hash follows the value");
   RTI_UNIQUE_PTR<DataElement> cl = HLAASCIIstring("clone").clone();
   check(dynamic_cast<HLAASCIIstring*>(cl.get()) && dynamic_cast<HLAASCIIstring&>(*cl).get() == "clone",
         "clone keeps type and value");
}

void constructedTypes()
{
   // Fixed record {HLAoctet, HLAinteger64BE}: 7 octets of padding.
   HLAfixedRecord rec;
   rec.appendElement(HLAoctet(Octet(1)));
   rec.appendElement(HLAinteger64BE(2));
   checkEncoding(rec, bytes({1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2}), "HLAfixedRecord {octet, int64BE} alignment");
   check(rec.getOctetBoundary() == 8 && rec.size() == 2, "HLAfixedRecord boundary = max of fields");

   HLAfixedRecord rec2;
   rec2.appendElement(HLAoctet(Octet(7)));
   rec2.appendElement(HLAASCIIstring("x"));
   checkEncoding(rec2, bytes({7, 0, 0, 0, 0, 0, 0, 1, 0x78}), "HLAfixedRecord {octet, string}");
   check(throws<EncoderException>([&] { rec2.set(1, HLAinteger32BE(1)); }), "HLAfixedRecord::set wrong type -> EncoderException");
   rec2.set(1, HLAASCIIstring("yz"));
   check(dynamic_cast<HLAASCIIstring const&>(rec2.get(1)).get() == "yz", "HLAfixedRecord::set / get");

   // Variable arrays
   HLAvariableArray a16((HLAinteger16BE()));
   a16.addElement(HLAinteger16BE(1));
   a16.addElement(HLAinteger16BE(2));
   a16.addElement(HLAinteger16BE(3));
   checkEncoding(a16, bytes({0, 0, 0, 3, 0, 1, 0, 2, 0, 3}), "HLAvariableArray of HLAinteger16BE");
   check(a16.getOctetBoundary() == 4, "HLAvariableArray boundary >= 4");

   HLAvariableArray a64((HLAinteger64BE()));
   a64.addElement(HLAinteger64BE(5));
   checkEncoding(a64, bytes({0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5}), "HLAvariableArray of HLAinteger64BE (padding after count)");
   check(throws<EncoderException>([&] { a16.addElement(HLAinteger32BE(1)); }), "HLAvariableArray::addElement wrong type -> EncoderException");

   HLAvariableArray decoded((HLAinteger16BE()));
   decoded.decode(a16.encode());
   check(decoded.size() == 3 && dynamic_cast<HLAinteger16BE const&>(decoded[2]).get() == 3, "HLAvariableArray decode resizes");
   decoded.decode(HLAvariableArray(HLAinteger16BE()).encode());
   check(decoded.size() == 0, "HLAvariableArray decode shrinks");
   check(throws<EncoderException>([] {
            HLAvariableArray v((HLAinteger32BE()));
            v.decode(VariableLengthData("\x00\x00\x00\x05\x00\x00\x00\x01", 8));
         }),
         "HLAvariableArray truncated -> EncoderException");

   // Element pointers
   Integer32 x = 0, y = 0;
   HLAinteger32BE ex(&x), ey(&y);
   HLAfixedArray fa(HLAinteger32BE(), 2);
   fa.setElementPointer(0, &ex);
   fa.setElementPointer(1, &ey);
   fa.decode(VariableLengthData("\x00\x00\x00\x0A\x00\x00\x00\x0B", 8));
   check(x == 10 && y == 11, "HLAfixedArray decodes into element pointers");
   fa.set(0, HLAinteger32BE(12));
   check(x == 12, "HLAfixedArray::set on an element pointer keeps the external memory");
   checkEncoding(fa, bytes({0, 0, 0, 12, 0, 0, 0, 11}), "HLAfixedArray of HLAinteger32BE (no count)");
   check(throws<EncoderException>([&] { fa.get(2); }), "HLAfixedArray::get out of range -> EncoderException");
   check(fa.isSameTypeAs(HLAfixedArray(HLAinteger32BE(), 2)) && !fa.isSameTypeAs(HLAfixedArray(HLAinteger32BE(), 3)),
         "HLAfixedArray::isSameTypeAs checks length");

   // Nested: variable array of records
   HLAvariableArray recs(rec);
   recs.addElement(rec);
   recs.addElement(rec);
   std::vector<Octet> expected = bytes({0, 0, 0, 2, 0, 0, 0, 0});
   for (int k = 0; k < 2; ++k)
   {
      std::vector<Octet> r = bytes({1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2});
      expected.insert(expected.end(), r.begin(), r.end());
   }
   checkEncoding(recs, expected, "HLAvariableArray of records (boundary 8)");

   // Variant record: discriminant HLAinteger32BE, 1 -> HLAfloat64BE, 2 -> HLAASCIIstring
   HLAvariantRecord v((HLAinteger32BE()));
   v.addVariant(HLAinteger32BE(1), HLAfloat64BE());
   v.addVariant(HLAinteger32BE(2), HLAASCIIstring());
   v.setVariant(HLAinteger32BE(1), HLAfloat64BE(2.5));
   checkEncoding(v, bytes({0, 0, 0, 1, 0, 0, 0, 0, 0x40, 0x04, 0, 0, 0, 0, 0, 0}), "HLAvariantRecord alternative 1 (float64)");
   v.setVariant(HLAinteger32BE(2), HLAASCIIstring("hi"));
   checkEncoding(v, bytes({0, 0, 0, 2, 0, 0, 0, 2, 0x68, 0x69}), "HLAvariantRecord alternative 2 (string)");

   HLAvariantRecord w((HLAinteger32BE()));
   w.addVariant(HLAinteger32BE(1), HLAfloat64BE());
   w.addVariant(HLAinteger32BE(2), HLAASCIIstring());
   w.decode(v.encode());
   check(dynamic_cast<HLAinteger32BE const&>(w.getDiscriminant()).get() == 2 &&
            dynamic_cast<HLAASCIIstring const&>(w.getVariant()).get() == "hi",
         "HLAvariantRecord decode selects the alternative");
   check(w.isSameTypeAs(v) && w.isSameTypeAs(HLAinteger32BE(1), HLAfloat64BE()), "HLAvariantRecord::isSameTypeAs");
   w.setDiscriminant(HLAinteger32BE(3));
   checkEncoding(w, bytes({0, 0, 0, 3}), "HLAvariantRecord discriminant without alternative");
   check(throws<EncoderException>([&] { w.getVariant(); }), "getVariant without alternative -> EncoderException");
   check(throws<EncoderException>([&] { w.setVariant(HLAinteger32BE(1), HLAASCIIstring("bad")); }),
         "setVariant with the wrong value type -> EncoderException");
   check(throws<EncoderException>([&] { w.setDiscriminant(HLAinteger16BE(1)); }),
         "setDiscriminant with the wrong type -> EncoderException");
   check(throws<EncoderException>([&] { w.addVariant(HLAinteger32BE(1), HLAfloat64BE()); }),
         "addVariant twice for one discriminant -> EncoderException");

   // Opaque data
   Octet const raw[] = {1, 2, 3};
   HLAopaqueData op(raw, 3);
   checkEncoding(op, bytes({0, 0, 0, 3, 1, 2, 3}), "HLAopaqueData");
   check(op.dataLength() == 3 && op.get()[2] == 3, "HLAopaqueData dataLength / get");

   Octet* buf = new Octet[1];
   Octet* const original = buf;
   HLAopaqueData ext(&buf, 1, 0);
   ext.decode(op.encode());
   check(buf != original && ext.dataLength() == 3 && ext.bufferLength() == 3 && buf[0] == 1 && buf[2] == 3,
         "HLAopaqueData external buffer grown on decode");
   delete[] buf;
   check(throws<EncoderException>([] { HLAopaqueData bad(nullptr, 0, 0); }), "HLAopaqueData(null, ...) -> EncoderException");
}

void times()
{
   HLAinteger64TimeFactory ifac;
   check(ifac.getName() == L"HLAinteger64Time", "HLAinteger64TimeFactory::getName");
   HLAinteger64Time t(10);
   t += HLAinteger64Interval(5);
   check(t.getTime() == 15, "HLAinteger64Time += interval");
   VariableLengthData e = t.encode();
   check(e.size() == 8 && static_cast<unsigned char const*>(e.data())[7] == 15 && t.encodedLength() == 8,
         "HLAinteger64Time encoding (HLAinteger64BE)");
   RTI_UNIQUE_PTR<LogicalTime> d = ifac.decodeLogicalTime(e);
   check(*d == t && !(*d < t) && *d >= t, "HLAinteger64TimeFactory::decodeLogicalTime and comparisons");
   check(ifac.makeInitial()->isInitial() && ifac.makeFinal()->isFinal() && ifac.makeEpsilon()->isEpsilon() &&
            ifac.makeZero()->isZero(),
         "HLAinteger64TimeFactory initial/final/epsilon/zero");
   check(throws<IllegalTimeArithmetic>([] {
            HLAinteger64Time f;
            f.setFinal();
            f += HLAinteger64Interval(1);
         }),
         "final + epsilon -> IllegalTimeArithmetic");
   check(throws<IllegalTimeArithmetic>([] {
            HLAinteger64Time z(0);
            z -= HLAinteger64Interval(1);
         }),
         "initial - 1 -> IllegalTimeArithmetic");
   check(throws<InvalidLogicalTime>([&] { (void)(t < HLAfloat64Time(1.0)); }), "compare integer and float times -> InvalidLogicalTime");
   HLAinteger64Interval diff;
   diff.setToDifference(HLAinteger64Time(15), HLAinteger64Time(10));
   check(diff.getInterval() == 5, "HLAinteger64Interval::setToDifference");
   check(throws<IllegalTimeArithmetic>([&] { diff.setToDifference(HLAinteger64Time(1), HLAinteger64Time(2)); }),
         "negative difference -> IllegalTimeArithmetic");
   unsigned char small[4];
   check(throws<CouldNotEncode>([&] { t.encode(small, sizeof small); }), "encode into a 4-octet buffer -> CouldNotEncode");
   check(throws<CouldNotDecode>([&] { t.decode(small, sizeof small); }), "decode 4 octets -> CouldNotDecode");

   HLAfloat64TimeFactory ffac;
   HLAfloat64Time ft(1.5);
   VariableLengthData fe = ft.encode();
   check(fe.size() == 8 && static_cast<unsigned char const*>(fe.data())[0] == 0x3F &&
            static_cast<unsigned char const*>(fe.data())[1] == 0xF8,
         "HLAfloat64Time encoding (HLAfloat64BE)");
   ft += HLAfloat64Interval(0.5);
   check(ft.getTime() == 2.0 && *ffac.decodeLogicalTime(ft.encode()) == ft, "HLAfloat64Time arithmetic and decode");
   check(ft.implementationName() == L"HLAfloat64Time" && ffac.getName() == L"HLAfloat64Time", "HLAfloat64Time names");

   RTI_UNIQUE_PTR<LogicalTimeFactory> f1 = LogicalTimeFactoryFactory::makeLogicalTimeFactory(L"HLAinteger64Time");
   RTI_UNIQUE_PTR<LogicalTimeFactory> f2 = LogicalTimeFactoryFactory::makeLogicalTimeFactory(L"");
   RTI_UNIQUE_PTR<LogicalTimeFactory> f3 = LogicalTimeFactoryFactory::makeLogicalTimeFactory(L"bogus");
   check(f1.get() && f1->getName() == L"HLAinteger64Time", "LogicalTimeFactoryFactory(HLAinteger64Time) (fedtime)");
   check(f2.get() && f2->getName() == L"HLAfloat64Time", "LogicalTimeFactoryFactory(\"\") -> HLAfloat64Time");
   check(f3.get() == nullptr, "LogicalTimeFactoryFactory(unknown) -> null");
}
} // namespace

int main()
{
   try
   {
      basicTypes();
      constructedTypes();
      times();
   }
   catch (Exception const& e)
   {
      std::wcout << L"[FAIL] unexpected exception: " << e.what() << L"\n";
      ++failures;
   }
   std::cout << (failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED") << " (" << checks << " checks, "
             << failures << " failure(s))\n";
   return failures == 0 ? 0 : 1;
}
