// Derived from FreeType src/base/ftsystem.c and src/base/ftobjs.c (FreeType 2.4.12, commit cecf93ef90c660a8b0b45e5adbbfb5ea443fb6b9).
// Copyright 1996-2013 by David Turner, Robert Wilhelm, and Werner Lemberg.
// This file is part of the FreeType project, and may only be used, modified, and distributed
// under the terms of the FreeType project license, LICENSE.TXT. By continuing to use, modify,
// or distribute this file you indicate that you have read the license and understand and accept it fully.

#include "prx/libSceFreeTypeOt/include/System.hpp"
#include "prx/libSceFreeTypeOt/include/Memory.hpp"
#include "prx/libc/include/FilesystemError.hpp"
#include "prx/libc/include/General.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <memory>
#include <string>
#include <system_error>

using namespace FreeType;

namespace {

constexpr int CannotOpenResource = 0x01;
constexpr int InvalidArgument = 0x06;
constexpr int InvalidLibraryHandle = 0x21;
constexpr int InvalidStreamHandle = 0x28;
constexpr int CannotOpenStream = 0x51;
constexpr std::uint32_t OpenMemory = 0x1;
constexpr std::uint32_t OpenStream = 0x2;
constexpr std::uint32_t OpenPathname = 0x4;

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

std::string CopyGuestCString(const char* source, const char* func) {
    std::string result;
    const auto base = reinterpret_cast<std::uintptr_t>(source);
    std::size_t offset = 0;
    for (;;) {
        if (offset > std::numeric_limits<std::uintptr_t>::max() - base) NotImplemented_nid_no_patch(func);
        const auto current = base + offset;
        const std::size_t toPageEnd = 4096 - (current % 4096);
        const std::size_t chunk = toPageEnd < 256 ? toPageEnd : 256;
        char buffer[256];
        if (!CopyInPreservingErrno(buffer, reinterpret_cast<const void*>(current), chunk)) {
            NotImplemented_nid_no_patch(func);
        }
        if (const auto* end = static_cast<const char*>(std::memchr(buffer, '\0', chunk))) {
            result.append(buffer, static_cast<std::size_t>(end - buffer));
            return result;
        }
        result.append(buffer, chunk);
        offset += chunk;
    }
}

void APS5_VABI StreamClose(GuestStreamRec* stream) {
    GuestStreamRec record{};
    if (!stream || !CopyInPreservingErrno(&record, stream, sizeof(record)) || !WritablePreservingErrno(stream, sizeof(record))) {
        NotImplemented_nid_no_patch(__func__);
    }
    delete static_cast<std::ifstream*>(record.descriptor);
    record.descriptor = nullptr;
    record.size = 0;
    record.base = nullptr;
    if (!CopyOutPreservingErrno(stream, &record, sizeof(record))) NotImplemented_nid_no_patch(__func__);
}

std::uint64_t APS5_VABI StreamIo(GuestStreamRec* stream, std::uint64_t offset, std::uint8_t* buffer, std::uint64_t count) {
    GuestStreamRec record{};
    if (!stream || !CopyInPreservingErrno(&record, stream, sizeof(record))) NotImplemented_nid_no_patch(__func__);
    if (!count && offset > record.size) return 1;
    if (count && (!buffer || !WritablePreservingErrno(buffer, 1))) NotImplemented_nid_no_patch(__func__);
    auto* file = static_cast<std::ifstream*>(record.descriptor);
    if (!file) NotImplemented_nid_no_patch(__func__);
    file->clear();
    if (record.pos != offset) {
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) NotImplemented_nid_no_patch("FT_Stream_Open");
        file->seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    }
    if (!count || !*file) return 0;
    std::uint64_t remaining = std::min<std::uint64_t>(count, static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()));
    std::uint64_t total = 0;
    std::array<char, 4096> chunk{};
    while (remaining > 0) {
        const auto ask = static_cast<std::streamsize>(std::min<std::uint64_t>(remaining, chunk.size()));
        file->read(chunk.data(), ask);
        const auto got = file->gcount();
        if (got <= 0) break;
        if (!CopyOutPreservingErrno(buffer + total, chunk.data(), static_cast<std::size_t>(got))) {
            NotImplemented_nid_no_patch(__func__);
        }
        total += static_cast<std::uint64_t>(got);
        remaining -= static_cast<std::uint64_t>(got);
        if (got < ask) break;
    }
    return total;
}

}

