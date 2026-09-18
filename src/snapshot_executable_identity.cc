#include "snapshot_executable_identity.h"
#include <openssl/sha.h>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <vector>
#if defined(__linux__)
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace snapshot_profiles::identity {
namespace {
[[noreturn]] void Bad(const std::string &message) {
  throw std::runtime_error("snapshot executable: " + message);
}
#if defined(__linux__)
class File {
public:
  explicit File(const std::string &path) : fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK)) {
    if (fd < 0) Bad("cannot open " + path);
    struct stat st{};
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); fd = -1; Bad("not a regular file"); }
  }
  ~File() { if (fd >= 0) close(fd); }
  File(const File &) = delete;
  int fd;
};
bool RuntimeName(std::string_view name) {
  return name == "libc.so.6" || name == "libm.so.6" || name == "libpthread.so.0" ||
      name == "libdl.so.2" || name == "librt.so.1" || name == "libgcc_s.so.1";
}
bool RuntimePath(const char *path) {
  char *resolved = realpath(path, nullptr);
  if (!resolved) return false;
  std::string p(resolved); free(resolved);
  auto slash = p.rfind('/');
  const auto name = std::string_view(p).substr(slash + 1);
  bool root = p.starts_with("/usr/lib/") || p.starts_with("/lib/") || p.starts_with("/lib64/");
  return root && (RuntimeName(name) || name == "ld-linux-x86-64.so.2" || name == "ld-linux-aarch64.so.1");
}
void CheckEnvironment() {
  for (const char *name : {"LD_PRELOAD", "LD_LIBRARY_PATH", "LD_AUDIT", "LD_PROFILE", "LD_ORIGIN_PATH", "LD_ASSUME_KERNEL", "LD_DYNAMIC_WEAK", "LD_HWCAP_MASK", "LD_BIND_NOT", "GLIBC_TUNABLES"})
    if (const char *value = getenv(name); value && *value) Bad(std::string("unsupported loader environment: ") + name);
  struct stat st{};
  int preload_status = stat("/etc/ld.so.preload", &st);
  if (preload_status == 0 && st.st_size != 0) Bad("system preload is unsupported");
  if (preload_status != 0 && errno != ENOENT) Bad("cannot inspect system preload");
  std::string failure;
  dl_iterate_phdr([](dl_phdr_info *info, size_t, void *context) {
    auto &error = *static_cast<std::string *>(context);
    const char *name = info->dlpi_name;
    if (!name || !*name || std::string_view(name) == "linux-vdso.so.1") return 0;
    if (!RuntimePath(name)) { error = "unsupported loaded dependency: " + std::string(name); return 1; }
    return 0;
  }, &failure);
  if (!failure.empty()) Bad(failure);
}
class ELF {
public:
  explicit ELF(int input) : fd(input) {
    struct stat st{};
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0) Bad("invalid ELF file");
    size = static_cast<uint64_t>(st.st_size);
    header = At<Elf64_Ehdr>(0);
    if (memcmp(header.e_ident, ELFMAG, SELFMAG) || header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_ident[EI_VERSION] != EV_CURRENT ||
        header.e_version != EV_CURRENT || (header.e_type != ET_EXEC && header.e_type != ET_DYN) ||
        header.e_ehsize != sizeof(Elf64_Ehdr) || header.e_phentsize != sizeof(Elf64_Phdr) ||
        !header.e_phnum || header.e_phnum == PN_XNUM) Bad("unsupported ELF format");
#if defined(__x86_64__)
    if (header.e_machine != EM_X86_64) Bad("ELF machine mismatch");
#elif defined(__aarch64__)
    if (header.e_machine != EM_AARCH64) Bad("ELF machine mismatch");
#else
    Bad("unsupported host architecture");
#endif
    for (uint64_t i = 0; i < header.e_phnum; ++i) {
      auto ph = At<Elf64_Phdr>(header.e_phoff + i * sizeof(Elf64_Phdr));
      Range(ph.p_offset, ph.p_filesz);
      if (ph.p_type == PT_LOAD) loads.push_back(ph);
      if (ph.p_type == PT_DYNAMIC) {
        if (dynamic.p_filesz) Bad("multiple dynamic tables");
        dynamic = ph;
      }
      if (ph.p_type == PT_INTERP) {
        auto path = Bytes(ph.p_offset, ph.p_filesz);
        if (path.empty() || path.back() != '\0' || !RuntimePath(path.c_str())) Bad("unsupported interpreter");
        interpreter = true;
      }
    }
    if (!interpreter || !dynamic.p_filesz || dynamic.p_filesz % sizeof(Elf64_Dyn)) Bad("missing dynamic ELF metadata");
    if (Offset(dynamic.p_vaddr, dynamic.p_filesz) != dynamic.p_offset) Bad("dynamic loader/file mismatch");
    bool ended = false;
    for (uint64_t offset = 0; offset < dynamic.p_filesz; offset += sizeof(Elf64_Dyn)) {
      auto d = At<Elf64_Dyn>(dynamic.p_offset + offset);
      if (d.d_tag == DT_NULL) { ended = true; break; }
      if (d.d_tag == DT_NEEDED) needed.push_back(d.d_un.d_val);
      else if (!tags.emplace(d.d_tag, d.d_un.d_val).second) Bad("duplicate dynamic tag");
    }
    if (!ended) Bad("unterminated dynamic table");
  }
  template<class T> T At(uint64_t offset) const {
    T v{}; Read(offset, &v, sizeof(v)); return v;
  }
  void Read(uint64_t offset, void *out, uint64_t count) const {
    Range(offset, count);
    auto *p = static_cast<char *>(out);
    while (count) {
      ssize_t n = pread(fd, p, static_cast<size_t>(count), static_cast<off_t>(offset));
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) Bad("short ELF read");
      offset += n; count -= n; p += n;
    }
  }
  void Range(uint64_t offset, uint64_t count) const {
    if (offset > size || count > size - offset) Bad("ELF range outside file");
  }
  uint64_t Offset(uint64_t address, uint64_t count) const {
    for (auto ph : loads)
      if (address >= ph.p_vaddr && address - ph.p_vaddr <= ph.p_filesz && count <= ph.p_filesz - (address - ph.p_vaddr))
        return ph.p_offset + (address - ph.p_vaddr);
    Bad("ELF address outside file-backed load segment");
  }
  std::string Bytes(uint64_t offset, uint64_t count) const {
    if (count > 16 * 1024 * 1024) Bad("oversized ELF metadata");
    std::string s(count, '\0'); Read(offset, s.data(), count); return s;
  }
  uint64_t Tag(int64_t tag) const {
    auto it = tags.find(tag); if (it == tags.end()) Bad("missing dynamic tag"); return it->second;
  }
  std::string String(uint64_t index) const {
    if (index >= strings.size()) Bad("invalid string index");
    auto end = strings.find('\0', index);
    if (end == std::string::npos) Bad("unterminated dynamic string");
    return strings.substr(index, end - index);
  }
  void Verify() {
    for (int64_t tag : {DT_RPATH, DT_RUNPATH, DT_FILTER, DT_AUXILIARY, DT_AUDIT, DT_DEPAUDIT})
      if (tags.count(tag)) Bad("unsupported dependency search/interposition tag");
    strings = Bytes(Offset(Tag(DT_STRTAB), Tag(DT_STRSZ)), Tag(DT_STRSZ));
    for (auto n : needed) if (!RuntimeName(String(n))) Bad("unsupported semantic dependency");
    if (Tag(DT_SYMENT) != sizeof(Elf64_Sym)) Bad("wrong dynamic symbol entry size");
    // Derive the complete loader symbol extent from its hash tables, not the
    // independently mutable SHT_DYNSYM section length (including WEAK imports).
    uint64_t count = 0;
    if (tags.count(DT_HASH)) {
      auto h = Offset(Tag(DT_HASH), 8);
      uint32_t buckets = At<uint32_t>(h), chains = At<uint32_t>(h + 4);
      if (!buckets || !chains || chains > 1000000 || buckets > 1000000) Bad("invalid SysV hash extent");
      Offset(Tag(DT_HASH), 8 + 4ull * (buckets + chains));
      for (uint64_t i = 0; i < uint64_t(buckets) + chains; ++i)
        if (At<uint32_t>(h + 8 + 4 * i) >= chains) Bad("SysV symbol reference outside extent");
      count = chains;
    }
    if (tags.count(DT_GNU_HASH)) {
      auto address = Tag(DT_GNU_HASH); auto h = Offset(address, 16);
      uint32_t buckets = At<uint32_t>(h), start = At<uint32_t>(h + 4), bloom = At<uint32_t>(h + 8);
      if (!buckets || !bloom || buckets > 1000000 || bloom > 1000000 || start > 1000000) Bad("invalid GNU hash extent");
      uint64_t bucket_address = address + 16 + 8ull * bloom;
      auto bucket_file = Offset(bucket_address, 4ull * buckets);
      uint64_t chains = bucket_address + 4ull * buckets, gnu_count = start;
      for (uint64_t i = 0; i < buckets; ++i) {
        uint64_t s = At<uint32_t>(bucket_file + 4 * i);
        if (!s) continue;
        if (s < start) Bad("invalid GNU hash bucket");
        for (;;) {
          if (s > 1000000) Bad("unbounded GNU hash chain");
          auto hash = At<uint32_t>(Offset(chains + 4 * (s - start), 4));
          ++s; gnu_count = std::max(gnu_count, s);
          if (hash & 1) break;
        }
      }
      if (count && count != gnu_count) Bad("conflicting symbol extents");
      count = gnu_count;
    }
    if (!count || count > 1000000) Bad("missing complete symbol extent");
    auto symbols = Offset(Tag(DT_SYMTAB), count * sizeof(Elf64_Sym));
    if (header.e_shentsize != sizeof(Elf64_Shdr) || !header.e_shnum) Bad("missing verifiable ELF sections");
    unsigned symbol_sections = 0;
    for (uint64_t i = 0; i < header.e_shnum; ++i) {
      auto s = At<Elf64_Shdr>(header.e_shoff + i * sizeof(Elf64_Shdr));
      if (s.sh_type == SHT_DYNSYM) {
        ++symbol_sections;
        if (s.sh_addr != Tag(DT_SYMTAB) || s.sh_offset != symbols || s.sh_size != count * sizeof(Elf64_Sym) || s.sh_entsize != sizeof(Elf64_Sym)) Bad("section/loader symbol extent mismatch");
      }
    }
    if (symbol_sections != 1) Bad("invalid dynamic symbol sections");
    auto versions = Offset(Tag(DT_VERSYM), count * sizeof(Elf64_Half));
    std::map<uint16_t, std::string> version_names;
    uint64_t v = Tag(DT_VERNEED);
    auto n = Tag(DT_VERNEEDNUM);
    if (!n || n > 1000) Bad("invalid version count");
    for (uint64_t i = 0; i < n; ++i) {
      auto need = At<Elf64_Verneed>(Offset(v, sizeof(Elf64_Verneed)));
      if (need.vn_version != 1 || !need.vn_cnt || need.vn_cnt > 1000 || !RuntimeName(String(need.vn_file))) Bad("unsupported version dependency");
      uint64_t a = v + need.vn_aux;
      for (unsigned j = 0; j < need.vn_cnt; ++j) {
        auto aux = At<Elf64_Vernaux>(Offset(a, sizeof(Elf64_Vernaux)));
        auto name = String(aux.vna_name);
        if (!(name.starts_with("GLIBC_") || name.starts_with("GCC_")) || !version_names.emplace(aux.vna_other & 0x7fff, name).second) Bad("unsupported symbol version");
        if (j + 1 < need.vn_cnt && !aux.vna_next) Bad("short symbol version list");
        a += aux.vna_next;
      }
      if (i + 1 < n && !need.vn_next) Bad("short dependency version list");
      v += need.vn_next;
    }
    for (uint64_t i = 1; i < count; ++i) {
      auto s = At<Elf64_Sym>(symbols + i * sizeof(Elf64_Sym));
      auto binding = ELF64_ST_BIND(s.st_info);
      auto name = String(s.st_name);
      if (binding == STB_GNU_UNIQUE) Bad("unsupported GNU unique symbol");
      if (s.st_shndx != SHN_UNDEF || (binding != STB_GLOBAL && binding != STB_WEAK)) continue;
      auto version = At<Elf64_Half>(versions + i * sizeof(Elf64_Half)) & 0x7fff;
      if (!version_names.count(version)) {
        if (binding == STB_WEAK && (name == "__gmon_start__" || name == "_ITM_deregisterTMCloneTable" || name == "_ITM_registerTMCloneTable") && !dlsym(RTLD_DEFAULT, name.c_str())) continue;
        Bad("unversioned non-runtime import: " + name);
      }
    }
    for (auto [address_tag, size_tag] : {std::pair<int64_t, int64_t>{DT_RELA, DT_RELASZ}, {DT_JMPREL, DT_PLTRELSZ}}) {
      if (!tags.count(address_tag)) continue;
      uint64_t bytes = Tag(size_tag);
      if (bytes % sizeof(Elf64_Rela)) Bad("invalid relocation extent");
      auto offset = Offset(Tag(address_tag), bytes);
      for (uint64_t i = 0; i < bytes; i += sizeof(Elf64_Rela))
        if (ELF64_R_SYM(At<Elf64_Rela>(offset + i).r_info) >= count) Bad("relocation symbol outside extent");
    }
    if (tags.count(DT_REL)) Bad("unsupported REL relocation table");
  }
