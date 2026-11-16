// Derived from FreeType src/base/ftbitmap.c (FreeType 2.4.12, commit cecf93ef90c660a8b0b45e5adbbfb5ea443fb6b9).
// Copyright 2004-2009, 2011, 2013 by David Turner, Robert Wilhelm, and Werner Lemberg.
// This file is part of the FreeType project, and may only be used, modified, and distributed
// under the terms of the FreeType project license, LICENSE.TXT. By continuing to use, modify,
// or distribute this file you indicate that you have read the license and understand and accept it fully.

#include "prx/libSceFreeTypeOt/include/Bitmap.hpp"
#include "prx/libSceFreeTypeOt/include/GlyphSlot.hpp"
#include "prx/libSceFreeTypeOt/include/Memory.hpp"
#include "prx/libc/include/General.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

using namespace FreeType;

namespace {

constexpr std::int64_t IntMax = std::numeric_limits<std::int32_t>::max();
constexpr std::int64_t IntMin = std::numeric_limits<std::int32_t>::min();

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

bool WritablePreservingErrno(void* destination, std::size_t bytes) {
    const int savedErrno = errno;
    const bool ok = GuestWritable_nid_no_patch(destination, bytes);
    errno = savedErrno;
    return ok;
}

GuestBitmap* SlotBitmapField(GuestGlyphSlotRec* slot) {
    return &slot->bitmap;
}

std::int32_t CopyBitmap(GuestBitmapLibrary* library, const GuestBitmap* source, GuestBitmap* target, const char* function) {
    if (!library) NotImplemented_nid_no_patch(function);
    GuestBitmapLibrary guestLibrary{};
    if (!CopyInPreservingErrno(&guestLibrary, library, sizeof(guestLibrary))) NotImplemented_nid_no_patch(function);
    GuestMemoryRec* memory = guestLibrary.memory;
    if (!source->buffer) {
        *target = *source;
        return 0;
    }
    std::int64_t pitch = source->pitch;
    if (pitch == IntMin) NotImplemented_nid_no_patch(function);
    if (pitch < 0) pitch = -pitch;
    const std::int64_t size = pitch * static_cast<std::int32_t>(source->rows);
    if (size > IntMax || size < IntMin) NotImplemented_nid_no_patch(function);
    if (size > 0 && !memory) NotImplemented_nid_no_patch(function);
    std::int32_t error = 0;
    auto* buffer = static_cast<unsigned char*>(ft_mem_qalloc_nid_postfix(memory, size, &error));
    if (!error) {
        *target = *source;
        target->buffer = buffer;
        if (size > 0 && !CopyInPreservingErrno(buffer, source->buffer, static_cast<std::size_t>(size))) {
            ft_mem_free_nid_postfix(memory, buffer);
            NotImplemented_nid_no_patch(function);
        }
    }
    return error;
}

}

extern "C" {

int APS5_VABI sceFreeTypeOtGlyphSlotOwnBitmap_nid_no_patch(GuestGlyphSlotRec* slot) {
    const char* function = __func__;
    if (!slot) return 0;
    GuestGlyphSlotRec guestSlot{};
    if (!CopyInPreservingErrno(&guestSlot, slot, sizeof(guestSlot))) NotImplemented_nid_no_patch(function);
    if (guestSlot.format != GlyphFormatBitmap) return 0;
    auto* internal = static_cast<GuestSlotInternalRec*>(guestSlot.internal);
    if (!internal) NotImplemented_nid_no_patch(function);
    GuestSlotInternalRec guestInternal{};
    if (!CopyInPreservingErrno(&guestInternal, internal, sizeof(guestInternal))) NotImplemented_nid_no_patch(function);
    if (guestInternal.flags & GlyphOwnBitmap) return 0;
    GuestBitmap* slotBitmap = SlotBitmapField(slot);
    if (!WritablePreservingErrno(slotBitmap, sizeof(*slotBitmap)) ||
        !WritablePreservingErrno(&internal->flags, sizeof(internal->flags))) {
        NotImplemented_nid_no_patch(function);
    }
    GuestBitmap bitmap{};
    const std::int32_t error = CopyBitmap(static_cast<GuestBitmapLibrary*>(guestSlot.library), &guestSlot.bitmap, &bitmap, function);
    if (error) return error;
    if (!CopyOutPreservingErrno(slotBitmap, &bitmap, sizeof(bitmap))) {
        GuestBitmapLibrary guestLibrary{};
        if (CopyInPreservingErrno(&guestLibrary, guestSlot.library, sizeof(guestLibrary))) {
            ft_mem_free_nid_postfix(guestLibrary.memory, bitmap.buffer);
        }
        NotImplemented_nid_no_patch(function);
    }
    guestInternal.flags |= GlyphOwnBitmap;
    if (!CopyOutPreservingErrno(&internal->flags, &guestInternal.flags, sizeof(guestInternal.flags))) {
        NotImplemented_nid_no_patch(function);
    }
    return 0;
}
APS5_EXPORT("nSIux4OQyJM", sceFreeTypeOtGlyphSlotOwnBitmap_nid_no_patch);

}
