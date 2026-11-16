#include "prx/libSceFreeTypeOt/include/SizeMetrics.hpp"
#include "prx/libc/include/General.hpp"

#if defined(ANYPS5_FREETYPEOT_SYSTEM_DEPS)
extern "C" {

void APS5_VABI FT_Select_Metrics_nid_postfix(FreeType::GuestFaceRec*, std::uint64_t) {
    NotImplemented_nid_no_patch(__func__);
}

std::int32_t APS5_VABI FT_Request_Metrics_nid_postfix(FreeType::GuestFaceRec*, const FreeType::GuestSizeRequestRec*) {
    NotImplemented_nid_no_patch(__func__);
}

std::int32_t APS5_VABI FT_Match_Size_nid_postfix(FreeType::GuestFaceRec*, const FreeType::GuestSizeRequestRec*, unsigned char, std::uint64_t*) {
    NotImplemented_nid_no_patch(__func__);
}

}
#else
#include <ft2build.h>
#include FT_FREETYPE_H
#include <freetype/internal/ftobjs.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

using FreeType::GuestBitmapSize;
using FreeType::GuestFaceRec;
using FreeType::GuestSizeMetrics;
using FreeType::GuestSizeRec;
using FreeType::GuestSizeRequestRec;

namespace {

bool CopyInPreservingErrno(void* destination, const void* source, std::size_t bytes) {
    const int savedErrno = errno;
    const bool ok = GuestCopyIn_nid_no_patch(destination, source, bytes);
    errno = savedErrno;
    return ok;
}

bool CopyOutPreservingErrno(void* destination, const void* source, std::size_t bytes) {
    const int savedErrno = errno;
    const bool ok = GuestCopyOut_nid_no_patch(destination, source, bytes);
    errno = savedErrno;
    return ok;
}

FT_Long Narrow(std::int64_t value, const char* function) {
    if (value < std::numeric_limits<FT_Long>::min() || value > std::numeric_limits<FT_Long>::max()) NotImplemented_nid_no_patch(function);
    return static_cast<FT_Long>(value);
}

void RequireNoOverflow(const GuestSizeRequestRec& req, const char* function) {
    std::int64_t product = 0;
    if (__builtin_mul_overflow(req.width, static_cast<std::int64_t>(req.horiResolution), &product) ||
        __builtin_mul_overflow(req.height, static_cast<std::int64_t>(req.vertResolution), &product) ||
        req.width > std::numeric_limits<std::int64_t>::max() - 64 || req.height > std::numeric_limits<std::int64_t>::max() - 64) {
        NotImplemented_nid_no_patch(function);
    }
}

FT_Size_RequestRec HostRequest(const GuestSizeRequestRec& req, const char* function) {
    RequireNoOverflow(req, function);
    return {static_cast<FT_Size_Request_Type>(req.type), Narrow(req.width, function), Narrow(req.height, function), req.horiResolution, req.vertResolution};
}

struct HostFace {
    FT_FaceRec face{};
    FT_SizeRec size{};
    std::vector<FT_Bitmap_Size> strikes;

    HostFace(const GuestFaceRec& guest, const GuestSizeRec* guestSize, const char* function) {
        face.face_flags = Narrow(guest.face_flags, function);
        face.units_per_EM = guest.units_per_EM;
        face.ascender = guest.ascender;
        face.descender = guest.descender;
        face.height = guest.height;
        face.max_advance_width = guest.max_advance_width;
        std::int64_t width = 0;
        std::int64_t height = 0;
        if (__builtin_sub_overflow(guest.bbox.xMax, guest.bbox.xMin, &width) || __builtin_sub_overflow(guest.bbox.yMax, guest.bbox.yMin, &height) ||
            width == std::numeric_limits<std::int64_t>::min() || height == std::numeric_limits<std::int64_t>::min()) {
            NotImplemented_nid_no_patch(function);
        }
        face.bbox = {Narrow(guest.bbox.xMin, function), Narrow(guest.bbox.yMin, function), Narrow(guest.bbox.xMax, function), Narrow(guest.bbox.yMax, function)};
        if (guest.num_fixed_sizes > 0) {
            if (!guest.available_sizes) NotImplemented_nid_no_patch(function);
            const auto base = reinterpret_cast<std::uintptr_t>(guest.available_sizes);
            const auto total = static_cast<std::size_t>(guest.num_fixed_sizes);
            constexpr std::size_t ChunkCount = 64;
            std::array<GuestBitmapSize, ChunkCount> chunk{};
            for (std::size_t offset = 0; offset < total;) {
                const std::size_t count = std::min(ChunkCount, total - offset);
                if (offset > (std::numeric_limits<std::uintptr_t>::max() - base) / sizeof(GuestBitmapSize)) {
                    NotImplemented_nid_no_patch(function);
                }
                const auto* source = reinterpret_cast<const GuestBitmapSize*>(base + offset * sizeof(GuestBitmapSize));
                if (!CopyInPreservingErrno(chunk.data(), source, count * sizeof(GuestBitmapSize))) {
                    NotImplemented_nid_no_patch(function);
                }
                for (std::size_t i = 0; i < count; ++i) {
                    const GuestBitmapSize& strike = chunk[i];
                    if (strike.x_ppem > std::numeric_limits<std::int64_t>::max() - 64 || strike.y_ppem > std::numeric_limits<std::int64_t>::max() - 64) {
                        NotImplemented_nid_no_patch(function);
                    }
                    strikes.push_back({strike.height, strike.width, Narrow(strike.size, function), Narrow(strike.x_ppem, function), Narrow(strike.y_ppem, function)});
                }
                offset += count;
            }
        }
        face.num_fixed_sizes = guest.num_fixed_sizes;
        face.available_sizes = strikes.data();
        face.size = &size;
        size.face = &face;
        if (!guestSize) return;
        const GuestSizeMetrics& metrics = guestSize->metrics;
        size.metrics = {metrics.x_ppem, metrics.y_ppem, Narrow(metrics.x_scale, function), Narrow(metrics.y_scale, function),
            Narrow(metrics.ascender, function), Narrow(metrics.descender, function), Narrow(metrics.height, function), Narrow(metrics.max_advance, function)};
    }

