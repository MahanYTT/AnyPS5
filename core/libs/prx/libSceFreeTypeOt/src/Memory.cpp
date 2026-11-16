#include "prx/libSceFreeTypeOt/include/Memory.hpp"
#include "prx/libc/include/General.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

namespace {

constexpr std::int32_t InvalidArgument = 0x06;
constexpr std::int32_t ArrayTooLarge = 0x0A;
constexpr std::int32_t OutOfMemory = 0x40;
constexpr std::int64_t IntMax = 0x7FFFFFFF;

std::int64_t Bytes(std::int64_t count, std::int64_t itemSize) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(count) * static_cast<std::uint64_t>(itemSize));
}

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

FreeType::GuestMemoryRec ReadGuestMemory(FreeType::GuestMemoryRec* memory, const char* func) {
    FreeType::GuestMemoryRec host{};
    if (!memory || !CopyInPreservingErrno(&host, memory, sizeof(host))) NotImplemented_nid_no_patch(func);
    return host;
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

void ZeroGuestBytes(void* destination, std::size_t size, const char* func) {
    if (size == 0) return;
    if (!destination) NotImplemented_nid_no_patch(func);
    constexpr std::size_t ChunkSize = 4096;
    const char zeros[ChunkSize]{};
    auto* cursor = static_cast<char*>(destination);
    std::size_t remaining = size;
    while (remaining > 0) {
        const std::size_t chunk = remaining < ChunkSize ? remaining : ChunkSize;
        if (!CopyOutPreservingErrno(cursor, zeros, chunk)) NotImplemented_nid_no_patch(func);
        cursor += chunk;
        remaining -= chunk;
    }
}

}

