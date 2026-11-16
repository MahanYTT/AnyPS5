#include "prx/libSceFreeTypeOt/include/Memory.hpp"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

using FreeType::GuestMemoryRec;

struct Recorder {
    GuestMemoryRec memory{};
    int allocs = 0;
    int frees = 0;
    int reallocs = 0;
    std::int64_t lastSize = 0;
    std::int64_t lastCurrent = 0;
    std::int64_t lastNew = 0;
    void* lastBlock = nullptr;
    GuestMemoryRec* lastMemory = nullptr;
    bool fail = false;
};

Recorder* Owner(GuestMemoryRec* memory) {
    return static_cast<Recorder*>(memory->user);
}

void* APS5_VABI RecordAlloc(GuestMemoryRec* memory, std::int64_t size) {
    auto* recorder = Owner(memory);
    ++recorder->allocs;
    recorder->lastMemory = memory;
    recorder->lastSize = size;
    if (recorder->fail) return nullptr;
    void* block = std::malloc(static_cast<std::size_t>(size));
    std::memset(block, 0xAA, static_cast<std::size_t>(size));
    return block;
}

void APS5_VABI RecordFree(GuestMemoryRec* memory, void* block) {
    auto* recorder = Owner(memory);
    ++recorder->frees;
    recorder->lastMemory = memory;
    recorder->lastBlock = block;
    std::free(block);
}

void* APS5_VABI RecordRealloc(GuestMemoryRec* memory, std::int64_t current, std::int64_t size, void* block) {
    auto* recorder = Owner(memory);
    ++recorder->reallocs;
    recorder->lastMemory = memory;
    recorder->lastCurrent = current;
    recorder->lastNew = size;
    recorder->lastBlock = block;
    if (recorder->fail) return nullptr;
    auto* resized = static_cast<unsigned char*>(std::realloc(block, static_cast<std::size_t>(size)));
    if (size > current) std::memset(resized + current, 0xAA, static_cast<std::size_t>(size - current));
    return resized;
}

void Reset(Recorder& recorder) {
    recorder = Recorder{};
    recorder.memory = {&recorder, RecordAlloc, RecordFree, RecordRealloc};
}

void Require(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FreeType Ot memory: %s\n", message);
    std::exit(1);
}

bool Filled(const void* block, std::size_t size, unsigned char value) {
    const auto* bytes = static_cast<const unsigned char*>(block);
    for (std::size_t i = 0; i < size; ++i) {
        if (bytes[i] != value) return false;
    }
    return true;
}

void QuickAllocation() {
    Recorder recorder;
    Reset(recorder);
    std::int32_t error = -1;
    void* block = ft_mem_qalloc_nid_postfix(&recorder.memory, 24, &error);
    Require(block && error == 0, "qalloc of a positive size returns a block and no error");
    Require(recorder.allocs == 1 && recorder.lastSize == 24 && recorder.lastMemory == &recorder.memory, "qalloc passes the memory record and size to alloc");
    Require(Filled(block, 24, 0xAA), "qalloc leaves the block uncleared");
    ft_mem_free_nid_postfix(&recorder.memory, block);
    Require(recorder.frees == 1 && recorder.lastBlock == block && recorder.lastMemory == &recorder.memory, "free passes the block to memory->free");

    Reset(recorder);
    error = -1;
    Require(!ft_mem_qalloc_nid_postfix(&recorder.memory, 0, &error) && error == 0 && recorder.allocs == 0, "qalloc of size 0 returns null without calling alloc");
    error = -1;
    Require(!ft_mem_qalloc_nid_postfix(&recorder.memory, -1, &error) && error == 0x06 && recorder.allocs == 0, "qalloc of a negative size is Invalid_Argument");
    recorder.fail = true;
    error = -1;
    Require(!ft_mem_qalloc_nid_postfix(&recorder.memory, 8, &error) && error == 0x40 && recorder.allocs == 1, "qalloc reports Out_Of_Memory when alloc fails");
}

