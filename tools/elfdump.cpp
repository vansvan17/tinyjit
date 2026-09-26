// elfdump: a small ELF64 reader. Prints the file header, the section table,
// symbol tables and RELA relocations. Works on the objects tinyjit emits and
// on ordinary binaries (try it on /bin/ls or on build/tinyjit itself).
//
// The ELF layout in one paragraph: a fixed 64-byte header at offset 0 says
// where the section header table lives (e_shoff) and how many entries it has.
// Each section header gives a name (an offset into the section named by
// e_shstrndx), a type, and where its bytes are in the file. A symbol table
// section (SHT_SYMTAB / SHT_DYNSYM) is an array of Elf64_Sym whose names
// live in the string table named by the section's sh_link. A SHT_RELA
// section patches the section named by sh_info using symbols from sh_link.
#include <elf.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static std::vector<uint8_t> data;

template <class T>
static const T* at(size_t off, size_t n = 1) {
  if (off + sizeof(T) * n > data.size()) {
    fprintf(stderr, "elfdump: truncated file (offset %zu)\n", off);
    exit(1);
  }
  return reinterpret_cast<const T*>(data.data() + off);
}

static const char* str(size_t table_off, size_t table_size, uint32_t idx) {
  if (idx >= table_size) return "<bad>";
  return reinterpret_cast<const char*>(data.data() + table_off + idx);
}

static const char* type_name(uint16_t t) {
  switch (t) {
    case ET_REL: return "REL (relocatable object)";
    case ET_EXEC: return "EXEC (executable)";
    case ET_DYN: return "DYN (shared object / PIE)";
    case ET_CORE: return "CORE";
    default: return "?";
  }
}

static const char* sh_type_name(uint32_t t) {
  switch (t) {
    case SHT_NULL: return "NULL";
    case SHT_PROGBITS: return "PROGBITS";
    case SHT_SYMTAB: return "SYMTAB";
    case SHT_STRTAB: return "STRTAB";
    case SHT_RELA: return "RELA";
    case SHT_HASH: return "HASH";
    case SHT_DYNAMIC: return "DYNAMIC";
    case SHT_NOTE: return "NOTE";
    case SHT_NOBITS: return "NOBITS";
    case SHT_REL: return "REL";
    case SHT_DYNSYM: return "DYNSYM";
    case SHT_INIT_ARRAY: return "INIT_ARRAY";
    case SHT_FINI_ARRAY: return "FINI_ARRAY";
    case SHT_GNU_HASH: return "GNU_HASH";
    case SHT_GNU_versym: return "VERSYM";
    case SHT_GNU_verneed: return "VERNEED";
    default: return "OTHER";
  }
}

static std::string flags(uint64_t f) {
  std::string s;
  if (f & SHF_WRITE) s += 'W';
  if (f & SHF_ALLOC) s += 'A';
  if (f & SHF_EXECINSTR) s += 'X';
  if (f & SHF_INFO_LINK) s += 'I';
  return s;
}

static const char* sym_type(unsigned t) {
  switch (t) {
    case STT_NOTYPE: return "NOTYPE";
    case STT_OBJECT: return "OBJECT";
    case STT_FUNC: return "FUNC";
    case STT_SECTION: return "SECTION";
    case STT_FILE: return "FILE";
    default: return "OTHER";
  }
}

