#include "prx/libSceFreeTypeOt/include/Memory.hpp"
#include "prx/libSceFreeTypeOt/include/Raccess.hpp"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

using FreeType::GuestMemoryRec;
using FreeType::GuestRaccessLibrary;
using FreeType::GuestStreamRec;

constexpr std::int64_t Post = 0x504F5354;
constexpr std::int64_t Sfnt = 0x73666E74;

struct Recorder {
    GuestMemoryRec memory{};
    int allocs = 0;
    int frees = 0;
};

void* APS5_VABI RecordAlloc(GuestMemoryRec* memory, std::int64_t size) {
    ++static_cast<Recorder*>(memory->user)->allocs;
    return std::malloc(static_cast<std::size_t>(size));
}

void APS5_VABI RecordFree(GuestMemoryRec* memory, void* block) {
    ++static_cast<Recorder*>(memory->user)->frees;
    std::free(block);
}

void* APS5_VABI RecordRealloc(GuestMemoryRec*, std::int64_t, std::int64_t size, void* block) {
    return std::realloc(block, static_cast<std::size_t>(size));
}

void Require(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FreeType Ot raccess: %s\n", message);
    std::exit(1);
}

void Put16(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    bytes[at] = static_cast<std::uint8_t>(value >> 8);
    bytes[at + 1] = static_cast<std::uint8_t>(value);
}

void Put32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
    Put16(bytes, at, value >> 16);
    Put16(bytes, at + 2, value);
}

struct Fork {
    std::vector<std::uint8_t> bytes;
    std::size_t prefix;
    std::uint32_t dataStart = 0x100;
    std::uint32_t dataLength = 0x40;
    std::uint32_t mapPos = 0x140;
    std::uint32_t typeList = 28;
};

Fork MakeFork(std::size_t prefix, bool zeroCopy) {
    Fork fork;
    fork.prefix = prefix;
    fork.bytes.assign(prefix + 0x140 + 0x70, 0xEE);
    const std::size_t base = prefix;
    Put32(fork.bytes, base, fork.dataStart);
    Put32(fork.bytes, base + 4, fork.mapPos);
    Put32(fork.bytes, base + 8, fork.dataLength);
    Put32(fork.bytes, base + 12, 0x60);
    const std::size_t map = base + fork.mapPos;
    for (std::size_t index = 0; index < 16; ++index) fork.bytes[map + index] = zeroCopy ? 0 : fork.bytes[base + index];
    Put16(fork.bytes, map + 24, fork.typeList);
    const std::size_t types = map + fork.typeList;
    Put16(fork.bytes, types, 1);
    Put32(fork.bytes, types + 2, Sfnt);
    Put16(fork.bytes, types + 6, 0);
    Put16(fork.bytes, types + 8, 22);
    Put32(fork.bytes, types + 10, Post);
    Put16(fork.bytes, types + 14, 2);
    Put16(fork.bytes, types + 16, 34);
    const std::uint32_t refs[4][2] = {{0x0080, 0x00000010}, {0x0102, 0xA0000020}, {0x0100, 0x00000030}, {0x0101, 0x7F000008}};
    for (std::size_t index = 0; index < 4; ++index) {
        const std::size_t ref = types + 22 + index * 12;
        Put16(fork.bytes, ref, refs[index][0]);
        Put16(fork.bytes, ref + 2, 0xFFFF);
        Put32(fork.bytes, ref + 4, refs[index][1]);
        Put32(fork.bytes, ref + 8, 0);
    }
    return fork;
}

struct Session {
    Recorder recorder;
    GuestRaccessLibrary library{};
    GuestStreamRec stream{};

    explicit Session(const std::vector<std::uint8_t>& bytes) {
        recorder.memory = {&recorder, RecordAlloc, RecordFree, RecordRealloc};
        library.memory = &recorder.memory;
        FT_Stream_OpenMemory_nid_postfix(&stream, bytes.data(), bytes.size());
    }
};