extern "C" {

void* APS5_VABI ft_mem_qalloc_nid_postfix(FreeType::GuestMemoryRec* memory, std::int64_t size, std::int32_t* error) {
    if (!error || !WritablePreservingErrno(error, sizeof(*error))) NotImplemented_nid_no_patch(__func__);
    std::int32_t result = 0;
    void* block = nullptr;
    if (size > 0) {
        const auto hostMemory = ReadGuestMemory(memory, __func__);
        if (!hostMemory.alloc) NotImplemented_nid_no_patch(__func__);
        block = hostMemory.alloc(memory, size);
        if (!block) result = OutOfMemory;
    } else if (size < 0) {
        result = InvalidArgument;
    }
    if (!CopyOutPreservingErrno(error, &result, sizeof(result))) NotImplemented_nid_no_patch(__func__);
    return block;
}

void* APS5_VABI ft_mem_alloc_nid_postfix(FreeType::GuestMemoryRec* memory, std::int64_t size, std::int32_t* error) {
    if (!error || !WritablePreservingErrno(error, sizeof(*error))) NotImplemented_nid_no_patch(__func__);
    std::int32_t result = 0;
    void* block = ft_mem_qalloc_nid_postfix(memory, size, &result);
    if (!result && block && size > 0) ZeroGuestBytes(block, static_cast<std::size_t>(size), __func__);
    if (!CopyOutPreservingErrno(error, &result, sizeof(result))) NotImplemented_nid_no_patch(__func__);
    return block;
}

void APS5_VABI ft_mem_free_nid_postfix(FreeType::GuestMemoryRec* memory, const void* block) {
    if (!block) return;
    const auto hostMemory = ReadGuestMemory(memory, __func__);
    if (!hostMemory.free) NotImplemented_nid_no_patch(__func__);
    hostMemory.free(memory, const_cast<void*>(block));
}

void* APS5_VABI ft_mem_qrealloc_nid_postfix(FreeType::GuestMemoryRec* memory, std::int64_t itemSize, std::int64_t currentCount,
                                            std::int64_t newCount, void* block, std::int32_t* error) {
    if (!error || !WritablePreservingErrno(error, sizeof(*error))) NotImplemented_nid_no_patch(__func__);
    std::int32_t result = 0;
    if (currentCount < 0 || newCount < 0 || itemSize < 0) {
        result = InvalidArgument;
    } else if (newCount == 0 || itemSize == 0) {
        ft_mem_free_nid_postfix(memory, block);
        block = nullptr;
    } else if (newCount > IntMax / itemSize) {
        result = ArrayTooLarge;
    } else if (currentCount == 0) {
        block = ft_mem_alloc_nid_postfix(memory, newCount * itemSize, &result);
    } else {
        const auto hostMemory = ReadGuestMemory(memory, __func__);
        if (!hostMemory.realloc) NotImplemented_nid_no_patch(__func__);
        void* resized = hostMemory.realloc(memory, Bytes(currentCount, itemSize), newCount * itemSize, block);
        if (resized) {
            block = resized;
        } else {
            result = OutOfMemory;
        }
    }
    if (!CopyOutPreservingErrno(error, &result, sizeof(result))) NotImplemented_nid_no_patch(__func__);
    return block;
}

void* APS5_VABI ft_mem_realloc_nid_postfix(FreeType::GuestMemoryRec* memory, std::int64_t itemSize, std::int64_t currentCount,
                                           std::int64_t newCount, void* block, std::int32_t* error) {
    if (!error || !WritablePreservingErrno(error, sizeof(*error))) NotImplemented_nid_no_patch(__func__);
    std::int32_t result = 0;
    block = ft_mem_qrealloc_nid_postfix(memory, itemSize, currentCount, newCount, block, &result);
    if (!result && block && newCount > currentCount) {
        ZeroGuestBytes(static_cast<char*>(block) + currentCount * itemSize,
                       static_cast<std::size_t>((newCount - currentCount) * itemSize), __func__);
    }
    if (!CopyOutPreservingErrno(error, &result, sizeof(result))) NotImplemented_nid_no_patch(__func__);
    return block;
}

void* APS5_VABI ft_mem_dup_nid_postfix(FreeType::GuestMemoryRec* memory, const void* address, std::uint64_t size, std::int32_t* error) {
    if (!error || !WritablePreservingErrno(error, sizeof(*error))) NotImplemented_nid_no_patch(__func__);
    std::int32_t result = 0;
    void* copy = ft_mem_qalloc_nid_postfix(memory, static_cast<std::int64_t>(size), &result);
    if (!result && address && size > 0) {
        constexpr std::size_t ChunkSize = 4096;
        std::uint8_t buffer[ChunkSize]{};
        const auto total = static_cast<std::size_t>(size);
        for (std::size_t offset = 0; offset < total;) {
            const std::size_t chunk = (total - offset) < ChunkSize ? (total - offset) : ChunkSize;
            if (!CopyInPreservingErrno(buffer, static_cast<const char*>(address) + offset, chunk) ||
                !CopyOutPreservingErrno(static_cast<char*>(copy) + offset, buffer, chunk)) {
                ft_mem_free_nid_postfix(memory, copy);
                NotImplemented_nid_no_patch(__func__);
            }
            offset += chunk;
        }
    }
    if (!CopyOutPreservingErrno(error, &result, sizeof(result))) NotImplemented_nid_no_patch(__func__);
    return copy;
}

void* APS5_VABI ft_mem_strdup_nid_postfix(FreeType::GuestMemoryRec* memory, const char* string, std::int32_t* error) {
    if (!error || !WritablePreservingErrno(error, sizeof(*error))) NotImplemented_nid_no_patch(__func__);
    if (!string) return ft_mem_dup_nid_postfix(memory, nullptr, 0, error);
    const std::string copy = CopyGuestCString(string, __func__);
    return ft_mem_dup_nid_postfix(memory, copy.c_str(), copy.size() + 1, error);
}

std::int32_t APS5_VABI ft_mem_strcpyn_nid_postfix(char* destination, const char* source, std::uint64_t size) {
    if (!destination || !source) NotImplemented_nid_no_patch(__func__);
    const std::uint64_t maxCopy = size > 1 ? size - 1 : 0;
    const std::uint64_t maxRead = maxCopy + 1;
    std::string output;
    std::int32_t truncated = 1;
    const auto base = reinterpret_cast<std::uintptr_t>(source);
    std::uint64_t offset = 0;
    while (offset < maxRead) {
        if (offset > std::numeric_limits<std::uintptr_t>::max() - base) NotImplemented_nid_no_patch(__func__);
        const auto current = base + static_cast<std::uintptr_t>(offset);
        const std::size_t toPageEnd = 4096 - (current % 4096);
        const std::uint64_t remaining = maxRead - offset;
        std::size_t chunk = toPageEnd < 256 ? toPageEnd : 256;
        if (remaining < chunk) chunk = static_cast<std::size_t>(remaining);
        char buffer[256];
        if (!CopyInPreservingErrno(buffer, reinterpret_cast<const void*>(current), chunk)) {
            NotImplemented_nid_no_patch(__func__);
        }
        if (const auto* end = static_cast<const char*>(std::memchr(buffer, '\0', chunk))) {
            const auto prefix = static_cast<std::size_t>(end - buffer);
            const std::uint64_t keep = (offset + prefix) <= maxCopy ? prefix : static_cast<std::size_t>(maxCopy - offset);
            output.append(buffer, static_cast<std::size_t>(keep));
            truncated = 0;
            break;
        }
        const std::uint64_t canKeep = offset < maxCopy ? std::min<std::uint64_t>(chunk, maxCopy - offset) : 0;
        output.append(buffer, static_cast<std::size_t>(canKeep));
        offset += chunk;
    }
    output.push_back('\0');
    if (!CopyOutPreservingErrno(destination, output.data(), output.size())) {
        NotImplemented_nid_no_patch(__func__);
    }
    return truncated;
}

}
