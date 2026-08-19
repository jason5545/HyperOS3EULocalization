#include "art_resolver.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <vector>

#include "xz.h"

namespace {

// Minimal ELF64 definitions (kept local so this file also builds on macOS for
// host-side testing against a pulled libart.so).
struct Elf64Ehdr {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct Elf64Shdr {
    uint32_t name;
    uint32_t type;
    uint64_t flags;
    uint64_t addr;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t addralign;
    uint64_t entsize;
};

struct Elf64Sym {
    uint32_t name;
    uint8_t info;
    uint8_t other;
    uint16_t shndx;
    uint64_t value;
    uint64_t size;
};

constexpr int kShtSymtab = 2;
constexpr int kShtDynsym = 11;
constexpr uint16_t kShnUndef = 0;

bool blobRead(const ArtResolver::Blob &blob, void *out, size_t size, uint64_t offset) {
    if (!blob.data || offset > blob.size || size > blob.size - offset) return false;
    memcpy(out, blob.data + offset, size);
    return true;
}

bool readEhdr(const ArtResolver::Blob &blob, Elf64Ehdr *ehdr) {
    if (!blobRead(blob, ehdr, sizeof(*ehdr), 0)) return false;
    return ehdr->ident[0] == 0x7f && ehdr->ident[1] == 'E' &&
           ehdr->ident[2] == 'L' && ehdr->ident[3] == 'F' &&
           ehdr->ident[4] == 2;  // ELFCLASS64
}

bool readShdr(const ArtResolver::Blob &blob, const Elf64Ehdr &ehdr,
              uint16_t index, Elf64Shdr *shdr) {
    return blobRead(blob, shdr, sizeof(*shdr),
                    ehdr.shoff + static_cast<uint64_t>(index) * ehdr.shentsize);
}

// XZ 解壓（xz-embedded，single-call mode）。mini debug info 只有幾 MB。
bool xzDecompress(const uint8_t *in, size_t in_size, std::vector<uint8_t> *out) {
    xz_crc32_init();
    xz_crc64_init();
    xz_dec *dec = xz_dec_init(XZ_SINGLE, 0);
    if (!dec) return false;

    size_t cap = 16 * 1024 * 1024;
    for (int attempt = 0; attempt < 3; ++attempt) {
        out->resize(cap);
        xz_buf buf{};
        buf.in = in;
        buf.in_pos = 0;
        buf.in_size = in_size;
        buf.out = out->data();
        buf.out_pos = 0;
        buf.out_size = cap;
        const xz_ret ret = xz_dec_run(dec, &buf);
        if (ret == XZ_STREAM_END) {
            out->resize(buf.out_pos);
            xz_dec_end(dec);
            return true;
        }
        if (ret == XZ_OK && buf.out_pos == buf.out_size) {
            cap *= 4;  // 輸出不夠，放大重試
            xz_dec_end(dec);
            dec = xz_dec_init(XZ_SINGLE, 0);
            if (!dec) return false;
            continue;
        }
        break;
    }
    xz_dec_end(dec);
    return false;
}

}  // namespace

ArtResolver::~ArtResolver() {
    freeSymtab(&dynsym_);
    freeSymtab(&symtab_);
    freeSymtab(&debugsym_);
}

void ArtResolver::freeSymtab(Symtab *tab) {
    free(tab->syms);
    free(tab->strs);
    tab->syms = nullptr;
    tab->strs = nullptr;
    tab->count = 0;
    tab->strs_size = 0;
}

bool ArtResolver::loadSymtab(const Blob &elf, int section_type, Symtab *out) {
    Elf64Ehdr ehdr{};
    if (!readEhdr(elf, &ehdr)) return false;

    for (uint16_t i = 0; i < ehdr.shnum; ++i) {
        Elf64Shdr shdr{};
        if (!readShdr(elf, ehdr, i, &shdr)) return false;
        if (shdr.type != section_type || shdr.entsize == 0 || shdr.link >= ehdr.shnum)
            continue;

        Elf64Shdr strhdr{};
        if (!readShdr(elf, ehdr, static_cast<uint16_t>(shdr.link), &strhdr))
            return false;

        out->count = static_cast<size_t>(shdr.size / shdr.entsize);
        out->syms = static_cast<uint8_t *>(malloc(shdr.size));
        out->strs = static_cast<char *>(malloc(strhdr.size));
        out->strs_size = static_cast<size_t>(strhdr.size);
        if (!out->syms || !out->strs) return false;
        if (shdr.offset + shdr.size > elf.size ||
            strhdr.offset + strhdr.size > elf.size)
            return false;
        memcpy(out->syms, elf.data + shdr.offset, shdr.size);
        memcpy(out->strs, elf.data + strhdr.offset, strhdr.size);
        return true;
    }
    return false;
}

bool ArtResolver::findSectionByName(const Blob &elf, const char *name,
                                    uint64_t *offset, uint64_t *size) {
    Elf64Ehdr ehdr{};
    if (!readEhdr(elf, &ehdr) || ehdr.shstrndx >= ehdr.shnum) return false;

    Elf64Shdr strhdr{};
    if (!readShdr(elf, ehdr, ehdr.shstrndx, &strhdr)) return false;
    if (strhdr.offset + strhdr.size > elf.size) return false;
    const char *names = reinterpret_cast<const char *>(elf.data + strhdr.offset);
    const size_t names_size = strhdr.size;

    for (uint16_t i = 0; i < ehdr.shnum; ++i) {
        Elf64Shdr shdr{};
        if (!readShdr(elf, ehdr, i, &shdr)) return false;
        if (shdr.name >= names_size) continue;
        const char *candidate = names + shdr.name;
        if (memchr(candidate, '\0', names_size - shdr.name) == nullptr) continue;
        if (strcmp(candidate, name) != 0) continue;
        if (shdr.offset + shdr.size > elf.size) return false;
        *offset = shdr.offset;
        *size = shdr.size;
        return true;
    }
    return false;
}

bool ArtResolver::parseFile(const char *path, uint64_t load_bias) {
    freeSymtab(&dynsym_);
    freeSymtab(&symtab_);
    freeSymtab(&debugsym_);
    ready_ = false;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    const off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size <= 0 || file_size > 256L * 1024 * 1024) {
        close(fd);
        return false;
    }
    std::vector<uint8_t> file(static_cast<size_t>(file_size));
    size_t done = 0;
    while (done < file.size()) {
        const ssize_t n = pread(fd, file.data() + done, file.size() - done, done);
        if (n <= 0) {
            close(fd);
            return false;
        }
        done += static_cast<size_t>(n);
    }
    close(fd);