int main(int argc, char** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: elfdump file\n");
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) { perror(argv[1]); return 1; }
  data.assign(std::istreambuf_iterator<char>(in), {});

  const auto* eh = at<Elf64_Ehdr>(0);
  if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) { fprintf(stderr, "not an ELF file\n"); return 1; }
  if (eh->e_ident[EI_CLASS] != ELFCLASS64) { fprintf(stderr, "only ELF64 is supported\n"); return 1; }

  printf("ELF header\n");
  printf("  class        ELF64, %s\n", eh->e_ident[EI_DATA] == ELFDATA2LSB ? "little endian" : "big endian");
  printf("  type         %s\n", type_name(eh->e_type));
  printf("  machine      %s\n", eh->e_machine == EM_X86_64 ? "x86-64" : eh->e_machine == EM_AARCH64 ? "AArch64" : "other");
  printf("  entry        0x%llx\n", (unsigned long long)eh->e_entry);
  printf("  sections     %u at offset %llu, names in section %u\n", eh->e_shnum,
         (unsigned long long)eh->e_shoff, eh->e_shstrndx);
  printf("  segments     %u at offset %llu\n", eh->e_phnum, (unsigned long long)eh->e_phoff);

  const auto* sh = at<Elf64_Shdr>(eh->e_shoff, eh->e_shnum);
  const Elf64_Shdr& names = sh[eh->e_shstrndx];
  auto sname = [&](uint32_t i) { return str(names.sh_offset, names.sh_size, sh[i].sh_name); };

  printf("\nSections\n  %-3s %-20s %-10s %-5s %10s %10s %6s %4s %4s\n", "#", "name", "type", "flags", "offset",
         "size", "entsz", "link", "info");
  for (unsigned i = 0; i < eh->e_shnum; i++)
    printf("  %-3u %-20s %-10s %-5s %10llu %10llu %6llu %4u %4u\n", i, sname(i), sh_type_name(sh[i].sh_type),
           flags(sh[i].sh_flags).c_str(), (unsigned long long)sh[i].sh_offset, (unsigned long long)sh[i].sh_size,
           (unsigned long long)sh[i].sh_entsize, sh[i].sh_link, sh[i].sh_info);

  for (unsigned i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type != SHT_SYMTAB && sh[i].sh_type != SHT_DYNSYM) continue;
    const Elf64_Shdr& st = sh[sh[i].sh_link];
    size_t n = sh[i].sh_size / sizeof(Elf64_Sym);
    const auto* syms = at<Elf64_Sym>(sh[i].sh_offset, n);
    printf("\nSymbols in %s (%zu)\n  %-5s %-18s %8s %-8s %-7s %-6s %s\n", sname(i), n, "#", "value", "size", "type",
           "bind", "shndx", "name");
    for (size_t k = 0; k < n; k++) {
      const Elf64_Sym& s = syms[k];
      unsigned bind = ELF64_ST_BIND(s.st_info);
      char ndx[16];
      if (s.st_shndx == SHN_UNDEF) snprintf(ndx, sizeof ndx, "UND");
      else if (s.st_shndx == SHN_ABS) snprintf(ndx, sizeof ndx, "ABS");
      else snprintf(ndx, sizeof ndx, "%u", s.st_shndx);
      printf("  %-5zu %018llx %8llu %-8s %-7s %-6s %s\n", k, (unsigned long long)s.st_value,
             (unsigned long long)s.st_size, sym_type(ELF64_ST_TYPE(s.st_info)),
             bind == STB_LOCAL ? "LOCAL" : bind == STB_GLOBAL ? "GLOBAL" : "WEAK", ndx,
             str(st.sh_offset, st.sh_size, s.st_name));
    }
  }

  for (unsigned i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type != SHT_RELA) continue;
    const Elf64_Shdr& symsec = sh[sh[i].sh_link];
    const Elf64_Shdr& strsec = sh[symsec.sh_link];
    const auto* syms = at<Elf64_Sym>(symsec.sh_offset, symsec.sh_size / sizeof(Elf64_Sym));
    size_t n = sh[i].sh_size / sizeof(Elf64_Rela);
    const auto* rel = at<Elf64_Rela>(sh[i].sh_offset, n);
    printf("\nRelocations in %s (patching %s)\n", sname(i), sname(sh[i].sh_info));
    size_t shown = 0;
    for (size_t k = 0; k < n && shown < 40; k++, shown++) {
      uint32_t type = ELF64_R_TYPE(rel[k].r_info);
      uint32_t sym = ELF64_R_SYM(rel[k].r_info);
      const char* tname = type == R_X86_64_64 ? "R_X86_64_64" : type == R_X86_64_PC32 ? "R_X86_64_PC32"
                          : type == R_X86_64_PLT32 ? "R_X86_64_PLT32" : type == R_X86_64_RELATIVE ? "R_X86_64_RELATIVE"
                          : "other";
      printf("  offset %08llx  %-18s %s%+lld\n", (unsigned long long)rel[k].r_offset, tname,
             sym ? str(strsec.sh_offset, strsec.sh_size, syms[sym].st_name) : "", (long long)rel[k].r_addend);
    }
    if (n > shown) printf("  ... %zu more\n", n - shown);
  }
  return 0;
}
