#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static std::vector<uint8_t> data;
static size_t base = 0;

template <class T>
static T get(size_t off) {
  if (base + off + sizeof(T) > data.size()) {
    fprintf(stderr, "machodump: truncated file (offset %zu)\n", base + off);
    exit(1);
  }
  T v;
  memcpy(&v, &data[base + off], sizeof(T));
  return v;
}
static uint32_t be32(size_t abs) {
  return (uint32_t)data[abs] << 24 | (uint32_t)data[abs + 1] << 16 | (uint32_t)data[abs + 2] << 8 | data[abs + 3];
}
static std::string fixed(const char* p, size_t n) { return std::string(p, strnlen(p, n)); }

#pragma pack(push, 4)
struct Header { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct Segment {
  uint32_t cmd, cmdsize;
  char segname[16];
  uint64_t vmaddr, vmsize, fileoff, filesize;
  int32_t maxprot, initprot;
  uint32_t nsects, flags;
};
struct Section {
  char sectname[16], segname[16];
  uint64_t addr, size;
  uint32_t offset, align, reloff, nreloc, flags, reserved1, reserved2, reserved3;
};
struct Symtab { uint32_t cmd, cmdsize, symoff, nsyms, stroff, strsize; };
struct Dysymtab { uint32_t cmd, cmdsize, ilocalsym, nlocalsym, iextdefsym, nextdefsym, iundefsym, nundefsym; };
struct Nlist { uint32_t n_strx; uint8_t n_type, n_sect; uint16_t n_desc; uint64_t n_value; };
struct Reloc { int32_t r_address; uint32_t info; };
#pragma pack(pop)

static const char* cpu_name(uint32_t c) {
  switch (c) {
    case 0x0100000C: return "arm64";
    case 0x01000007: return "x86_64";
    default: return "other";
  }
}
static const char* filetype_name(uint32_t t) {
  switch (t) {
    case 1: return "MH_OBJECT (relocatable object, .o)";
    case 2: return "MH_EXECUTE (executable)";
    case 6: return "MH_DYLIB (shared library)";
    case 8: return "MH_BUNDLE";
    default: return "other";
  }
}
static const char* cmd_name(uint32_t c) {
  switch (c & 0x7FFFFFFF) {
    case 0x19: return "LC_SEGMENT_64";
    case 0x02: return "LC_SYMTAB";
    case 0x0B: return "LC_DYSYMTAB";
    case 0x0C: return "LC_LOAD_DYLIB";
    case 0x0E: return "LC_LOAD_DYLINKER";
    case 0x1B: return "LC_UUID";
    case 0x1D: return "LC_CODE_SIGNATURE";
    case 0x26: return "LC_FUNCTION_STARTS";
    case 0x28: return "LC_MAIN";
    case 0x29: return "LC_DATA_IN_CODE";
    case 0x2A: return "LC_SOURCE_VERSION";
    case 0x32: return "LC_BUILD_VERSION";
    case 0x33: return "LC_DYLD_EXPORTS_TRIE";
    case 0x34: return "LC_DYLD_CHAINED_FIXUPS";
    case 0x22: return "LC_DYLD_INFO";
    default: return "(other)";
  }
}
static const char* reloc_name(uint32_t t) {
  static const char* n[] = {"ARM64_RELOC_UNSIGNED", "ARM64_RELOC_SUBTRACTOR", "ARM64_RELOC_BRANCH26",
                            "ARM64_RELOC_PAGE21", "ARM64_RELOC_PAGEOFF12", "ARM64_RELOC_GOT_LOAD_PAGE21",
                            "ARM64_RELOC_GOT_LOAD_PAGEOFF12", "ARM64_RELOC_POINTER_TO_GOT",
                            "ARM64_RELOC_TLVP_LOAD_PAGE21", "ARM64_RELOC_TLVP_LOAD_PAGEOFF12", "ARM64_RELOC_ADDEND"};
  return t < 11 ? n[t] : "other";
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: machodump file\n");
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) { perror(argv[1]); return 1; }
  data.assign(std::istreambuf_iterator<char>(in), {});
  if (data.size() < 32) { fprintf(stderr, "file too small\n"); return 1; }

  uint32_t fat = be32(0);
  if (fat == 0xCAFEBABE || fat == 0xCAFEBABF) {
    bool is64 = fat == 0xCAFEBABF;
    uint32_t n = be32(4);
    size_t entry = is64 ? 32 : 20;
    printf("Universal binary with %u slices:\n", n);
    size_t chosen = 0;
    for (uint32_t i = 0; i < n; i++) {
      size_t e = 8 + i * entry;
      uint32_t cpu = be32(e);
      uint64_t off = is64 ? ((uint64_t)be32(e + 8) << 32 | be32(e + 12)) : be32(e + 8);
      printf("  %-7s at offset %llu\n", cpu_name(cpu), (unsigned long long)off);
      if (i == 0 || cpu == 0x0100000C) chosen = off;
    }
    base = chosen;
    printf("Showing the slice at offset %zu.\n\n", base);
  }