void HeaderAndOffsets(std::size_t prefix, bool zeroCopy) {
    const Fork fork = MakeFork(prefix, zeroCopy);
    Session session(fork.bytes);
    std::int64_t mapOffset = -1;
    std::int64_t rdataPos = -1;
    const auto offset = static_cast<std::int64_t>(prefix);
    Require(FT_Raccess_Get_HeaderInfo_nid_postfix(&session.library, &session.stream, offset, &mapOffset, &rdataPos) == 0, "a resource fork header is accepted");
    Require(rdataPos == offset + 0x100 && mapOffset == offset + 0x140 + 28, "header info adds the fork offset to the data and type list positions");
    Require(session.stream.pos == static_cast<std::uint64_t>(mapOffset), "header info leaves the stream at the type list");

    std::int64_t* offsets = nullptr;
    std::int64_t count = 0;
    Require(FT_Raccess_Get_DataOffsets_nid_postfix(&session.library, &session.stream, mapOffset, rdataPos, Post, &offsets, &count) == 0, "a present tag is found");
    Require(count == 3 && offsets, "the subcount plus one references are returned");
    Require(offsets[0] == rdataPos + 0x30 && offsets[1] == rdataPos + 0x08 && offsets[2] == rdataPos + 0x20, "offsets are sorted by resource ID and drop the attribute byte");
    Require(session.recorder.allocs == 2 && session.recorder.frees == 1, "the reference array is freed and the offsets array is kept");
    ft_mem_free_nid_postfix(&session.recorder.memory, offsets);

    offsets = nullptr;
    count = 0;
    Require(FT_Raccess_Get_DataOffsets_nid_postfix(&session.library, &session.stream, mapOffset, rdataPos, Sfnt, &offsets, &count) == 0, "the first type is found");
    Require(count == 1 && offsets[0] == rdataPos + 0x10, "a single reference is returned");
    ft_mem_free_nid_postfix(&session.recorder.memory, offsets);

    offsets = nullptr;
    count = -1;
    const int allocs = session.recorder.allocs;
    Require(FT_Raccess_Get_DataOffsets_nid_postfix(&session.library, &session.stream, mapOffset, rdataPos, 0x4E464E54, &offsets, &count) == 0x01, "a missing tag is Cannot_Open_Resource");
    Require(count == -1 && !offsets && session.recorder.allocs == allocs, "a missing tag writes and allocates nothing");
}

void RejectedHeaders() {
    Fork fork = MakeFork(0, false);
    Put32(fork.bytes, 8, 0x41);
    Session lengths(fork.bytes);
    std::int64_t mapOffset = -1;
    std::int64_t rdataPos = -1;
    Require(FT_Raccess_Get_HeaderInfo_nid_postfix(&lengths.library, &lengths.stream, 0, &mapOffset, &rdataPos) == 0x02, "data that does not end at the map is Unknown_File_Format");
    Require(rdataPos == 0x100 && mapOffset == -1, "the data position is written before the length check");

    fork = MakeFork(0, false);
    fork.bytes[0x140 + 3] ^= 1;
    Session copy(fork.bytes);
    rdataPos = -1;
    Require(FT_Raccess_Get_HeaderInfo_nid_postfix(&copy.library, &copy.stream, 0, &mapOffset, &rdataPos) == 0x02, "a map header that neither matches nor is zero is Unknown_File_Format");

    fork = MakeFork(0, false);
    Put32(fork.bytes, 0, 0);
    Put32(fork.bytes, 4, 0);
    Put32(fork.bytes, 8, 0);
    Session empty(fork.bytes);
    Require(FT_Raccess_Get_HeaderInfo_nid_postfix(&empty.library, &empty.stream, 0, &mapOffset, &rdataPos) == 0x02, "a map at the fork offset is Unknown_File_Format");

    const std::vector<std::uint8_t> shortStream(10, 0);
    Session truncated(shortStream);
    Require(FT_Raccess_Get_HeaderInfo_nid_postfix(&truncated.library, &truncated.stream, 0, &mapOffset, &rdataPos) == 0x55, "a short header is Invalid_Stream_Operation");
    Require(FT_Raccess_Get_HeaderInfo_nid_postfix(&truncated.library, &truncated.stream, 11, &mapOffset, &rdataPos) == 0x55, "a fork offset past the stream is Invalid_Stream_Operation");
}