void ClearedAllocation() {
    Recorder recorder;
    Reset(recorder);
    std::int32_t error = -1;
    void* block = ft_mem_alloc_nid_postfix(&recorder.memory, 16, &error);
    Require(block && error == 0 && recorder.allocs == 1 && recorder.lastSize == 16, "alloc allocates through memory->alloc");
    Require(Filled(block, 16, 0), "alloc clears the block");
    ft_mem_free_nid_postfix(&recorder.memory, block);

    error = -1;
    Require(!ft_mem_alloc_nid_postfix(&recorder.memory, 0, &error) && error == 0, "alloc of size 0 returns null");
    error = -1;
    Require(!ft_mem_alloc_nid_postfix(&recorder.memory, -5, &error) && error == 0x06, "alloc of a negative size is Invalid_Argument");
    recorder.fail = true;
    error = -1;
    Require(!ft_mem_alloc_nid_postfix(&recorder.memory, 4, &error) && error == 0x40, "alloc reports Out_Of_Memory when alloc fails");

    Reset(recorder);
    ft_mem_free_nid_postfix(&recorder.memory, nullptr);
    Require(recorder.frees == 0, "free of null does not call memory->free");
}

void QuickReallocation() {
    Recorder recorder;
    Reset(recorder);
    std::int32_t error = -1;
    void* block = ft_mem_qrealloc_nid_postfix(&recorder.memory, 4, 0, 3, nullptr, &error);
    Require(block && error == 0 && recorder.allocs == 1 && recorder.lastSize == 12 && recorder.reallocs == 0, "qrealloc from zero items allocates new_count * item_size");
    Require(Filled(block, 12, 0), "qrealloc from zero items clears the new block, as ft_mem_alloc does");
    std::memset(block, 0x11, 12);

    error = -1;
    void* grown = ft_mem_qrealloc_nid_postfix(&recorder.memory, 4, 3, 5, block, &error);
    Require(grown && error == 0 && recorder.reallocs == 1, "qrealloc of a live block calls memory->realloc");
    Require(recorder.lastCurrent == 12 && recorder.lastNew == 20 && recorder.lastBlock == block, "qrealloc passes byte sizes and the old block");
    Require(Filled(grown, 12, 0x11) && Filled(static_cast<char*>(grown) + 12, 8, 0xAA), "qrealloc does not clear the grown tail");

    recorder.fail = true;
    error = -1;
    Require(ft_mem_qrealloc_nid_postfix(&recorder.memory, 4, 5, 9, grown, &error) == grown && error == 0x40, "qrealloc keeps the old block when realloc fails");
    recorder.fail = false;

    const int callbacks = recorder.allocs + recorder.frees + recorder.reallocs;
    error = -1;
    Require(ft_mem_qrealloc_nid_postfix(&recorder.memory, -1, 5, 6, grown, &error) == grown && error == 0x06, "negative item_size is Invalid_Argument");
    error = -1;
    Require(ft_mem_qrealloc_nid_postfix(&recorder.memory, 4, -1, 6, grown, &error) == grown && error == 0x06, "negative cur_count is Invalid_Argument");
    error = -1;
    Require(ft_mem_qrealloc_nid_postfix(&recorder.memory, 4, 5, -1, grown, &error) == grown && error == 0x06, "negative new_count is Invalid_Argument");
    error = -1;
    Require(ft_mem_qrealloc_nid_postfix(&recorder.memory, 2, 5, 0x40000000, grown, &error) == grown && error == 0x0A, "new_count above FT_INT_MAX / item_size is Array_Too_Large");
    Require(recorder.allocs + recorder.frees + recorder.reallocs == callbacks, "rejected qrealloc calls make no memory callback");

    recorder.fail = true;
    error = -1;
    Require(!ft_mem_qrealloc_nid_postfix(&recorder.memory, 2, 0, 0x3FFFFFFF, nullptr, &error) && error == 0x40, "new_count at FT_INT_MAX / item_size is allocated");
    Require(recorder.lastSize == 0x7FFFFFFE, "the boundary allocation asks for new_count * item_size bytes");
    recorder.fail = false;

    error = -1;
    Require(!ft_mem_qrealloc_nid_postfix(&recorder.memory, 4, 5, 0, grown, &error) && error == 0, "qrealloc to zero items frees and returns null");
    Require(recorder.lastBlock == grown && recorder.frees == 1, "qrealloc to zero items passes the block to memory->free");

    block = ft_mem_qalloc_nid_postfix(&recorder.memory, 8, &error);
    error = -1;
    Require(!ft_mem_qrealloc_nid_postfix(&recorder.memory, 0, 2, 4, block, &error) && error == 0, "qrealloc with item_size 0 frees and returns null");
    Require(recorder.lastBlock == block && recorder.frees == 2, "qrealloc with item_size 0 passes the block to memory->free");
}

