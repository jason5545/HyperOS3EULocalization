// libart.so symbol resolver for LSPlant InitInfo.
//
// Production Android builds strip .symtab from libart.so, but keep internal
// symbols in .gnu_debugdata (XZ-compressed mini debug info). This resolver
// reads, in order: .dynsym, .symtab, then the decompressed .gnu_debugdata
// symtab. Internal-linkage LTO symbols carry a ".__uniq.<hash>" suffix, so an
// exact lookup also tries "<name>.__uniq." prefix matching.
//
// Addresses are translated through the process load bias (st_value is a
// vaddr relative to the first PT_LOAD). The parser is self-contained and
// host-testable against a pulled libart.so.

#pragma once

#include <cstdint>
#include <cstddef>

class ArtResolver {
public:
    ArtResolver() = default;
    ~ArtResolver();

    ArtResolver(const ArtResolver &) = delete;
    ArtResolver &operator=(const ArtResolver &) = delete;

#ifdef __ANDROID__
    // Locates libart.so in /proc/self/maps and parses the backing file.
    bool init();
#endif

    // Parses the ELF64 shared object at `path`. `load_bias` is the runtime
    // address where the file's first PT_LOAD segment is mapped (0 when only
    // raw offsets are wanted, e.g. host-side testing).
    bool parseFile(const char *path, uint64_t load_bias);

    // Exact symbol match (including ".__uniq." LTO suffixes); returns
    // absolute address (bias + st_value), nullptr when not found.
    void *resolve(const char *symbol_name) const;

    // First symbol whose name starts with `prefix`, nullptr when none.
    void *resolvePrefix(const char *symbol_prefix) const;

    struct Blob {
        const uint8_t *data = nullptr;
        size_t size = 0;
    };

private:
    struct Symtab {
        uint8_t *syms = nullptr;      // copied Elf64_Sym array
        size_t count = 0;
        char *strs = nullptr;         // copied string table
        size_t strs_size = 0;
    };

    static bool loadSymtab(const Blob &elf, int section_type, Symtab *out);
    static bool findSectionByName(const Blob &elf, const char *name,
                                  uint64_t *offset, uint64_t *size);
    static void freeSymtab(Symtab *tab);
    void *lookup(const Symtab &tab, const char *name, int name_len,
                 bool prefix) const;
    void *lookupAll(const char *name, bool prefix) const;

    uint64_t load_bias_ = 0;
    Symtab dynsym_;
    Symtab symtab_;
    Symtab debugsym_;   // .gnu_debugdata 解出來的 mini symtab
    bool ready_ = false;
};
