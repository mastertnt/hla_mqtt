// hla-pe-linker-version: sets (or checks) the linker version recorded in the
// optional header of a PE image (MajorLinkerVersion, MinorLinkerVersion).
//
//   hla-pe-linker-version <file.dll> <major>.<minor>          set
//   hla-pe-linker-version --check <file.dll> <major>.<minor>  exit 1 if different
//
// VR-Link (MAK) reads this field as the "compiler version" of the RTI DLL and
// expects 14.00; a DLL linked by VS2019 or VS2022 records 14.2x / 14.3x. The
// field is informative only: changing it does not change the code. A non-zero
// image checksum is computed again.
//
// Standalone (standard library only): built for the host and run after the
// link (CMake option HLA_MQTT_PE_LINKER_VERSION).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
std::uint32_t le32(std::vector<unsigned char> const& d, size_t at)
{
   return std::uint32_t(d[at]) | std::uint32_t(d[at + 1]) << 8 | std::uint32_t(d[at + 2]) << 16 |
          std::uint32_t(d[at + 3]) << 24;
}

// Image checksum (the algorithm of CheckSumMappedFile): 16-bit one's
// complement sum of the file, checksum field taken as zero, plus the size.
std::uint32_t peChecksum(std::vector<unsigned char> const& d, size_t checksumAt)
{
   std::uint64_t sum = 0;
   for (size_t i = 0; i < d.size(); i += 2)
   {
      if (i >= checksumAt && i < checksumAt + 4)
         continue;
      std::uint32_t w = d[i];
      if (i + 1 < d.size())
         w |= std::uint32_t(d[i + 1]) << 8;
      sum += w;
      sum = (sum & 0xFFFF) + (sum >> 16);
   }
   sum = (sum & 0xFFFF) + (sum >> 16);
   return std::uint32_t(sum) + std::uint32_t(d.size());
}

int fail(char const* what, char const* file)
{
   std::fprintf(stderr, "hla-pe-linker-version: %s: %s\n", file, what);
   return 2;
}
} // namespace

int main(int argc, char** argv)
{
   bool check = argc == 4 && std::strcmp(argv[1], "--check") == 0;
   if (argc != 3 && !check)
   {
      std::fprintf(stderr, "usage: hla-pe-linker-version [--check] <file> <major>.<minor>\n");
      return 2;
   }
   char const* file = argv[check ? 2 : 1];
   std::string const version = argv[check ? 3 : 2];
   size_t const dot = version.find('.');
   if (dot == std::string::npos)
      return fail("version must be <major>.<minor>", version.c_str());
   unsigned long const major = std::strtoul(version.substr(0, dot).c_str(), nullptr, 10);
   unsigned long const minor = std::strtoul(version.substr(dot + 1).c_str(), nullptr, 10);
   if (major > 255 || minor > 255)
      return fail("major and minor must be 0 to 255", version.c_str());

   std::vector<unsigned char> d;
   {
      std::ifstream in(file, std::ios::binary);
      if (!in)
         return fail("cannot read", file);
      d.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
   }
   if (d.size() < 0x40 || d[0] != 'M' || d[1] != 'Z')
      return fail("not a PE image (MZ)", file);
   size_t const pe = le32(d, 0x3C);
   if (pe + 24 + 68 > d.size() || std::memcmp(&d[pe], "PE\0\0", 4) != 0)
      return fail("not a PE image (PE signature)", file);
   size_t const optional = pe + 24;    // after the signature and the COFF header
   size_t const linker = optional + 2; // MajorLinkerVersion, MinorLinkerVersion
   size_t const checksumAt = optional + 64;

   if (check)
   {
      bool const ok = d[linker] == major && d[linker + 1] == minor;
      std::printf("%s: linker version %u.%02u%s\n", file, d[linker], d[linker + 1], ok ? "" : " (different)");
      return ok ? 0 : 1;
   }
   if (d[linker] == major && d[linker + 1] == minor)
      return 0; // already set: the file is not rewritten
   d[linker] = static_cast<unsigned char>(major);
   d[linker + 1] = static_cast<unsigned char>(minor);
   if (le32(d, checksumAt) != 0)
   {
      std::uint32_t const c = peChecksum(d, checksumAt);
      for (int i = 0; i < 4; ++i)
         d[checksumAt + i] = static_cast<unsigned char>(c >> (8 * i));
   }
   std::ofstream out(file, std::ios::binary | std::ios::trunc);
   out.write(reinterpret_cast<char const*>(d.data()), static_cast<std::streamsize>(d.size()));
   if (!out)
      return fail("cannot write", file);
   return 0;
}
