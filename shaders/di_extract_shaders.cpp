// di_extract_shaders: pulls every Xenos shader container out of Dante's Inferno's files, offline.
//
// The game keeps its shaders in the executable: tools/codegen.sh dumps the loaded TU2 image (out/codegen/image.bin)
// and this finds 1,359 containers in it. It also reads the bigfiles, where it finds none in this game; that path stays
// for games that keep shaders there.
//
// bigfileN.viv ("BIGH"): big-endian header {magic, file size (LE), entry count, header size}, then entries of
// {offset, stored size, name hash}. Most assets are "ols3" streams of SHOC chunks; their payloads are RefPack
// ("Rpak" tag, then a 0x10FB stream). Shaders are 2008-format containers (flags 0x102A1100 PS, 0x102A1101 VS,
// then virtualSize and physicalSize: the container is their sum), as D3DDevice_Create*Shader receives them.
// Each one is written once as {vs,ps}_<xxh3 of the container>.bin, the key the shader library uses.
//
// Usage: di_extract_shaders <out dir> <bigfileN.viv | executable image> ...
#define XXH_INLINE_ALL
#include <xxhash.h>

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// RefPack (EA's LZ77 variant). Returns false on a malformed stream.
bool Refpack(const uint8_t* in, size_t n, std::vector<uint8_t>& out) {
  if (n < 5 || (in[0] & 0x3E) != 0x10 || in[1] != 0xFB) return false;
  const bool large = in[0] & 0x80;
  size_t p = 2;
  if (in[0] & 0x01) p += large ? 4 : 3;
  size_t size = 0;
  for (int i = 0; i < (large ? 4 : 3); ++i) size = size << 8 | in[p++];
  out.clear();
  out.reserve(size);
  auto copy_plain = [&](size_t k) {
    if (p + k > n) return false;
    out.insert(out.end(), in + p, in + p + k);
    p += k;
    return true;
  };
  auto copy_back = [&](size_t off, size_t k) {
    if (off > out.size()) return false;
    size_t from = out.size() - off;
    for (size_t i = 0; i < k; ++i) out.push_back(out[from + i]);
    return true;
  };
  while (p < n) {
    const uint8_t b0 = in[p++];
    if (b0 < 0x80) {
      if (p >= n) return false;
      const uint8_t b1 = in[p++];
      if (!copy_plain(b0 & 3) || !copy_back(((b0 & 0x60) << 3) + b1 + 1, ((b0 & 0x1C) >> 2) + 3)) return false;
    } else if (b0 < 0xC0) {
      if (p + 2 > n) return false;
      const uint8_t b1 = in[p++], b2 = in[p++];
      if (!copy_plain(b1 >> 6) || !copy_back(((b1 & 0x3F) << 8) + b2 + 1, (b0 & 0x3F) + 4)) return false;
    } else if (b0 < 0xE0) {
      if (p + 3 > n) return false;
      const uint8_t b1 = in[p++], b2 = in[p++], b3 = in[p++];
      if (!copy_plain(b0 & 3) ||
          !copy_back(((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1, ((b0 & 0x0C) << 6) + b3 + 5))
        return false;
    } else if (b0 < 0xFC) {
      if (!copy_plain(((b0 & 0x1F) << 2) + 4)) return false;
    } else {
      copy_plain(b0 & 3);
      break;
    }
  }
  return out.size() == size;
}

struct Stats {
  size_t magic = 0, entries = 0, rpak = 0, rpak_bad = 0, found = 0, written = 0;
} stats;
std::unordered_set<uint64_t> seen;
std::filesystem::path out_dir;

void ScanContainers(const uint8_t* d, size_t n) {
  for (size_t i = 0; i + 36 <= n; ++i) {
    if (d[i] != 0x10 || d[i + 1] != 0x2A || d[i + 2] != 0x11) continue;
    ++stats.magic;
    if (d[i + 3] > 1) continue;
    const uint64_t size = uint64_t(be32(d + i + 4)) + be32(d + i + 8);
    if (size < 36 || size > (4u << 20) || i + size > n) continue;
    const uint32_t shader_offset = be32(d + i + 24);
    if (shader_offset >= size) continue;
    ++stats.found;
    const uint64_t h = XXH3_64bits(d + i, size);
    if (!seen.insert(h).second) continue;
    char name[64];
    std::snprintf(name, sizeof(name), "%s_%016llx.bin", d[i + 3] ? "vs" : "ps", (unsigned long long)h);
    if (FILE* f = std::fopen((out_dir / name).string().c_str(), "wb")) {
      std::fwrite(d + i, 1, size, f);
      std::fclose(f);
      ++stats.written;
    }
  }
}

// Every RefPack payload of one asset, plus the asset itself (a few containers are stored raw).
void ScanAsset(const std::vector<uint8_t>& a) {
  ScanContainers(a.data(), a.size());
  std::vector<uint8_t> plain;
  for (size_t i = 0; i + 14 <= a.size(); ++i) {
    if (std::memcmp(&a[i], "Rpak", 4) != 0) continue;
    const size_t size = be32(&a[i + 4]);
    const size_t start = i + 8;
    if (size < 5 || start + size > a.size()) continue;
    ++stats.rpak;
    if (!Refpack(&a[start], size, plain)) {
      ++stats.rpak_bad;
      continue;
    }
    ScanContainers(plain.data(), plain.size());
    i = start + size - 1;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <out dir> bigfile0.viv [bigfile1.viv ...]\n", argv[0]);
    return 1;
  }
  out_dir = argv[1];
  std::filesystem::create_directories(out_dir);
  for (const auto& entry : std::filesystem::directory_iterator(out_dir)) {
    // Already extracted: keep their keys so reruns add only new ones.
    unsigned long long h;
    if (std::sscanf(entry.path().filename().string().c_str() + 3, "%16llx", &h) == 1) seen.insert(h);
  }
  for (int a = 2; a < argc; ++a) {
    FILE* f = std::fopen(argv[a], "rb");
    if (!f) {
      std::fprintf(stderr, "%s: cannot open\n", argv[a]);
      return 1;
    }
    uint8_t head[16];
    if (std::fread(head, 1, 16, f) != 16 || std::memcmp(head, "BIGH", 4) != 0) {
      // Not an archive: a raw memory image (the executable's, where this game keeps its shaders).
      std::vector<uint8_t> raw(std::filesystem::file_size(argv[a]));
      std::fseek(f, 0, SEEK_SET);
      std::fread(raw.data(), 1, raw.size(), f);
      std::fclose(f);
      ScanAsset(raw);
      std::printf("%s: raw image, %zu bytes\n", argv[a], raw.size());
      continue;
    }
    const uint32_t count = be32(head + 8);
    std::vector<uint8_t> table(size_t(count) * 12);
    std::fread(table.data(), 1, table.size(), f);
    std::vector<uint8_t> asset;
    for (uint32_t e = 0; e < count; ++e) {
      const uint32_t offset = be32(&table[e * 12]), size = be32(&table[e * 12 + 4]);
      asset.resize(size);
      if (fseeko(f, offset, SEEK_SET) != 0 || std::fread(asset.data(), 1, size, f) != size) {
        std::fprintf(stderr, "%s: entry %u out of range\n", argv[a], e);
        continue;
      }
      ++stats.entries;
      ScanAsset(asset);
    }
    std::fclose(f);
    std::printf("%s: %u entries\n", argv[a], count);
  }
  std::printf("magic %zu, RefPack payloads %zu (%zu bad), containers found %zu, new files %zu, total unique %zu\n", stats.magic, stats.rpak,
              stats.rpak_bad, stats.found, stats.written, seen.size());
  return stats.rpak_bad ? 2 : 0;
}