    const Blob elf{file.data(), file.size()};
    bool ok = loadSymtab(elf, kShtDynsym, &dynsym_);
    loadSymtab(elf, kShtSymtab, &symtab_);  // 正式版通常沒有，無妨

    uint64_t debugdata_off = 0, debugdata_size = 0;
    if (findSectionByName(elf, ".gnu_debugdata", &debugdata_off, &debugdata_size)) {
        std::vector<uint8_t> decompressed;
        if (xzDecompress(file.data() + debugdata_off,
                         static_cast<size_t>(debugdata_size), &decompressed)) {
            const Blob debug_elf{decompressed.data(), decompressed.size()};
            loadSymtab(debug_elf, kShtSymtab, &debugsym_);
        }
    }

    load_bias_ = load_bias;
    ready_ = ok;
    return ok;
}

void *ArtResolver::lookup(const Symtab &tab, const char *name, int name_len,
                          bool prefix) const {
    if (!tab.syms || !tab.strs) return nullptr;
    for (size_t i = 0; i < tab.count; ++i) {
        Elf64Sym sym{};
        memcpy(&sym, tab.syms + i * sizeof(Elf64Sym), sizeof(sym));
        if (sym.shndx == kShnUndef || sym.name >= tab.strs_size) continue;
        const char *candidate = tab.strs + sym.name;
        if (memchr(candidate, '\0', tab.strs_size - sym.name) == nullptr) continue;
        const bool match = prefix
                ? strncmp(candidate, name, static_cast<size_t>(name_len)) == 0
                : strcmp(candidate, name) == 0;
        if (match) return reinterpret_cast<void *>(load_bias_ + sym.value);
    }
    return nullptr;
}

void *ArtResolver::lookupAll(const char *name, bool prefix) const {
    const int name_len = static_cast<int>(strlen(name));
    if (void *hit = lookup(dynsym_, name, name_len, prefix)) return hit;
    if (void *hit = lookup(symtab_, name, name_len, prefix)) return hit;
    return lookup(debugsym_, name, name_len, prefix);
}

void *ArtResolver::resolve(const char *symbol_name) const {
    if (!ready_) return nullptr;
    if (void *hit = lookupAll(symbol_name, false)) return hit;
    // Android LTO 的 internal-linkage 符號帶 .__uniq.<hash> 後綴
    char uniq[512];
    snprintf(uniq, sizeof(uniq), "%s.__uniq.", symbol_name);
    return lookupAll(uniq, true);
}

void *ArtResolver::resolvePrefix(const char *symbol_prefix) const {
    if (!ready_) return nullptr;
    return lookupAll(symbol_prefix, true);
}

#ifdef __ANDROID__
bool ArtResolver::init() {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return false;

    char line[1024];
    uint64_t base = 0;
    char path[512] = {0};
    while (fgets(line, sizeof(line), maps)) {
        if (!strstr(line, "libart.so")) continue;
        char *dash = strchr(line, '-');
        char *slash = strchr(line, '/');
        if (!dash || !slash) continue;
        base = strtoull(line, nullptr, 16);
        char *newline = strchr(slash, '\n');
        if (newline) *newline = '\0';
        // slash 已是路徑開頭（maps 行的第一個 '/' 只屬於路徑欄位）
        snprintf(path, sizeof(path), "%s", slash);
        break;  // 第一條 mapping 對應 file offset 0
    }
    fclose(maps);
    if (base == 0 || path[0] == '\0') return false;

    // st_value 是相對 load bias 的虛擬位址；libart.so 第一個 PT_LOAD 的
    // p_vaddr 為 0，因此 bias 就是 maps 裡的基底位址。
    return parseFile(path, base);
}
#endif