  auto h = get<Header>(0);
  if (h.magic != 0xFEEDFACF) { fprintf(stderr, "not a 64-bit Mach-O file (magic %08x)\n", h.magic); return 1; }
  printf("Mach-O header\n");
  printf("  cpu          %s\n", cpu_name(h.cputype));
  printf("  type         %s\n", filetype_name(h.filetype));
  printf("  commands     %u, %u bytes\n", h.ncmds, h.sizeofcmds);
  printf("  flags        0x%x\n", h.flags);

  std::vector<Section> sections;
  Symtab symtab{};
  printf("\nLoad commands\n");
  size_t off = sizeof(Header);
  for (uint32_t i = 0; i < h.ncmds; i++) {
    uint32_t cmd = get<uint32_t>(off), size = get<uint32_t>(off + 4);
    printf("  %-2u %-24s %5u bytes", i, cmd_name(cmd), size);
    if (cmd == 0x19) {
      auto s = get<Segment>(off);
      printf("  segment '%s' vm 0x%llx+0x%llx file %llu+%llu, %u sections\n", fixed(s.segname, 16).c_str(),
             (unsigned long long)s.vmaddr, (unsigned long long)s.vmsize, (unsigned long long)s.fileoff,
             (unsigned long long)s.filesize, s.nsects);
      for (uint32_t k = 0; k < s.nsects; k++) {
        auto sec = get<Section>(off + sizeof(Segment) + k * sizeof(Section));
        sections.push_back(sec);
        printf("       section %zu: %s,%s  addr 0x%llx size %llu  align 2^%u  relocs %u\n", sections.size(),
               fixed(sec.segname, 16).c_str(), fixed(sec.sectname, 16).c_str(), (unsigned long long)sec.addr,
               (unsigned long long)sec.size, sec.align, sec.nreloc);
      }
    } else if (cmd == 0x02) {
      symtab = get<Symtab>(off);
      printf("  %u symbols at %u, strings at %u\n", symtab.nsyms, symtab.symoff, symtab.stroff);
    } else if (cmd == 0x0B) {
      auto d = get<Dysymtab>(off);
      printf("  local %u+%u, defined %u+%u, undefined %u+%u\n", d.ilocalsym, d.nlocalsym, d.iextdefsym,
             d.nextdefsym, d.iundefsym, d.nundefsym);
    } else if ((cmd & 0x7FFFFFFF) == 0x0C || cmd == 0x0E) {
      uint32_t name_off = get<uint32_t>(off + 8);
      printf("  %s\n", (const char*)&data[base + off + name_off]);
    } else if (cmd == 0x32) {
      uint32_t platform = get<uint32_t>(off + 8), minos = get<uint32_t>(off + 12);
      printf("  platform %s, min OS %u.%u\n", platform == 1 ? "macOS" : "other", minos >> 16, (minos >> 8) & 0xFF);
    } else if ((cmd & 0x7FFFFFFF) == 0x28) {
      printf("  entry point at file offset %llu\n", (unsigned long long)get<uint64_t>(off + 8));
    } else {
      printf("\n");
    }
    off += size;
  }

  auto sym_name = [&](uint32_t idx) -> std::string {
    auto n = get<Nlist>(symtab.symoff + idx * sizeof(Nlist));
    return (const char*)&data[base + symtab.stroff + n.n_strx];
  };
  if (symtab.nsyms) {
    printf("\nSymbols (%u%s)\n", symtab.nsyms, symtab.nsyms > 50 ? ", first 50" : "");
    printf("  %-5s %-18s %-10s %-5s %s\n", "#", "value", "kind", "sect", "name");
    for (uint32_t i = 0; i < symtab.nsyms && i < 50; i++) {
      auto n = get<Nlist>(symtab.symoff + i * sizeof(Nlist));
      const char* kind = (n.n_type & 0xE0) ? "debug" : (n.n_type & 0x0E) == 0x0E ? "defined" : "undefined";
      printf("  %-5u %018llx %-10s %-5u %s%s\n", i, (unsigned long long)n.n_value, kind, n.n_sect,
             sym_name(i).c_str(), (n.n_type & 1) ? "" : "  (local)");
    }
  }

  for (size_t s = 0; s < sections.size(); s++) {
    auto& sec = sections[s];
    if (!sec.nreloc) continue;
    printf("\nRelocations in %s,%s\n", fixed(sec.segname, 16).c_str(), fixed(sec.sectname, 16).c_str());
    for (uint32_t k = 0; k < sec.nreloc && k < 40; k++) {
      auto r = get<Reloc>(sec.reloff + k * sizeof(Reloc));
      uint32_t sym = r.info & 0xFFFFFF, pcrel = r.info >> 24 & 1, len = r.info >> 25 & 3, ext = r.info >> 27 & 1,
               type = r.info >> 28;
      printf("  offset %08x  %-28s %s%s  (%u bytes%s)\n", r.r_address, reloc_name(type),
             ext ? sym_name(sym).c_str() : "section ", ext ? "" : std::to_string(sym).c_str(), 1u << len,
             pcrel ? ", pc-relative" : "");
    }
    if (sec.nreloc > 40) printf("  ... %u more\n", sec.nreloc - 40);
  }
  return 0;
}