void ClearedReallocation() {
    Recorder recorder;
    Reset(recorder);
    std::int32_t error = -1;
    void* block = ft_mem_realloc_nid_postfix(&recorder.memory, 4, 0, 3, nullptr, &error);
    Require(block && error == 0 && recorder.lastSize == 12 && Filled(block, 12, 0), "realloc from zero items allocates and clears");
    auto* bytes = static_cast<unsigned char*>(block);
    for (int i = 0; i < 12; ++i) bytes[i] = static_cast<unsigned char>(i + 1);

    error = -1;
    void* grown = ft_mem_realloc_nid_postfix(&recorder.memory, 4, 3, 5, block, &error);
    Require(grown && error == 0 && recorder.reallocs == 1 && recorder.lastCurrent == 12 && recorder.lastNew == 20, "realloc resizes through memory->realloc");
    bytes = static_cast<unsigned char*>(grown);
    bool kept = true;
    for (int i = 0; i < 12; ++i) kept = kept && bytes[i] == i + 1;
    Require(kept && Filled(bytes + 12, 8, 0), "realloc keeps the old items and clears the new ones");

    std::memset(bytes, 0x22, 20);
    error = -1;
    void* shrunk = ft_mem_realloc_nid_postfix(&recorder.memory, 4, 5, 2, grown, &error);
    Require(shrunk && error == 0 && recorder.lastCurrent == 20 && recorder.lastNew == 8 && Filled(shrunk, 8, 0x22), "realloc shrinks without clearing");

    error = -1;
    Require(ft_mem_realloc_nid_postfix(&recorder.memory, 4, 2, -3, shrunk, &error) == shrunk && error == 0x06 && Filled(shrunk, 8, 0x22), "realloc errors keep the block unchanged");
    recorder.fail = true;
    error = -1;
    Require(ft_mem_realloc_nid_postfix(&recorder.memory, 4, 2, 6, shrunk, &error) == shrunk && error == 0x40 && Filled(shrunk, 8, 0x22), "realloc keeps the block when realloc fails");
    recorder.fail = false;
    ft_mem_free_nid_postfix(&recorder.memory, shrunk);
}

void Duplication() {
    Recorder recorder;
    Reset(recorder);
    const char source[7] = {'a', 'b', 'c', 0, 'd', 'e', 'f'};
    std::int32_t error = -1;
    void* copy = ft_mem_dup_nid_postfix(&recorder.memory, source, sizeof(source), &error);
    Require(copy && error == 0 && recorder.allocs == 1 && recorder.lastSize == 7, "dup allocates size bytes");
    Require(std::memcmp(copy, source, sizeof(source)) == 0, "dup copies every byte, including embedded zeros");
    ft_mem_free_nid_postfix(&recorder.memory, copy);

    error = -1;
    Require(!ft_mem_dup_nid_postfix(&recorder.memory, source, 0, &error) && error == 0 && recorder.allocs == 1, "dup of size 0 returns null");
    error = -1;
    void* uncopied = ft_mem_dup_nid_postfix(&recorder.memory, nullptr, 4, &error);
    Require(uncopied && error == 0 && recorder.allocs == 2 && Filled(uncopied, 4, 0xAA), "dup of a null address allocates without copying");
    ft_mem_free_nid_postfix(&recorder.memory, uncopied);
    error = -1;
    Require(!ft_mem_dup_nid_postfix(&recorder.memory, source, 0x8000000000000000ull, &error) && error == 0x06 && recorder.allocs == 2, "dup of a size above FT_Long is Invalid_Argument");

    error = -1;
    char* text = static_cast<char*>(ft_mem_strdup_nid_postfix(&recorder.memory, "hello", &error));
    Require(text && error == 0 && recorder.lastSize == 6 && std::strcmp(text, "hello") == 0, "strdup copies strlen + 1 bytes");
    ft_mem_free_nid_postfix(&recorder.memory, text);
    const int allocs = recorder.allocs;
    error = -1;
    Require(!ft_mem_strdup_nid_postfix(&recorder.memory, nullptr, &error) && error == 0 && recorder.allocs == allocs, "strdup of null returns null without allocating");
    recorder.fail = true;
    error = -1;
    Require(!ft_mem_strdup_nid_postfix(&recorder.memory, "x", &error) && error == 0x40, "strdup reports Out_Of_Memory when alloc fails");
}

