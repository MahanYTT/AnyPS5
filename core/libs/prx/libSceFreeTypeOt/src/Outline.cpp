#include "prx/libSceFreeTypeOt/include/Outline.hpp"
#include "prx/libSceFreeTypeOt/include/Memory.hpp"
#include "prx/libc/include/General.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>

namespace {

constexpr int InvalidArgument = 0x06;
constexpr std::int32_t OutlineOwner = 0x1;

bool CopyIn(void* destination, const void* source, std::size_t bytes) {
    const int saved = errno;
    const bool ok = GuestCopyIn_nid_no_patch(destination, source, bytes);
    errno = saved;
    return ok;
}

bool CopyOut(void* destination, const void* source, std::size_t bytes) {
    const int saved = errno;
    const bool ok = GuestCopyOut_nid_no_patch(destination, source, bytes);
    errno = saved;
    return ok;
}

template <typename T>
T* NewArray(FreeType::GuestMemoryRec* memory, std::int64_t count, std::int32_t* error) {
    return static_cast<T*>(ft_mem_realloc_nid_postfix(memory, static_cast<std::int64_t>(sizeof(T)), 0, count, nullptr, error));
}

}

extern "C" {

int APS5_VABI FT_Outline_Done_Internal_nid_postfix(FreeType::GuestMemoryRec* memory, FreeType::GuestOutline* outline) {
    if (!memory || !outline) return InvalidArgument;
    FreeType::GuestMemoryRec localMemory{};
    if (!CopyIn(&localMemory, memory, sizeof(localMemory))) return InvalidArgument;
    FreeType::GuestOutline localOutline{};
    if (!CopyIn(&localOutline, outline, sizeof(localOutline))) return InvalidArgument;
    constexpr FreeType::GuestOutline empty{};
    if (!CopyOut(outline, &empty, sizeof(empty))) return InvalidArgument;
    if (localOutline.flags & OutlineOwner) {
        ft_mem_free_nid_postfix(memory, localOutline.points);
        ft_mem_free_nid_postfix(memory, localOutline.tags);
        ft_mem_free_nid_postfix(memory, localOutline.contours);
    }
    return 0;
}

int APS5_VABI FT_Outline_New_Internal_nid_postfix(FreeType::GuestMemoryRec* memory, std::uint32_t numPoints, std::int32_t numContours, FreeType::GuestOutline* anoutline) {
    if (!anoutline || !memory) return InvalidArgument;
    FreeType::GuestMemoryRec localMemory{};
    if (!CopyIn(&localMemory, memory, sizeof(localMemory))) return InvalidArgument;
    FreeType::GuestOutline localOutline{};
    if (!CopyOut(anoutline, &localOutline, sizeof(localOutline))) return InvalidArgument;
    std::int32_t error = 0;
    localOutline.points = NewArray<FreeType::GuestVector>(memory, numPoints, &error);
    if (!error) localOutline.tags = NewArray<unsigned char>(memory, numPoints, &error);
    if (!error) localOutline.contours = NewArray<std::uint16_t>(memory, numContours, &error);
    localOutline.flags |= OutlineOwner;
    if (error) {
        ft_mem_free_nid_postfix(memory, localOutline.points);
        ft_mem_free_nid_postfix(memory, localOutline.tags);
        ft_mem_free_nid_postfix(memory, localOutline.contours);
        return error;
    }
    localOutline.n_points = static_cast<std::uint16_t>(numPoints);
    localOutline.n_contours = static_cast<std::uint16_t>(numContours);
    if (!CopyOut(anoutline, &localOutline, sizeof(localOutline))) {
        ft_mem_free_nid_postfix(memory, localOutline.points);
        ft_mem_free_nid_postfix(memory, localOutline.tags);
        ft_mem_free_nid_postfix(memory, localOutline.contours);
        return InvalidArgument;
    }
    return 0;
}

}
