#include "prx/libSceFreeTypeOt/include/Stream.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

using FreeType::GuestFrameField;
using FreeType::GuestMemoryRec;
using FreeType::GuestStreamRec;

constexpr int InvalidArgument = 0x06;
constexpr int InvalidStreamOperation = 0x55;

void Require(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FreeType Ot stream: %s\n", message);
    std::exit(1);
}

template <typename T>
T* BadPointer(std::uintptr_t value = 0x10) {
    return reinterpret_cast<T*>(value);
}

template <class Call>
bool Throws(Call call) {
    errno = 71;
    try {
        call();
    } catch (const std::invalid_argument&) {
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

struct Device {
    std::vector<std::uint8_t> source;
    unsigned allocs = 0;
    unsigned frees = 0;
    unsigned closes = 0;
};

Device device;

std::uint64_t APS5_VABI DeviceRead(GuestStreamRec*, std::uint64_t offset, std::uint8_t* buffer, std::uint64_t count) {
    if (!buffer && count == 0) return 0;
    std::uint64_t available = offset < device.source.size() ? device.source.size() - offset : 0;
    if (available > count) available = count;
    if (available) std::memcpy(buffer, device.source.data() + offset, static_cast<std::size_t>(available));
    return available;
}

void APS5_VABI DeviceClose(GuestStreamRec*) {
    ++device.closes;
}

void* APS5_VABI DeviceAlloc(GuestMemoryRec*, std::int64_t size) {
    ++device.allocs;
    return std::malloc(static_cast<std::size_t>(size));
}

void APS5_VABI DeviceFree(GuestMemoryRec*, void* block) {
    ++device.frees;
    std::free(block);
}

GuestMemoryRec memory{nullptr, DeviceAlloc, DeviceFree, nullptr};

void TestValidBehaviorAndErrno() {
    std::array<std::uint8_t, 8> data{{0x80, 0x01, 0x02, 0x03, 0xFE, 0xFF, 0x7F, 0x10}};
    GuestStreamRec stream{};
    errno = 71;
    FT_Stream_OpenMemory_nid_postfix(&stream, data.data(), data.size());
    Require(errno == 71 && stream.base == data.data() && stream.size == 8 && stream.pos == 0, "open memory sets fields and preserves errno");

    Require(FT_Stream_Seek_nid_postfix(&stream, 2) == 0 && errno == 71 && FT_Stream_Pos_nid_postfix(&stream) == 2 && errno == 71,
            "seek and pos preserve errno");
    Require(FT_Stream_Skip_nid_postfix(&stream, 1) == 0 && errno == 71 && stream.pos == 3, "skip preserves errno");

    std::array<std::uint8_t, 4> out{};
    Require(FT_Stream_Read_nid_postfix(&stream, out.data(), 2) == 0 && errno == 71 && out[0] == 0x03 && out[1] == 0xFE, "read preserves errno");
    Require(FT_Stream_ReadAt_nid_postfix(&stream, 0, out.data(), 2) == 0 && errno == 71 && out[0] == 0x80 && out[1] == 0x01,
            "read at preserves errno");
    Require(FT_Stream_TryRead_nid_postfix(&stream, out.data(), 2) == 2 && errno == 71 && out[0] == 0x02 && out[1] == 0x03,
            "try read preserves errno");

    Require(FT_Stream_EnterFrame_nid_postfix(&stream, 4) == 0 && errno == 71, "enter frame preserves errno");
    Require(FT_Stream_GetChar_nid_postfix(&stream) == -2 && errno == 71, "get char preserves errno");
    Require(FT_Stream_GetUShort_nid_postfix(&stream) == 0xFF7Fu && errno == 71, "get ushort preserves errno");
    FT_Stream_ExitFrame_nid_postfix(&stream);
    Require(errno == 71, "exit frame preserves errno");

    stream.pos = 0;
    std::uint8_t* frame = nullptr;
    Require(FT_Stream_ExtractFrame_nid_postfix(&stream, 4, &frame) == 0 && frame == data.data() && errno == 71,
            "extract frame preserves errno");
    FT_Stream_ReleaseFrame_nid_postfix(&stream, &frame);
    Require(!frame && errno == 71, "release frame preserves errno");

    stream.pos = 0;
    int error = -1;
    Require(FT_Stream_ReadChar_nid_postfix(&stream, &error) == -128 && error == 0 && errno == 71, "read char preserves errno");
    Require(FT_Stream_ReadUShort_nid_postfix(&stream, &error) == 0x0102u && error == 0 && errno == 71, "read ushort preserves errno");
    stream.pos = 5;
    Require(FT_Stream_ReadULong_nid_postfix(&stream, &error) == 0 && error == InvalidStreamOperation && errno == 71,
            "short read ulong preserves errno");

    stream.pos = 0;
    const std::array<GuestFrameField, 4> fields{{{4, 0, 4}, {8, 1, 0}, {12, 2, 2}, {0, 0, 0}}};
    std::array<std::uint8_t, 8> target{};
    Require(FT_Stream_ReadFields_nid_postfix(&stream, fields.data(), target.data()) == 0 && target[0] == 0x80 && errno == 71,
            "read fields preserves errno");

    FT_Stream_Close_nid_postfix(&stream);
    Require(errno == 71, "close preserves errno");
    FT_Stream_Free_nid_postfix(&stream, 1);
    Require(errno == 71, "free external preserves errno");
}

void TestUnmappedAndGuardedPointers() {
    auto* badStream = BadPointer<GuestStreamRec>();
    auto* badBuffer = BadPointer<std::uint8_t>();
    auto* badBytes = BadPointer<std::uint8_t*>();
    auto* badError = BadPointer<int>();
    auto* badFields = BadPointer<GuestFrameField>();
    auto* badStructure = BadPointer<void>();
    auto* badMemory = BadPointer<GuestMemoryRec>();

    GuardedPage readOnlyPage;
    GuestStreamRec roTemplate{};
    std::array<std::uint8_t, 8> validData{{1, 2, 3, 4, 5, 6, 7, 8}};
    roTemplate.base = validData.data();
    roTemplate.size = validData.size();
    *readOnlyPage.As<GuestStreamRec>() = roTemplate;
    readOnlyPage.SetReadOnly();
    auto* roStream = readOnlyPage.As<GuestStreamRec>();
    auto* roBuffer = readOnlyPage.As<std::uint8_t>();
    auto* roBytes = readOnlyPage.As<std::uint8_t*>();
    auto* roError = readOnlyPage.As<int>();

    GuardedPage noAccessPage;
    noAccessPage.SetInaccessible();
    auto* noAccessStream = noAccessPage.As<GuestStreamRec>();
    auto* noAccessBase = noAccessPage.As<std::uint8_t>();
    auto* noAccessFields = noAccessPage.As<GuestFrameField>();

    std::array<std::uint8_t, 8> data{{0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80}};
    GuestStreamRec stream{};
    FT_Stream_OpenMemory_nid_postfix(&stream, data.data(), data.size());

    Require(Throws([&] { FT_Stream_OpenMemory_nid_postfix(badStream, data.data(), data.size()); }), "open memory rejects unmapped stream");
    Require(Throws([&] { FT_Stream_OpenMemory_nid_postfix(roStream, data.data(), data.size()); }), "open memory rejects read-only stream");
    Require(Throws([&] { FT_Stream_Close_nid_postfix(badStream); }), "close rejects unmapped stream");
    Require(Throws([&] { FT_Stream_Close_nid_postfix(noAccessStream); }), "close rejects inaccessible stream");

    Require(Throws([&] { FT_Stream_Seek_nid_postfix(badStream, 0); }), "seek rejects unmapped stream");
    Require(Throws([&] { FT_Stream_Seek_nid_postfix(roStream, 0); }), "seek rejects read-only stream");
    Require(Throws([&] { FT_Stream_Skip_nid_postfix(badStream, 1); }), "skip rejects unmapped stream");
    errno = 71;
    Require(FT_Stream_Skip_nid_postfix(badStream, -1) == InvalidStreamOperation && errno == 71,
            "negative skip fails before reading unmapped stream");
    Require(Throws([&] { FT_Stream_Pos_nid_postfix(badStream); }), "pos rejects unmapped stream");

    std::array<std::uint8_t, 8> out{};
    Require(Throws([&] { FT_Stream_Read_nid_postfix(badStream, out.data(), 1); }), "read rejects unmapped stream");
    Require(Throws([&] { FT_Stream_Read_nid_postfix(&stream, badBuffer, 1); }), "read rejects unmapped buffer");
    Require(Throws([&] { FT_Stream_Read_nid_postfix(&stream, roBuffer, 1); }), "read rejects read-only buffer");
    Require(Throws([&] { FT_Stream_ReadAt_nid_postfix(badStream, 0, out.data(), 1); }), "read at rejects unmapped stream");
    Require(Throws([&] { FT_Stream_ReadAt_nid_postfix(&stream, 0, badBuffer, 1); }), "read at rejects unmapped buffer");
    Require(Throws([&] { FT_Stream_ReadAt_nid_postfix(&stream, 0, roBuffer, 1); }), "read at rejects read-only buffer");
    Require(Throws([&] { FT_Stream_TryRead_nid_postfix(badStream, out.data(), 1); }), "try read rejects unmapped stream");
    Require(Throws([&] { FT_Stream_TryRead_nid_postfix(&stream, badBuffer, 1); }), "try read rejects unmapped buffer");
    Require(Throws([&] { FT_Stream_TryRead_nid_postfix(&stream, roBuffer, 1); }), "try read rejects read-only buffer");

    GuestStreamRec badBaseStream{};
    FT_Stream_OpenMemory_nid_postfix(&badBaseStream, badBuffer, 8);
    Require(Throws([&] { FT_Stream_Read_nid_postfix(&badBaseStream, out.data(), 4); }), "read rejects unmapped memory base");
    FT_Stream_OpenMemory_nid_postfix(&badBaseStream, noAccessBase, 8);
    Require(Throws([&] { FT_Stream_TryRead_nid_postfix(&badBaseStream, out.data(), 4); }), "try read rejects inaccessible memory base");

    device = {};
    device.source = {1, 2, 3, 4, 5, 6, 7, 8};
    GuestStreamRec deviceStream{};
    deviceStream.size = device.source.size();
    deviceStream.read = DeviceRead;
    deviceStream.close = DeviceClose;
    deviceStream.memory = &memory;
    Require(Throws([&] { FT_Stream_ReadAt_nid_postfix(&deviceStream, 0, badBuffer, 4); }), "device read at rejects unmapped buffer");
    Require(Throws([&] { FT_Stream_TryRead_nid_postfix(&deviceStream, roBuffer, 4); }), "device try read rejects read-only buffer");

    Require(Throws([&] { FT_Stream_EnterFrame_nid_postfix(badStream, 4); }), "enter frame rejects unmapped stream");
    Require(Throws([&] { FT_Stream_EnterFrame_nid_postfix(roStream, 4); }), "enter frame rejects read-only stream");
    Require(Throws([&] { FT_Stream_ExitFrame_nid_postfix(badStream); }), "exit frame rejects unmapped stream");
    Require(Throws([&] { FT_Stream_ExitFrame_nid_postfix(roStream); }), "exit frame rejects read-only stream");

    deviceStream.memory = badMemory;
    Require(Throws([&] { FT_Stream_EnterFrame_nid_postfix(&deviceStream, 4); }), "device enter frame rejects unmapped memory");
    deviceStream.memory = &memory;

    stream.pos = 0;
    Require(Throws([&] { FT_Stream_ExtractFrame_nid_postfix(badStream, 4, &roBuffer); }), "extract frame rejects unmapped stream");
    Require(Throws([&] { FT_Stream_ExtractFrame_nid_postfix(&stream, 4, badBytes); }) && !stream.cursor && stream.pos == 0,
            "extract frame rejects unmapped bytes before entering frame");
    Require(Throws([&] { FT_Stream_ExtractFrame_nid_postfix(&stream, 4, roBytes); }) && !stream.cursor && stream.pos == 0,
            "extract frame rejects read-only bytes before entering frame");
    Require(Throws([&] { FT_Stream_ExtractFrame_nid_postfix(&deviceStream, 4, badBytes); }) && device.allocs == 0,
            "device extract frame rejects unmapped bytes without allocating");

    Require(Throws([&] { FT_Stream_ReleaseFrame_nid_postfix(&stream, badBytes); }), "release frame rejects unmapped bytes");
    Require(Throws([&] { FT_Stream_ReleaseFrame_nid_postfix(&stream, roBytes); }), "release frame rejects read-only bytes");
    std::uint8_t* frame = BadPointer<std::uint8_t>(0x1234);
    Require(Throws([&] { FT_Stream_ReleaseFrame_nid_postfix(badStream, &frame); }) && frame == BadPointer<std::uint8_t>(0x1234),
            "release frame rejects unmapped stream");
    deviceStream.memory = badMemory;
    Require(Throws([&] { FT_Stream_ReleaseFrame_nid_postfix(&deviceStream, &frame); }), "device release frame rejects unmapped memory");
    deviceStream.memory = &memory;

    GuestStreamRec frameStream{};
    frameStream.cursor = badBuffer;
    frameStream.limit = badBuffer + 8;
    Require(Throws([&] { FT_Stream_GetChar_nid_postfix(badStream); }), "get char rejects unmapped stream");
    Require(Throws([&] { FT_Stream_GetChar_nid_postfix(&frameStream); }), "get char rejects unmapped cursor");
    Require(Throws([&] { FT_Stream_GetShort_nid_postfix(&frameStream); }), "get short rejects unmapped cursor");
    Require(Throws([&] { FT_Stream_GetShortLE_nid_postfix(&frameStream); }), "get short LE rejects unmapped cursor");
    Require(Throws([&] { FT_Stream_GetUShort_nid_postfix(&frameStream); }), "get ushort rejects unmapped cursor");
    Require(Throws([&] { FT_Stream_GetUShortLE_nid_postfix(&frameStream); }), "get ushort LE rejects unmapped cursor");
    Require(Throws([&] { FT_Stream_GetOffset_nid_postfix(&frameStream); }), "get offset rejects unmapped cursor");
    Require(Throws([&] { FT_Stream_GetUOffset_nid_postfix(&frameStream); }), "get uoffset rejects unmapped cursor");
    Require(Throws([&] { FT_Stream_GetLong_nid_postfix(&frameStream); }), "get long rejects unmapped cursor");
    Require(Throws([&] { FT_Stream_GetLongLE_nid_postfix(&frameStream); }), "get long LE rejects unmapped cursor");
    Require(Throws([&] { FT_Stream_GetULong_nid_postfix(&frameStream); }), "get ulong rejects unmapped cursor");
    Require(Throws([&] { FT_Stream_GetULongLE_nid_postfix(&frameStream); }), "get ulong LE rejects unmapped cursor");

    int error = 0;
    stream.pos = 0;
    Require(Throws([&] { FT_Stream_ReadChar_nid_postfix(badStream, &error); }), "read char rejects unmapped stream");
    Require(Throws([&] { FT_Stream_ReadChar_nid_postfix(&stream, badError); }), "read char rejects unmapped error");
    Require(Throws([&] { FT_Stream_ReadChar_nid_postfix(&stream, roError); }), "read char rejects read-only error");
    Require(Throws([&] { FT_Stream_ReadChar_nid_postfix(&badBaseStream, &error); }), "read char rejects unmapped base");
    Require(Throws([&] { FT_Stream_ReadShort_nid_postfix(badStream, &error); }), "read short rejects unmapped stream");
    Require(Throws([&] { FT_Stream_ReadShort_nid_postfix(&stream, badError); }), "read short rejects unmapped error");
    Require(Throws([&] { FT_Stream_ReadULong_nid_postfix(&stream, roError); }), "read ulong rejects read-only error");
    Require(Throws([&] { FT_Stream_ReadULong_nid_postfix(&badBaseStream, &error); }), "read ulong rejects unmapped base");

    const std::array<GuestFrameField, 2> oneByte{{{8, 1, 0}, {0, 0, 0}}};
    const std::array<GuestFrameField, 2> copyBytes{{{24, 2, 0}, {0, 0, 0}}};
    const std::array<GuestFrameField, 3> deviceBytes{{{4, 0, 4}, {24, 2, 0}, {0, 0, 0}}};
    std::array<std::uint8_t, 8> target{};
    Require(Throws([&] { FT_Stream_ReadFields_nid_postfix(badStream, oneByte.data(), target.data()); }), "read fields rejects unmapped stream");
    Require(Throws([&] { FT_Stream_ReadFields_nid_postfix(&stream, badFields, target.data()); }), "read fields rejects unmapped fields");
    Require(Throws([&] { FT_Stream_ReadFields_nid_postfix(&stream, noAccessFields, target.data()); }),
            "read fields rejects inaccessible fields");
    stream.cursor = data.data();
    stream.limit = data.data() + 4;
    Require(Throws([&] { FT_Stream_ReadFields_nid_postfix(&stream, oneByte.data(), badStructure); }),
            "read fields rejects unmapped structure");
    Require(Throws([&] { FT_Stream_ReadFields_nid_postfix(&stream, oneByte.data(), roBuffer); }),
            "read fields rejects read-only structure");
    Require(Throws([&] { FT_Stream_ReadFields_nid_postfix(&stream, copyBytes.data(), badStructure); }),
            "read fields bytes rejects unmapped structure");
    frameStream.cursor = badBuffer;
    frameStream.limit = badBuffer + 4;
    Require(Throws([&] { FT_Stream_ReadFields_nid_postfix(&frameStream, oneByte.data(), target.data()); }),
            "read fields rejects unmapped cursor");
    Require(Throws([&] { FT_Stream_ReadFields_nid_postfix(&frameStream, copyBytes.data(), target.data()); }),
            "read fields bytes rejects unmapped cursor");
    deviceStream.pos = 0;
    Require(Throws([&] { FT_Stream_ReadFields_nid_postfix(&deviceStream, deviceBytes.data(), badStructure); }) &&
                device.allocs == 1 && device.frees == 1 && !deviceStream.base && !deviceStream.cursor && !deviceStream.limit,
            "read fields exits and frees device frame on unwritable structure");

    Require(Throws([&] { FT_Stream_Free_nid_postfix(badStream, 0); }), "free rejects unmapped stream");
    Require(Throws([&] { FT_Stream_Free_nid_postfix(badStream, 1); }), "external free rejects unmapped stream");
    deviceStream.memory = badMemory;
    const auto closesBefore = device.closes;
    Require(Throws([&] { FT_Stream_Free_nid_postfix(&deviceStream, 0); }) && device.closes == closesBefore,
            "free rejects unmapped memory before closing");
    errno = 71;
    FT_Stream_Free_nid_postfix(&deviceStream, 1);
    Require(device.closes == closesBefore + 1 && errno == 71, "external free ignores unmapped memory and preserves errno");
}

}

int main() {
    TestValidBehaviorAndErrno();
    TestUnmappedAndGuardedPointers();
    std::puts("Guest FreeType Ot stream tests passed");
    return 0;
}