void BoundedCopy() {
    char buffer[8];
    std::memset(buffer, 'x', sizeof(buffer));
    Require(ft_mem_strcpyn_nid_postfix(buffer, "abc", sizeof(buffer)) == 0 && std::strcmp(buffer, "abc") == 0, "strcpyn copies a string that fits and returns 0");
    std::memset(buffer, 'x', sizeof(buffer));
    Require(ft_mem_strcpyn_nid_postfix(buffer, "abc", 4) == 0 && std::strcmp(buffer, "abc") == 0, "strcpyn fits size - 1 characters");
    std::memset(buffer, 'x', sizeof(buffer));
    Require(ft_mem_strcpyn_nid_postfix(buffer, "abcdef", 4) == 1 && std::strcmp(buffer, "abc") == 0 && buffer[4] == 'x', "strcpyn truncates to size - 1 characters and returns 1");
    std::memset(buffer, 'x', sizeof(buffer));
    Require(ft_mem_strcpyn_nid_postfix(buffer, "a", 1) == 1 && buffer[0] == 0 && buffer[1] == 'x', "strcpyn of size 1 writes only the terminator");
    std::memset(buffer, 'x', sizeof(buffer));
    Require(ft_mem_strcpyn_nid_postfix(buffer, "a", 0) == 1 && buffer[0] == 0 && buffer[1] == 'x', "strcpyn of size 0 still writes the terminator");
    std::memset(buffer, 'x', sizeof(buffer));
    Require(ft_mem_strcpyn_nid_postfix(buffer, "", 5) == 0 && buffer[0] == 0 && buffer[1] == 'x', "strcpyn of an empty string returns 0");
}

template <typename T>
T* BadPointer(std::uintptr_t value = 0x10) {
    return reinterpret_cast<T*>(value);
}

template <class Call>
bool Rejects(Call call) {
    errno = 71;
    try {
        call();
    } catch (const std::runtime_error&) {
        return errno == 71;
    }
    return false;
}

struct GuardedPage {
    void* base = nullptr;
    std::size_t size = 0;

    GuardedPage() {
#ifdef _WIN32
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        size = info.dwPageSize;
        base = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
        size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        base = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED) base = nullptr;
#endif
        Require(base != nullptr, "allocate guarded page");
    }

    ~GuardedPage() {
        if (!base) return;
#ifdef _WIN32
        VirtualFree(base, 0, MEM_RELEASE);
#else
        munmap(base, size);
#endif
    }

    template <typename T>
    T* As() const {
        return static_cast<T*>(base);
    }

    void SetReadOnly() const {
#ifdef _WIN32
        DWORD prev = 0;
        Require(VirtualProtect(base, size, PAGE_READONLY, &prev) != 0, "protect page read-only");
#else
        Require(mprotect(base, size, PROT_READ) == 0, "protect page read-only");
#endif
    }

    void SetInaccessible() const {
#ifdef _WIN32
        DWORD prev = 0;
        Require(VirtualProtect(base, size, PAGE_NOACCESS, &prev) != 0, "protect page inaccessible");
#else
        Require(mprotect(base, size, PROT_NONE) == 0, "protect page inaccessible");
#endif
    }
};

void* APS5_VABI BadBlockAlloc(GuestMemoryRec*, std::int64_t) {
    return BadPointer<void>();
}