private:
  int fd;
  uint64_t size;
  Elf64_Ehdr header{};
  Elf64_Phdr dynamic{};
  bool interpreter = false;
  std::vector<Elf64_Phdr> loads;
  std::map<int64_t, uint64_t> tags;
  std::vector<uint64_t> needed;
  std::string strings;
};
#endif
} // namespace

std::string HexDigest(const unsigned char *bytes, size_t size) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out = "0x";
  for (size_t i = 0; i < size; ++i) { out += hex[bytes[i] >> 4]; out += hex[bytes[i] & 15]; }
  return out;
}
std::string ReadProfileFile(const std::string &path) {
#if defined(__linux__)
  File file(path);
  // Profiles are bounded startup configuration; executable hashing below is
  // independently streaming and has no 32-bit or configuration-size limit.
  std::string raw;
  std::array<char, 65536> buffer{};
  for (;;) {
    ssize_t n = read(file.fd, buffer.data(), buffer.size());
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) Bad("profile read failed");
    if (!n) break;
    if (raw.size() + n > 16 * 1024 * 1024) Bad("profile file exceeds 16 MiB");
    raw.append(buffer.data(), n);
  }
  return raw;
#else
  Bad("measured profiles require Linux");
#endif
}
void InspectExecutable(int fd) {
#if defined(__linux__)
  ELF(fd).Verify();
#else
  (void)fd; Bad("measured profiles require Linux");
#endif
}
std::string MeasureRunningExecutable() {
#if defined(__linux__)
  static_assert(sizeof(off_t) >= 8);
  CheckEnvironment();
  File file("/proc/self/exe");
  InspectExecutable(file.fd);
  struct stat before{}, after{};
  if (fstat(file.fd, &before)) Bad("cannot stat running executable");
  SHA256_CTX ctx; SHA256_Init(&ctx);
  std::array<char, 1024 * 1024> buffer{};
  uint64_t total = 0;
  for (;;) {
    ssize_t n = read(file.fd, buffer.data(), buffer.size());
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) Bad("executable read failed");
    if (!n) break;
    SHA256_Update(&ctx, buffer.data(), n); total += static_cast<uint64_t>(n);
  }
  if (fstat(file.fd, &after) || total != static_cast<uint64_t>(before.st_size) || before.st_size != after.st_size ||
      before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
      before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
    Bad("running executable changed while measuring");
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{}; SHA256_Final(digest.data(), &ctx);
  CheckEnvironment();
  return HexDigest(digest.data(), digest.size());
#else
  Bad("measured profiles require Linux");
#endif
}
} // namespace snapshot_profiles::identity