    void Store(GuestSizeMetrics& metrics) const {
        metrics.x_ppem = size.metrics.x_ppem;
        metrics.y_ppem = size.metrics.y_ppem;
        metrics.x_scale = size.metrics.x_scale;
        metrics.y_scale = size.metrics.y_scale;
        metrics.ascender = size.metrics.ascender;
        metrics.descender = size.metrics.descender;
        metrics.height = size.metrics.height;
        metrics.max_advance = size.metrics.max_advance;
    }
};

}

extern "C" {

void APS5_VABI FT_Select_Metrics_nid_postfix(GuestFaceRec* face, std::uint64_t strike_index) {
    if (!face) NotImplemented_nid_no_patch(__func__);
    GuestFaceRec guestFace{};
    if (!CopyInPreservingErrno(&guestFace, face, sizeof(guestFace))) NotImplemented_nid_no_patch(__func__);
    if (!guestFace.size || guestFace.num_fixed_sizes <= 0 || strike_index >= static_cast<std::uint64_t>(guestFace.num_fixed_sizes)) {
        NotImplemented_nid_no_patch(__func__);
    }
    GuestSizeRec guestSize{};
    if (!CopyInPreservingErrno(&guestSize, guestFace.size, sizeof(guestSize))) NotImplemented_nid_no_patch(__func__);
    HostFace host(guestFace, &guestSize, __func__);
    FT_Select_Metrics(&host.face, static_cast<FT_ULong>(strike_index));
    host.Store(guestSize.metrics);
    if (!CopyOutPreservingErrno(&guestFace.size->metrics, &guestSize.metrics, sizeof(guestSize.metrics))) {
        NotImplemented_nid_no_patch(__func__);
    }
}

std::int32_t APS5_VABI FT_Request_Metrics_nid_postfix(GuestFaceRec* face, const GuestSizeRequestRec* req) {
    if (!face) NotImplemented_nid_no_patch(__func__);
    GuestFaceRec guestFace{};
    if (!CopyInPreservingErrno(&guestFace, face, sizeof(guestFace))) NotImplemented_nid_no_patch(__func__);
    if (!guestFace.size) NotImplemented_nid_no_patch(__func__);
    GuestSizeRec guestSize{};
    if (!CopyInPreservingErrno(&guestSize, guestFace.size, sizeof(guestSize))) NotImplemented_nid_no_patch(__func__);
    HostFace host(guestFace, &guestSize, __func__);
    FT_Size_RequestRec request{};
    if (FT_IS_SCALABLE(&host.face)) {
        if (!req) NotImplemented_nid_no_patch(__func__);
        GuestSizeRequestRec guestReq{};
        if (!CopyInPreservingErrno(&guestReq, req, sizeof(guestReq))) NotImplemented_nid_no_patch(__func__);
        request = HostRequest(guestReq, __func__);
    }
    const FT_Error error = FT_Request_Metrics(&host.face, &request);
    host.Store(guestSize.metrics);
    if (!CopyOutPreservingErrno(&guestFace.size->metrics, &guestSize.metrics, sizeof(guestSize.metrics))) {
        NotImplemented_nid_no_patch(__func__);
    }
    return error;
}

std::int32_t APS5_VABI FT_Match_Size_nid_postfix(GuestFaceRec* face, const GuestSizeRequestRec* req, unsigned char ignore_width, std::uint64_t* size_index) {
    if (!face) NotImplemented_nid_no_patch(__func__);
    GuestFaceRec guestFace{};
    if (!CopyInPreservingErrno(&guestFace, face, sizeof(guestFace))) NotImplemented_nid_no_patch(__func__);
    if (!(guestFace.face_flags & FT_FACE_FLAG_FIXED_SIZES)) return FT_Err_Invalid_Face_Handle;
    if (!req) NotImplemented_nid_no_patch(__func__);
    GuestSizeRequestRec guestReq{};
    if (!CopyInPreservingErrno(&guestReq, req, sizeof(guestReq))) NotImplemented_nid_no_patch(__func__);
    if (guestReq.type != FT_SIZE_REQUEST_TYPE_NOMINAL) return FT_Err_Unimplemented_Feature;
    GuestSizeRec guestSize{};
    const GuestSizeRec* sizePtr = nullptr;
    if (guestFace.size) {
        if (!CopyInPreservingErrno(&guestSize, guestFace.size, sizeof(guestSize))) NotImplemented_nid_no_patch(__func__);
        sizePtr = &guestSize;
    }
    HostFace host(guestFace, sizePtr, __func__);
    FT_Size_RequestRec request = HostRequest(guestReq, __func__);
    FT_ULong index = 0;
    const FT_Error error = FT_Match_Size(&host.face, &request, ignore_width, &index);
    if (!error && size_index) {
        const std::uint64_t indexValue = index;
        if (!CopyOutPreservingErrno(size_index, &indexValue, sizeof(indexValue))) {
            NotImplemented_nid_no_patch(__func__);
        }
    }
    return error;
}

}
#endif