void UnmappedPointers() {
    Recorder recorder;
    Reset(recorder);
    std::int32_t error = -1;
    auto* badMemory = BadPointer<GuestMemoryRec>();
    auto* badError = BadPointer<std::int32_t>();
    auto* badBlock = BadPointer<void>();
    auto* badString = BadPointer<char>();

    GuardedPage readOnlyPage;
    readOnlyPage.SetReadOnly();
    auto* roError = readOnlyPage.As<std::int32_t>();
    auto* roDest = readOnlyPage.As<char>();

    GuardedPage noAccessPage;
    noAccessPage.SetInaccessible();
    auto* noAccessText = noAccessPage.As<const char>();

    Require(Rejects([&] { ft_mem_qalloc_nid_postfix(badMemory, 16, &error); }), "qalloc rejects unmapped memory");
    Require(Rejects([&] { ft_mem_qalloc_nid_postfix(nullptr, 16, &error); }), "qalloc rejects null memory");
    Require(Rejects([&] { ft_mem_qalloc_nid_postfix(&recorder.memory, 16, badError); }) && recorder.allocs == 0, "qalloc rejects unmapped error before allocating");
    Require(Rejects([&] { ft_mem_qalloc_nid_postfix(&recorder.memory, 0, badError); }), "qalloc of size 0 rejects unmapped error");
    Require(Rejects([&] { ft_mem_qalloc_nid_postfix(&recorder.memory, -1, roError); }), "qalloc rejects read-only error");
    Require(Rejects([&] { ft_mem_qalloc_nid_postfix(&recorder.memory, 16, nullptr); }) && recorder.allocs == 0, "qalloc rejects null error");

    Require(Rejects([&] { ft_mem_alloc_nid_postfix(badMemory, 16, &error); }), "alloc rejects unmapped memory");
    Require(Rejects([&] { ft_mem_alloc_nid_postfix(&recorder.memory, 16, badError); }) && recorder.allocs == 0, "alloc rejects unmapped error before allocating");
    Require(Rejects([&] { ft_mem_alloc_nid_postfix(&recorder.memory, 16, roError); }) && recorder.allocs == 0, "alloc rejects read-only error");

    GuestMemoryRec badAllocMemory{nullptr, BadBlockAlloc, RecordFree, RecordRealloc};
    Require(Rejects([&] { ft_mem_alloc_nid_postfix(&badAllocMemory, 16, &error); }), "alloc rejects unwritable returned block");

    Require(Rejects([&] { ft_mem_free_nid_postfix(badMemory, badBlock); }), "free rejects unmapped memory for non-null block");
    Require(Rejects([&] { ft_mem_free_nid_postfix(nullptr, badBlock); }), "free rejects null memory for non-null block");
    errno = 71;
    ft_mem_free_nid_postfix(badMemory, nullptr);
    Require(errno == 71, "free of null ignores unmapped memory and preserves errno");

    void* live = ft_mem_alloc_nid_postfix(&recorder.memory, 8, &error);
    Require(live != nullptr && error == 0, "live block allocated");
    Require(Rejects([&] { ft_mem_qrealloc_nid_postfix(badMemory, 4, 2, 4, live, &error); }), "qrealloc rejects unmapped memory");
    Require(Rejects([&] { ft_mem_qrealloc_nid_postfix(badMemory, 4, 2, 0, live, &error); }), "qrealloc to zero rejects unmapped memory");
    Require(Rejects([&] { ft_mem_qrealloc_nid_postfix(&recorder.memory, 4, 2, 4, live, badError); }), "qrealloc rejects unmapped error");
    Require(Rejects([&] { ft_mem_qrealloc_nid_postfix(&recorder.memory, 4, 2, 4, live, roError); }), "qrealloc rejects read-only error");
    Require(Rejects([&] { ft_mem_realloc_nid_postfix(badMemory, 4, 2, 4, live, &error); }), "realloc rejects unmapped memory");
    Require(Rejects([&] { ft_mem_realloc_nid_postfix(&recorder.memory, 4, 2, 4, live, badError); }), "realloc rejects unmapped error");
    ft_mem_free_nid_postfix(&recorder.memory, live);

    Reset(recorder);
    const char sample[] = "hello";
    Require(Rejects([&] { ft_mem_dup_nid_postfix(badMemory, sample, sizeof(sample), &error); }), "dup rejects unmapped memory");
    Require(Rejects([&] { ft_mem_dup_nid_postfix(&recorder.memory, sample, sizeof(sample), badError); }) && recorder.allocs == 0, "dup rejects unmapped error before allocating");
    Require(Rejects([&] { ft_mem_dup_nid_postfix(&recorder.memory, badString, 8, &error); }) && recorder.allocs == recorder.frees, "dup rejects unmapped source and frees temporary block");
    Require(Rejects([&] { ft_mem_dup_nid_postfix(&recorder.memory, noAccessText, 8, &error); }) && recorder.allocs == recorder.frees, "dup rejects inaccessible source");

    Reset(recorder);
    Require(Rejects([&] { ft_mem_strdup_nid_postfix(badMemory, "abc", &error); }), "strdup rejects unmapped memory");
    Require(Rejects([&] { ft_mem_strdup_nid_postfix(&recorder.memory, "abc", badError); }) && recorder.allocs == 0, "strdup rejects unmapped error");
    Require(Rejects([&] { ft_mem_strdup_nid_postfix(&recorder.memory, badString, &error); }) && recorder.allocs == 0, "strdup rejects unmapped string without allocating");
    Require(Rejects([&] { ft_mem_strdup_nid_postfix(&recorder.memory, noAccessText, &error); }) && recorder.allocs == 0, "strdup rejects inaccessible string");

    char destination[8] = {'z', 'z', 'z', 'z', 'z', 'z', 'z', 'z'};
    Require(Rejects([&] { ft_mem_strcpyn_nid_postfix(badString, "abc", 4); }), "strcpyn rejects unmapped destination");
    Require(Rejects([&] { ft_mem_strcpyn_nid_postfix(roDest, "abc", 4); }), "strcpyn rejects read-only destination");
    Require(Rejects([&] { ft_mem_strcpyn_nid_postfix(nullptr, "abc", 4); }), "strcpyn rejects null destination");
    Require(Rejects([&] { ft_mem_strcpyn_nid_postfix(destination, badString, 4); }) && destination[0] == 'z', "strcpyn rejects unmapped source without modifying destination");
    Require(Rejects([&] { ft_mem_strcpyn_nid_postfix(destination, badString, 0); }) && destination[0] == 'z', "strcpyn size 0 rejects unmapped source");
    Require(Rejects([&] { ft_mem_strcpyn_nid_postfix(destination, noAccessText, 4); }) && destination[0] == 'z', "strcpyn rejects inaccessible source");
    Require(Rejects([&] { ft_mem_strcpyn_nid_postfix(destination, nullptr, 4); }) && destination[0] == 'z', "strcpyn rejects null source");

    errno = 71;
    void* okBlock = ft_mem_alloc_nid_postfix(&recorder.memory, 12, &error);
    Require(errno == 71 && okBlock != nullptr && error == 0, "valid alloc preserves errno");
    void* okDup = ft_mem_dup_nid_postfix(&recorder.memory, "ok", 3, &error);
    Require(errno == 71 && okDup != nullptr && error == 0, "valid dup preserves errno");
    void* okStr = ft_mem_strdup_nid_postfix(&recorder.memory, "hi", &error);
    Require(errno == 71 && okStr != nullptr && error == 0, "valid strdup preserves errno");
    Require(ft_mem_strcpyn_nid_postfix(destination, "ok", sizeof(destination)) == 0 && errno == 71, "valid strcpyn preserves errno");
    ft_mem_free_nid_postfix(&recorder.memory, okBlock);
    ft_mem_free_nid_postfix(&recorder.memory, okDup);
    ft_mem_free_nid_postfix(&recorder.memory, okStr);
}

}

int main() {
    QuickAllocation();
    ClearedAllocation();
    QuickReallocation();
    ClearedReallocation();
    Duplication();
    BoundedCopy();
    UnmappedPointers();
    std::puts("Guest FreeType Ot memory tests passed");
    return 0;
}
