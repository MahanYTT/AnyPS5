// Derived from FreeType src/base/ftrfork.c (FreeType 2.4.12, commit cecf93ef90c660a8b0b45e5adbbfb5ea443fb6b9).
// Copyright 2004-2010, 2013 by Masatake YAMATO and Redhat K.K.
// This file is part of the FreeType project, and may only be used, modified, and distributed
// under the terms of the FreeType project license, LICENSE.TXT. By continuing to use, modify,
// or distribute this file you indicate that you have read the license and understand and accept it fully.

#include "prx/libSceFreeTypeOt/include/Raccess.hpp"
#include "prx/libSceFreeTypeOt/include/Memory.hpp"
#include "prx/libSceFreeTypeOt/include/System.hpp"
#include "prx/libc/include/General.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace FreeType;

namespace {

constexpr int CannotOpenResource = 0x01;
constexpr int UnknownFileFormat = 0x02;
constexpr int ArrayTooLarge = 0x0A;
constexpr int OutOfMemory = 0x40;
constexpr int CannotOpenStream = 0x51;
constexpr std::uint32_t OpenPathname = 0x4;
constexpr int RuleCount = 9;
constexpr std::int64_t IntMax = 0x7FFFFFFF;
constexpr std::int64_t RefSize = 16;
constexpr std::int64_t LongMax = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t LongMin = std::numeric_limits<std::int64_t>::min();

struct GuestRForkRef {
    std::uint16_t resId;
    std::uint64_t offset;
};

static_assert(sizeof(GuestRForkRef) == RefSize && offsetof(GuestRForkRef, offset) == 8);

bool CopyInPreservingErrno(void* destination, const void* source, std::size_t bytes) {
    const int saved = errno;
    const bool ok = GuestCopyIn_nid_no_patch(destination, source, bytes);
    errno = saved;
    return ok;
}

bool CopyOutPreservingErrno(void* destination, const void* source, std::size_t bytes) {
    const int saved = errno;
    const bool ok = GuestCopyOut_nid_no_patch(destination, source, bytes);
    errno = saved;
    return ok;
}

bool WritablePreservingErrno(void* destination, std::size_t bytes) {
    const int saved = errno;
    const bool ok = GuestWritable_nid_no_patch(destination, bytes);
    errno = saved;
    return ok;
}

void CheckStream(GuestStreamRec* stream, const char* function) {
    GuestStreamRec copy{};
    if (!CopyInPreservingErrno(&copy, stream, sizeof(copy)) || !WritablePreservingErrno(stream, sizeof(copy))) {
        NotImplemented_nid_no_patch(function);
    }
}

std::string CopyGuestCString(const char* source, const char* function) {
    std::string result;
    const auto base = reinterpret_cast<std::uintptr_t>(source);
    std::size_t offset = 0;
    for (;;) {
        if (offset > std::numeric_limits<std::uintptr_t>::max() - base) NotImplemented_nid_no_patch(function);
        const auto current = base + offset;
        const std::size_t toPageEnd = 4096 - (current % 4096);
        const std::size_t chunk = toPageEnd < 256 ? toPageEnd : 256;
        char buffer[256];
        if (!CopyInPreservingErrno(buffer, reinterpret_cast<const void*>(current), chunk)) {
            NotImplemented_nid_no_patch(function);
        }
        if (const auto* end = static_cast<const char*>(std::memchr(buffer, '\0', chunk))) {
            result.append(buffer, static_cast<std::size_t>(end - buffer));
            return result;
        }
        result.append(buffer, chunk);
        offset += chunk;
    }
}

std::int64_t Add(std::int64_t a, std::int64_t b, const char* function) {
    if ((b > 0 && a > LongMax - b) || (b < 0 && a < LongMin - b)) NotImplemented_nid_no_patch(function);
    return a + b;
}

std::int64_t Field(const std::uint8_t* head, unsigned index, const char* function) {
    if (head[index] >= 0x80) NotImplemented_nid_no_patch(function);
    return static_cast<std::int64_t>(head[index]) << 24 | head[index + 1] << 16 | head[index + 2] << 8 | head[index + 3];
}


int GuessAppleGeneric(GuestStreamRec* stream, std::int32_t magic, std::int64_t* resultOffset) {
    int error = 0;
    const auto magicFromStream = static_cast<std::int32_t>(FT_Stream_ReadLong_nid_postfix(stream, &error));
    if (error) return error;
    if (magicFromStream != magic) return UnknownFileFormat;
    FT_Stream_ReadLong_nid_postfix(stream, &error);
    if (error) return error;
    error = FT_Stream_Skip_nid_postfix(stream, 16);
    if (error) return error;
    const std::uint16_t entries = FT_Stream_ReadUShort_nid_postfix(stream, &error);
    if (error) return error;
    if (entries == 0) return UnknownFileFormat;
    for (int index = 0; index < entries; ++index) {
        const auto entryId = static_cast<std::uint32_t>(FT_Stream_ReadLong_nid_postfix(stream, &error));
        if (error) return error;
        if (entryId == 2) {
            const auto entryOffset = static_cast<std::uint32_t>(FT_Stream_ReadLong_nid_postfix(stream, &error));
            if (error) continue;
            FT_Stream_ReadLong_nid_postfix(stream, &error);
            if (error) continue;
            *resultOffset = entryOffset;
            return 0;
        }
        error = FT_Stream_Skip_nid_postfix(stream, 8);
        if (error) return error;
    }
    return UnknownFileFormat;
}

int GuessApple(GuestStreamRec* stream, std::int32_t magic, char** resultName, std::int64_t* resultOffset) {
    *resultName = nullptr;
    if (!stream) return CannotOpenStream;
    return GuessAppleGeneric(stream, magic, resultOffset);
}

char* MakeFileName(GuestMemoryRec* memory, const char* original, const char* insertion, const char* function) {
    const char* slash = std::strrchr(original, '/');
    std::string combined;
    if (slash) {
        const std::size_t prefix = static_cast<std::size_t>(slash - original) + 1;
        combined.reserve(prefix + std::strlen(insertion) + std::strlen(slash + 1));
        combined.append(original, prefix);
        combined.append(insertion);
        combined.append(slash + 1);
    } else {
        combined.reserve(std::strlen(insertion) + std::strlen(original));
        combined.append(insertion);
        combined.append(original);
    }
    int error = 0;
    auto* name = static_cast<char*>(ft_mem_alloc_nid_postfix(memory, static_cast<std::int64_t>(combined.size() + 1), &error));
    if (error || !name) return nullptr;
    if (!CopyOutPreservingErrno(name, combined.c_str(), combined.size() + 1)) {
        ft_mem_free_nid_postfix(memory, name);
        NotImplemented_nid_no_patch(function);
    }
    return name;
}

int GuessDoubleFromFileName(GuestRaccessLibrary* library, char* fileName, std::int64_t* resultOffset) {
    GuestStreamOpenArgs args{};
    args.flags = OpenPathname;
    args.pathname = fileName;
    GuestStreamRec* stream = nullptr;
    int error = FT_Stream_New_nid_postfix(library, &args, &stream);
    if (error) return error;
    char* unused = nullptr;
    try {
        error = GuessApple(stream, 0x00051607, &unused, resultOffset);
    } catch (...) {
        FT_Stream_Free_nid_postfix(stream, 0);
        throw;
    }
    FT_Stream_Free_nid_postfix(stream, 0);
    return error;
}

int GuessInsertedDouble(GuestRaccessLibrary* library, GuestMemoryRec* memory, const char* base, const char* insertion,
                        char** resultName, std::int64_t* resultOffset, const char* function) {
    char* name = MakeFileName(memory, base, insertion, function);
    if (!name) return OutOfMemory;
    int error;
    try {
        error = GuessDoubleFromFileName(library, name, resultOffset);
    } catch (...) {
        ft_mem_free_nid_postfix(memory, name);
        throw;
    }
    if (!error) {
        *resultName = name;
    } else {
        ft_mem_free_nid_postfix(memory, name);
    }
    return error;
}

int GuessInserted(GuestMemoryRec* memory, const char* base, const char* insertion, char** resultName,
                  std::int64_t* resultOffset, const char* function) {
    char* name = MakeFileName(memory, base, insertion, function);
    if (!name) return OutOfMemory;
    *resultName = name;
    *resultOffset = 0;
    return 0;
}

int GuessAppended(GuestMemoryRec* memory, const char* base, const char* suffix, char** resultName,
                  std::int64_t* resultOffset, const char* function) {
    const std::size_t length = std::strlen(base);
    const std::size_t suffixLength = std::strlen(suffix) + 1;
    if (length > static_cast<std::size_t>(IntMax) || suffixLength > static_cast<std::size_t>(IntMax) - length) return ArrayTooLarge;
    int error = 0;
    auto* name = static_cast<char*>(ft_mem_alloc_nid_postfix(memory, static_cast<std::int64_t>(length + suffixLength), &error));
    if (error) return error;
    std::string combined;
    combined.reserve(length + suffixLength - 1);
    combined.append(base, length);
    combined.append(suffix, suffixLength - 1);
    if (!CopyOutPreservingErrno(name, combined.c_str(), length + suffixLength)) {
        ft_mem_free_nid_postfix(memory, name);
        NotImplemented_nid_no_patch(function);
    }
    *resultName = name;
    *resultOffset = 0;
    return 0;
}

int GuessRule(int rule, GuestRaccessLibrary* library, GuestMemoryRec* memory, GuestStreamRec* stream, const char* base,
              char** resultName, std::int64_t* resultOffset, const char* function) {
    switch (rule) {
    case 0: return GuessApple(stream, 0x00051607, resultName, resultOffset);
    case 1: return GuessApple(stream, 0x00051600, resultName, resultOffset);
    case 2: return GuessInsertedDouble(library, memory, base, "._", resultName, resultOffset, function);
    case 3: return GuessAppended(memory, base, "/..namedfork/rsrc", resultName, resultOffset, function);
    case 4: return GuessAppended(memory, base, "/rsrc", resultName, resultOffset, function);
    case 5: return GuessInserted(memory, base, "resource.frk/", resultName, resultOffset, function);
    case 6: return GuessInserted(memory, base, ".resource/", resultName, resultOffset, function);
    case 7: return GuessInsertedDouble(library, memory, base, "%", resultName, resultOffset, function);
    default: return GuessInsertedDouble(library, memory, base, ".AppleDouble/", resultName, resultOffset, function);
    }
}

}