void TruncatedReferences() {
    Fork fork = MakeFork(0, false);
    fork.bytes.resize(0x140 + 28 + 22 + 12 + 6);
    Session session(fork.bytes);
    std::int64_t* offsets = nullptr;
    std::int64_t count = 0;
    Require(FT_Raccess_Get_DataOffsets_nid_postfix(&session.library, &session.stream, 0x140 + 28, 0x100, Post, &offsets, &count) == 0x55, "a cut reference list is Invalid_Stream_Operation");
    Require(count == 3 && !offsets, "the count is written before the references are read");
    Require(session.recorder.allocs == 1 && session.recorder.frees == 1, "the reference array is freed on a read error");
}

template <typename Call>
void RejectsUnmapped(Call&& call, const char* message) {
    errno = 71;
    bool threw = false;
    try {
        call();
    } catch (const std::runtime_error&) {
        threw = true;
    }
    Require(threw && errno == 71, message);
}

void GuestPointerFaultsAndErrno() {
    const Fork fork = MakeFork(0, false);
    Session session(fork.bytes);
    auto* badLibrary = reinterpret_cast<GuestRaccessLibrary*>(std::uintptr_t{0x10});
    auto* badStream = reinterpret_cast<GuestStreamRec*>(std::uintptr_t{0x10});
    auto* badInt64 = reinterpret_cast<std::int64_t*>(std::uintptr_t{0x10});
    auto* badOffsets = reinterpret_cast<std::int64_t**>(std::uintptr_t{0x10});
    auto* badBase = reinterpret_cast<char*>(std::uintptr_t{0x10});
    auto* badNames = reinterpret_cast<char**>(std::uintptr_t{0x10});
    auto* badErrors = reinterpret_cast<std::int32_t*>(std::uintptr_t{0x10});

    std::int64_t mapOffset = -1;
    std::int64_t rdataPos = -1;
    RejectsUnmapped([&] { FT_Raccess_Get_HeaderInfo_nid_postfix(&session.library, badStream, 0, &mapOffset, &rdataPos); },
        "header info rejects an unmapped stream");
    Require(mapOffset == -1 && rdataPos == -1, "header info writes nothing when stream is unmapped");
    RejectsUnmapped([&] { FT_Raccess_Get_HeaderInfo_nid_postfix(&session.library, &session.stream, 0, &mapOffset, badInt64); },
        "header info rejects an unmapped rdataPos");
    Require(mapOffset == -1, "header info stops before mapOffset when rdataPos is unmapped");
    RejectsUnmapped([&] { FT_Raccess_Get_HeaderInfo_nid_postfix(&session.library, &session.stream, 0, badInt64, &rdataPos); },
        "header info rejects an unmapped mapOffset");
    Require(rdataPos == 0x100, "header info still writes rdataPos before rejecting an unmapped mapOffset");

    errno = 71;
    Require(FT_Raccess_Get_HeaderInfo_nid_postfix(&session.library, &session.stream, 0, &mapOffset, &rdataPos) == 0 && errno == 71,
        "header info preserves errno");

    std::int64_t* offsets = nullptr;
    std::int64_t count = -1;
    RejectsUnmapped([&] { FT_Raccess_Get_DataOffsets_nid_postfix(badLibrary, &session.stream, mapOffset, rdataPos, Post, &offsets, &count); },
        "data offsets rejects an unmapped library");
    Require(count == -1 && !offsets, "data offsets writes nothing when library is unmapped");

    RejectsUnmapped([&] { FT_Raccess_Get_DataOffsets_nid_postfix(&session.library, badStream, mapOffset, rdataPos, Post, &offsets, &count); },
        "data offsets rejects an unmapped stream");
    Require(count == -1 && !offsets, "data offsets writes nothing when stream is unmapped");

    RejectsUnmapped([&] { FT_Raccess_Get_DataOffsets_nid_postfix(&session.library, &session.stream, mapOffset, rdataPos, Post, &offsets, badInt64); },
        "data offsets rejects an unmapped count");
    Require(!offsets && session.recorder.allocs == session.recorder.frees, "data offsets allocates nothing when count is unmapped");

    GuestRaccessLibrary brokenLibrary{};
    brokenLibrary.memory = reinterpret_cast<GuestMemoryRec*>(std::uintptr_t{0x10});
    RejectsUnmapped([&] { FT_Raccess_Get_DataOffsets_nid_postfix(&brokenLibrary, &session.stream, mapOffset, rdataPos, Post, &offsets, &count); },
        "data offsets rejects an unmapped library memory");
    Require(count == 3 && !offsets, "data offsets writes count before checking library memory");

    count = -1;
    RejectsUnmapped([&] { FT_Raccess_Get_DataOffsets_nid_postfix(&session.library, &session.stream, mapOffset, rdataPos, Post, badOffsets, &count); },
        "data offsets rejects an unmapped offsets output");
    Require(count == 3 && session.recorder.allocs == session.recorder.frees, "data offsets frees temporary and result allocations when offsets is unmapped");

    errno = 71;
    offsets = nullptr;
    count = -1;
    Require(FT_Raccess_Get_DataOffsets_nid_postfix(&session.library, &session.stream, mapOffset, rdataPos, Post, &offsets, &count) == 0 && errno == 71,
        "data offsets preserves errno");
    ft_mem_free_nid_postfix(&session.recorder.memory, offsets);

    char* guessNames[9]{};
    std::int64_t guessOffsets[9]{};
    std::int32_t guessErrors[9]{};
    for (int index = 0; index < 9; ++index) {
        guessNames[index] = reinterpret_cast<char*>(std::uintptr_t{1});
        guessOffsets[index] = -7;
        guessErrors[index] = -1;
    }
    char validBase[] = "/anyps5_missing_raccess_probe/font.ttf";
    RejectsUnmapped([&] { FT_Raccess_Guess_nid_postfix(badLibrary, nullptr, validBase, guessNames, guessOffsets, guessErrors); },
        "guess rejects an unmapped library");
    RejectsUnmapped([&] { FT_Raccess_Guess_nid_postfix(&brokenLibrary, nullptr, validBase, guessNames, guessOffsets, guessErrors); },
        "guess rejects an unmapped library memory");
    RejectsUnmapped([&] { FT_Raccess_Guess_nid_postfix(&session.library, badStream, validBase, guessNames, guessOffsets, guessErrors); },
        "guess rejects an unmapped stream");
    RejectsUnmapped([&] { FT_Raccess_Guess_nid_postfix(&session.library, nullptr, badBase, guessNames, guessOffsets, guessErrors); },
        "guess rejects an unmapped baseName");
    RejectsUnmapped([&] { FT_Raccess_Guess_nid_postfix(&session.library, nullptr, validBase, badNames, guessOffsets, guessErrors); },
        "guess rejects an unmapped newNames");
    RejectsUnmapped([&] { FT_Raccess_Guess_nid_postfix(&session.library, nullptr, validBase, guessNames, badInt64, guessErrors); },
        "guess rejects an unmapped offsets");
    RejectsUnmapped([&] { FT_Raccess_Guess_nid_postfix(&session.library, nullptr, validBase, guessNames, guessOffsets, badErrors); },
        "guess rejects an unmapped errors");
    Require(guessNames[0] == reinterpret_cast<char*>(std::uintptr_t{1}) && guessOffsets[0] == -7 && guessErrors[0] == -1,
        "guess leaves output arrays untouched when any pointer check fails");

    FT_Raccess_Guess_nid_postfix(&session.library, nullptr, validBase, guessNames, guessOffsets, guessErrors);
    Require(guessErrors[0] == 0x51 && guessErrors[1] == 0x51 && !guessNames[0] && !guessNames[1] && guessOffsets[0] == -7 && guessOffsets[1] == -7,
        "guess handles null stream for Apple rules without overwriting offsets");
    Require(guessErrors[3] == 0 && guessNames[3] && std::strcmp(guessNames[3], "/anyps5_missing_raccess_probe/font.ttf/..namedfork/rsrc") == 0 && guessOffsets[3] == 0,
        "guess produces appended rsrc path");
    for (char* name : guessNames) ft_mem_free_nid_postfix(&session.recorder.memory, name);
    Require(session.recorder.allocs == session.recorder.frees, "guess frees all allocated names");
}

}

int main() {
    HeaderAndOffsets(0, false);
    HeaderAndOffsets(0x80, true);
    RejectedHeaders();
    TruncatedReferences();
    GuestPointerFaultsAndErrno();
    std::puts("Guest FreeType Ot raccess tests passed");
    return 0;
}
