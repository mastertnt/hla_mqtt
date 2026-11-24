// DDM region extents: dimension handle -> [lower, upper).
//
// Two regions overlap when their ranges overlap on every dimension they share
// (a dimension absent from a region spans its whole range). A set of regions
// overlaps another when one region of each overlaps; an empty set stands for
// the default region, which overlaps everything.
#pragma once

#include "Json.h"
#include "Wire.h"

#include <cstdint>
#include <cstdlib>
#include <map>
#include <utility>
#include <vector>

namespace hla_mqtt
{
using Extents = std::map<std::uint64_t, std::pair<unsigned long, unsigned long>>;

inline bool overlaps(Extents const& a, Extents const& b)
{
   for (auto const& d : a)
   {
      auto it = b.find(d.first);
      if (it != b.end() && !(d.second.first < it->second.second && it->second.first < d.second.second))
         return false;
   }
   return true;
}

// Empty = default region.
inline bool anyOverlap(std::vector<Extents> const& a, std::vector<Extents> const& b)
{
   if (a.empty() || b.empty())
      return true;
   for (Extents const& x : a)
      for (Extents const& y : b)
         if (overlaps(x, y))
            return true;
   return false;
}

// JSON: {"<dimension>": [lower, upper], ...}
inline Json extentsToJson(Extents const& e)
{
   Json j = Json::object();
   for (auto const& d : e)
   {
      Json range = Json::array();
      range.push(Json(double(d.second.first)));
      range.push(Json(double(d.second.second)));
      j.set(hex64(d.first), range);
   }
   return j;
}

inline Extents extentsFromJson(Json const& j)
{
   Extents e;
   for (auto const& kv : j.members())
   {
      std::vector<Json> const& r = kv.second.items();
      if (r.size() == 2)
         e[std::strtoull(kv.first.c_str(), nullptr, 16)] =
            std::make_pair((unsigned long)r[0].num(), (unsigned long)r[1].num());
   }
   return e;
}

// Wire: u32 count, then per region u32 dimensions, then (u64 dim, u64 lower, u64 upper).
inline void writeExtents(WireWriter& w, std::vector<Extents> const& regions)
{
   w.u32(std::uint32_t(regions.size()));
   for (Extents const& e : regions)
   {
      w.u32(std::uint32_t(e.size()));
      for (auto const& d : e)
         w.u64(d.first).u64(d.second.first).u64(d.second.second);
   }
}

inline std::vector<Extents> readExtents(WireReader& r)
{
   std::uint32_t const count = r.u32();
   if (count > 4096)
      throw WireError("too many regions");
   std::vector<Extents> regions(count);
   for (Extents& e : regions)
   {
      std::uint32_t const n = r.u32();
      if (n > 1024)
         throw WireError("too many dimensions");
      for (std::uint32_t k = 0; k < n; ++k)
      {
         std::uint64_t const d = r.u64();
         unsigned long const lo = (unsigned long)r.u64();
         unsigned long const hi = (unsigned long)r.u64();
         e[d] = std::make_pair(lo, hi);
      }
   }
   return regions;
}
} // namespace hla_mqtt