extern "C" {

int APS5_VABI FT_Raccess_Get_HeaderInfo_nid_postfix(GuestRaccessLibrary*, GuestStreamRec* stream, std::int64_t rforkOffset,
                                                    std::int64_t* mapOffset, std::int64_t* rdataPos) {
    const char* function = __func__;
    if (stream) CheckStream(stream, function);
    std::uint8_t head[16];
    std::uint8_t head2[16];
    int error = FT_Stream_Seek_nid_postfix(stream, static_cast<std::uint64_t>(rforkOffset));
    if (error) return error;
    error = FT_Stream_Read_nid_postfix(stream, head, 16);
    if (error) return error;
    const std::int64_t dataStart = Add(rforkOffset, Field(head, 0, function), function);
    const std::int64_t mapPos = Add(rforkOffset, Field(head, 4, function), function);
    const std::int64_t dataLength = Field(head, 8, function);
    if (!rdataPos || !CopyOutPreservingErrno(rdataPos, &dataStart, sizeof(dataStart))) NotImplemented_nid_no_patch(function);
    if (Add(dataStart, dataLength, function) != mapPos || mapPos == rforkOffset) return UnknownFileFormat;
    error = FT_Stream_Seek_nid_postfix(stream, static_cast<std::uint64_t>(mapPos));
    if (error) return error;
    head2[15] = static_cast<std::uint8_t>(head[15] + 1);
    error = FT_Stream_Read_nid_postfix(stream, head2, 16);
    if (error) return error;
    bool allZeros = true;
    bool allMatch = true;
    for (unsigned index = 0; index < 16; ++index) {
        if (head2[index] != 0) allZeros = false;
        if (head2[index] != head[index]) allMatch = false;
    }
    if (!allZeros && !allMatch) return UnknownFileFormat;
    FT_Stream_Skip_nid_postfix(stream, 8);
    const std::int64_t typeList = FT_Stream_ReadUShort_nid_postfix(stream, &error);
    if (error) return error;
    error = FT_Stream_Seek_nid_postfix(stream, static_cast<std::uint64_t>(Add(mapPos, typeList, function)));
    if (error) return error;
    const std::int64_t mapStart = mapPos + typeList;
    if (!mapOffset || !CopyOutPreservingErrno(mapOffset, &mapStart, sizeof(mapStart))) NotImplemented_nid_no_patch(function);
    return 0;
}

int APS5_VABI FT_Raccess_Get_DataOffsets_nid_postfix(GuestRaccessLibrary* library, GuestStreamRec* stream, std::int64_t mapOffset,
                                                     std::int64_t rdataPos, std::int64_t tag, std::int64_t** offsets, std::int64_t* count) {
    const char* function = __func__;
    GuestRaccessLibrary guestLibrary{};
    if (!library || !CopyInPreservingErrno(&guestLibrary, library, sizeof(guestLibrary))) NotImplemented_nid_no_patch(function);
    GuestMemoryRec* memory = guestLibrary.memory;
    if (stream) CheckStream(stream, function);
    int error = FT_Stream_Seek_nid_postfix(stream, static_cast<std::uint64_t>(mapOffset));
    if (error) return error;
    const int types = FT_Stream_ReadUShort_nid_postfix(stream, &error) + 1;
    if (error) return error;
    for (int type = 0; type < types; ++type) {
        const std::int64_t typeTag = FT_Stream_ReadLong_nid_postfix(stream, &error);
        if (error) return error;
        const int subCount = FT_Stream_ReadUShort_nid_postfix(stream, &error);
        if (error) return error;
        const std::int64_t refList = FT_Stream_ReadUShort_nid_postfix(stream, &error);
        if (error) return error;
        if (typeTag != tag) continue;
        const std::int64_t refs = subCount + 1;
        if (!count || !CopyOutPreservingErrno(count, &refs, sizeof(refs))) NotImplemented_nid_no_patch(function);
        error = FT_Stream_Seek_nid_postfix(stream, static_cast<std::uint64_t>(Add(refList, mapOffset, function)));
        if (error) return error;
        GuestMemoryRec guestMemory{};
        if (!memory || !CopyInPreservingErrno(&guestMemory, memory, sizeof(guestMemory))) NotImplemented_nid_no_patch(function);
        auto* ref = static_cast<GuestRForkRef*>(ft_mem_realloc_nid_postfix(memory, RefSize, 0, refs, nullptr, &error));
        if (error) return error;
        std::vector<GuestRForkRef> hostRefs(static_cast<std::size_t>(refs));
        for (std::int64_t index = 0; index < refs; ++index) {
            hostRefs[static_cast<std::size_t>(index)].resId = FT_Stream_ReadUShort_nid_postfix(stream, &error);
            if (!error) error = FT_Stream_Skip_nid_postfix(stream, 2);
            std::int64_t value = 0;
            if (!error) value = FT_Stream_ReadLong_nid_postfix(stream, &error);
            if (!error) error = FT_Stream_Skip_nid_postfix(stream, 4);
            if (error) {
                ft_mem_free_nid_postfix(memory, ref);
                return error;
            }
            hostRefs[static_cast<std::size_t>(index)].offset = static_cast<std::uint64_t>(value) & 0xFFFFFF;
        }
        std::stable_sort(hostRefs.begin(), hostRefs.end(), [](const GuestRForkRef& a, const GuestRForkRef& b) { return a.resId < b.resId; });
        for (std::size_t index = 1; index < hostRefs.size(); ++index) {
            if (hostRefs[index].resId == hostRefs[index - 1].resId && hostRefs[index].offset != hostRefs[index - 1].offset) {
                ft_mem_free_nid_postfix(memory, ref);
                NotImplemented_nid_no_patch(function);
            }
        }
        if (!CopyOutPreservingErrno(ref, hostRefs.data(), hostRefs.size() * sizeof(GuestRForkRef))) {
            ft_mem_free_nid_postfix(memory, ref);
            NotImplemented_nid_no_patch(function);
        }
        std::vector<std::int64_t> hostResult(static_cast<std::size_t>(refs));
        try {
            for (std::size_t index = 0; index < hostRefs.size(); ++index) {
                hostResult[index] = Add(rdataPos, static_cast<std::int64_t>(hostRefs[index].offset), function);
            }
        } catch (...) {
            ft_mem_free_nid_postfix(memory, ref);
            throw;
        }
        auto* result = static_cast<std::int64_t*>(ft_mem_realloc_nid_postfix(memory, sizeof(std::int64_t), 0, refs, nullptr, &error));
        if (!error) {
            if (!CopyOutPreservingErrno(result, hostResult.data(), hostResult.size() * sizeof(std::int64_t)) ||
                !offsets || !CopyOutPreservingErrno(offsets, &result, sizeof(result))) {
                ft_mem_free_nid_postfix(memory, result);
                ft_mem_free_nid_postfix(memory, ref);
                NotImplemented_nid_no_patch(function);
            }
        }
        ft_mem_free_nid_postfix(memory, ref);
        return error;
    }
    return CannotOpenResource;
}

void APS5_VABI FT_Raccess_Guess_nid_postfix(GuestRaccessLibrary* library, GuestStreamRec* stream, char* baseName, char** newNames,
                                           std::int64_t* offsets, std::int32_t* errors) {
    const char* function = __func__;
    GuestRaccessLibrary guestLibrary{};
    if (!library || !CopyInPreservingErrno(&guestLibrary, library, sizeof(guestLibrary))) NotImplemented_nid_no_patch(function);
    GuestMemoryRec guestMemory{};
    if (!guestLibrary.memory || !CopyInPreservingErrno(&guestMemory, guestLibrary.memory, sizeof(guestMemory))) {
        NotImplemented_nid_no_patch(function);
    }
    if (stream) CheckStream(stream, function);
    if (!newNames || !WritablePreservingErrno(newNames, sizeof(char*) * RuleCount) ||
        !offsets || !WritablePreservingErrno(offsets, sizeof(std::int64_t) * RuleCount) ||
        !errors || !WritablePreservingErrno(errors, sizeof(std::int32_t) * RuleCount)) {
        NotImplemented_nid_no_patch(function);
    }
    if (!baseName) NotImplemented_nid_no_patch(function);
    const std::string hostBaseName = CopyGuestCString(baseName, function);
    for (int rule = 0; rule < RuleCount; ++rule) {
        char* ruleName = nullptr;
        if (!CopyOutPreservingErrno(newNames + rule, &ruleName, sizeof(ruleName))) NotImplemented_nid_no_patch(function);
        std::int32_t ruleError = stream ? FT_Stream_Seek_nid_postfix(stream, 0) : 0;
        if (!CopyOutPreservingErrno(errors + rule, &ruleError, sizeof(ruleError))) NotImplemented_nid_no_patch(function);
        if (ruleError) continue;
        std::int64_t ruleOffset = 0;
        ruleError = GuessRule(rule, library, guestLibrary.memory, stream, hostBaseName.c_str(), &ruleName, &ruleOffset, function);
        if (!CopyOutPreservingErrno(errors + rule, &ruleError, sizeof(ruleError))) {
            ft_mem_free_nid_postfix(guestLibrary.memory, ruleName);
            NotImplemented_nid_no_patch(function);
        }
        if (!ruleError && !CopyOutPreservingErrno(offsets + rule, &ruleOffset, sizeof(ruleOffset))) {
            ft_mem_free_nid_postfix(guestLibrary.memory, ruleName);
            NotImplemented_nid_no_patch(function);
        }
        if (ruleName && !CopyOutPreservingErrno(newNames + rule, &ruleName, sizeof(ruleName))) {
            ft_mem_free_nid_postfix(guestLibrary.memory, ruleName);
            NotImplemented_nid_no_patch(function);
        }
    }
}

}