extern "C" {

int APS5_VABI FT_Stream_Open_nid_postfix(GuestStreamRec* stream, const char* filepathname) {
    if (!stream) return InvalidStreamHandle;
    GuestStreamRec record{};
    if (!CopyInPreservingErrno(&record, stream, sizeof(record)) || !WritablePreservingErrno(stream, sizeof(record))) {
        NotImplemented_nid_no_patch(__func__);
    }
    if (!filepathname) NotImplemented_nid_no_patch(__func__);
    const std::string pathCopy = CopyGuestCString(filepathname, __func__);
    record.descriptor = nullptr;
    record.pathname = const_cast<char*>(filepathname);
    record.base = nullptr;
    record.pos = 0;
    record.read = nullptr;
    record.close = nullptr;
    if (!CopyOutPreservingErrno(stream, &record, sizeof(record))) NotImplemented_nid_no_patch(__func__);
    std::filesystem::path hostPath;
    if (ResolvePathError_nid_no_patch(pathCopy.c_str(), hostPath) != 0) return CannotOpenResource;
    std::error_code error;
    const auto status = std::filesystem::status(hostPath, error);
    if (error || !std::filesystem::exists(status)) return CannotOpenResource;
    if (!std::filesystem::is_regular_file(status)) NotImplemented_nid_no_patch(__func__);
    auto file = std::make_unique<std::ifstream>(hostPath, std::ios::binary);
    if (!*file) return CannotOpenResource;
    file->seekg(0, std::ios::end);
    const std::streamoff size = file->tellg();
    if (size < 0) NotImplemented_nid_no_patch(__func__);
    record.size = static_cast<std::uint64_t>(size);
    if (!record.size) {
        if (!CopyOutPreservingErrno(stream, &record, sizeof(record))) NotImplemented_nid_no_patch(__func__);
        return CannotOpenStream;
    }
    file->seekg(0, std::ios::beg);
    record.descriptor = file.release();
    record.read = StreamIo;
    record.close = StreamClose;
    if (!CopyOutPreservingErrno(stream, &record, sizeof(record))) {
        delete static_cast<std::ifstream*>(record.descriptor);
        NotImplemented_nid_no_patch(__func__);
    }
    return 0;
}

int APS5_VABI FT_Stream_New_nid_postfix(GuestRaccessLibrary* library, const GuestStreamOpenArgs* args, GuestStreamRec** astream) {
    if (!astream || !WritablePreservingErrno(astream, sizeof(*astream))) NotImplemented_nid_no_patch(__func__);
    GuestStreamRec* nullStream = nullptr;
    if (!CopyOutPreservingErrno(astream, &nullStream, sizeof(nullStream))) NotImplemented_nid_no_patch(__func__);
    if (!library) return InvalidLibraryHandle;
    if (!args) return InvalidArgument;
    GuestRaccessLibrary hostLibrary{};
    if (!CopyInPreservingErrno(&hostLibrary, library, sizeof(hostLibrary))) NotImplemented_nid_no_patch(__func__);
    GuestMemoryRec* memory = hostLibrary.memory;
    if (!memory) NotImplemented_nid_no_patch(__func__);
    GuestStreamOpenArgs hostArgs{};
    if (!CopyInPreservingErrno(&hostArgs, args, sizeof(hostArgs))) NotImplemented_nid_no_patch(__func__);
    int error = 0;
    auto* stream = static_cast<GuestStreamRec*>(ft_mem_alloc_nid_postfix(memory, sizeof(GuestStreamRec), &error));
    if (error) return error;
    GuestStreamRec hostStream{};
    if (!CopyInPreservingErrno(&hostStream, stream, sizeof(hostStream)) || !WritablePreservingErrno(stream, sizeof(hostStream))) {
        ft_mem_free_nid_postfix(memory, stream);
        NotImplemented_nid_no_patch(__func__);
    }
    hostStream.memory = memory;
    if (!CopyOutPreservingErrno(stream, &hostStream, sizeof(hostStream))) {
        ft_mem_free_nid_postfix(memory, stream);
        NotImplemented_nid_no_patch(__func__);
    }
    if (hostArgs.flags & OpenMemory) {
        FT_Stream_OpenMemory_nid_postfix(stream, hostArgs.memoryBase, static_cast<std::uint64_t>(hostArgs.memorySize));
    } else if (hostArgs.flags & OpenPathname) {
        try {
            error = FT_Stream_Open_nid_postfix(stream, hostArgs.pathname);
        } catch (...) {
            ft_mem_free_nid_postfix(memory, stream);
            throw;
        }
    } else if ((hostArgs.flags & OpenStream) && hostArgs.stream) {
        ft_mem_free_nid_postfix(memory, stream);
        stream = hostArgs.stream;
    } else {
        error = InvalidArgument;
    }
    if (error) {
        ft_mem_free_nid_postfix(memory, stream);
        stream = nullptr;
    } else {
        if (!CopyInPreservingErrno(&hostStream, stream, sizeof(hostStream))) NotImplemented_nid_no_patch(__func__);
        hostStream.memory = memory;
        if (!CopyOutPreservingErrno(stream, &hostStream, sizeof(hostStream))) NotImplemented_nid_no_patch(__func__);
    }
    if (!CopyOutPreservingErrno(astream, &stream, sizeof(stream))) NotImplemented_nid_no_patch(__func__);
    return error;
}

}
