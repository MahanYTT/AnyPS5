// Derived from FreeType src/base/ftobjs.c (FreeType 2.4.12, commit cecf93ef90c660a8b0b45e5adbbfb5ea443fb6b9).
// Copyright 1996-2013 by David Turner, Robert Wilhelm, and Werner Lemberg.
// This file is part of the FreeType project, and may only be used, modified, and distributed
// under the terms of the FreeType project license, LICENSE.TXT. By continuing to use, modify,
// or distribute this file you indicate that you have read the license and understand and accept it fully.

#include "prx/libSceFreeTypeOt/include/GlyphSlot.hpp"
#include "prx/libSceFreeTypeOt/include/Memory.hpp"
#include "prx/libc/include/General.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>

using namespace FreeType;

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

bool WritablePreservingErrno(void* destination, std::size_t bytes) {
    const int savedErrno = errno;
    const bool ok = GuestWritable_nid_no_patch(destination, bytes);
    errno = savedErrno;
    return ok;
}

unsigned char** SlotBitmapBufferField(GuestGlyphSlotRec* slot) {
    return &slot->bitmap.buffer;
}

GuestMemoryRec* FaceMemory(const GuestGlyphSlotRec& slot, const char* function) {
    if (!slot.face) NotImplemented_nid_no_patch(function);
    GuestMemoryRec* memory = nullptr;
    const auto* face = static_cast<const GuestFaceRec*>(slot.face);
    if (!CopyInPreservingErrno(&memory, &face->memory, sizeof(memory))) NotImplemented_nid_no_patch(function);
    return memory;
}

void WriteSlotBitmapBuffer(GuestGlyphSlotRec* slot, unsigned char* buffer, const char* function) {
    if (!CopyOutPreservingErrno(SlotBitmapBufferField(slot), &buffer, sizeof(buffer))) {
        NotImplemented_nid_no_patch(function);
    }
}

void WriteInternalFlags(GuestSlotInternalRec* internal, std::uint32_t flags, const char* function) {
    if (!CopyOutPreservingErrno(&internal->flags, &flags, sizeof(flags))) {
        NotImplemented_nid_no_patch(function);
    }
}

void FreeOwned(GuestMemoryRec* memory, GuestGlyphSlotRec* slot, GuestGlyphSlotRec& guestSlot, const char* function) {
    if (guestSlot.bitmap.buffer && !memory) NotImplemented_nid_no_patch(function);
    ft_mem_free_nid_postfix(memory, guestSlot.bitmap.buffer);
    guestSlot.bitmap.buffer = nullptr;
    WriteSlotBitmapBuffer(slot, nullptr, function);
}

void FreeBitmap(GuestGlyphSlotRec* slot, const char* function) {
    if (!slot) NotImplemented_nid_no_patch(function);
    GuestGlyphSlotRec guestSlot{};
    if (!CopyInPreservingErrno(&guestSlot, slot, sizeof(guestSlot))) NotImplemented_nid_no_patch(function);
    auto* internal = static_cast<GuestSlotInternalRec*>(guestSlot.internal);
    if (internal) {
        GuestSlotInternalRec guestInternal{};
        if (!CopyInPreservingErrno(&guestInternal, internal, sizeof(guestInternal))) NotImplemented_nid_no_patch(function);
        if (guestInternal.flags & GlyphOwnBitmap) {
            unsigned char** slotBuffer = SlotBitmapBufferField(slot);
            if (!WritablePreservingErrno(slotBuffer, sizeof(*slotBuffer)) ||
                !WritablePreservingErrno(&internal->flags, sizeof(internal->flags))) {
                NotImplemented_nid_no_patch(function);
            }
            FreeOwned(FaceMemory(guestSlot, function), slot, guestSlot, function);
            WriteInternalFlags(internal, guestInternal.flags & ~GlyphOwnBitmap, function);
            return;
        }
    }
    WriteSlotBitmapBuffer(slot, nullptr, function);
}

}

extern "C" {

void APS5_VABI ft_glyphslot_free_bitmap_nid_postfix(GuestGlyphSlotRec* slot) {
    FreeBitmap(slot, __func__);
}

void APS5_VABI ft_glyphslot_set_bitmap_nid_postfix(GuestGlyphSlotRec* slot, std::uint8_t* buffer) {
    const char* function = __func__;
    FreeBitmap(slot, function);
    WriteSlotBitmapBuffer(slot, buffer, function);
}

int APS5_VABI ft_glyphslot_alloc_bitmap_nid_postfix(GuestGlyphSlotRec* slot, std::uint64_t size) {
    const char* function = __func__;
    if (!slot) NotImplemented_nid_no_patch(function);
    GuestGlyphSlotRec guestSlot{};
    if (!CopyInPreservingErrno(&guestSlot, slot, sizeof(guestSlot))) NotImplemented_nid_no_patch(function);
    auto* internal = static_cast<GuestSlotInternalRec*>(guestSlot.internal);
    if (!internal) NotImplemented_nid_no_patch(function);
    GuestSlotInternalRec guestInternal{};
    if (!CopyInPreservingErrno(&guestInternal, internal, sizeof(guestInternal))) NotImplemented_nid_no_patch(function);
    GuestMemoryRec* memory = FaceMemory(guestSlot, function);
    unsigned char** slotBuffer = SlotBitmapBufferField(slot);
    if (!WritablePreservingErrno(slotBuffer, sizeof(*slotBuffer)) ||
        !WritablePreservingErrno(&internal->flags, sizeof(internal->flags))) {
        NotImplemented_nid_no_patch(function);
    }
    if (guestInternal.flags & GlyphOwnBitmap) {
        FreeOwned(memory, slot, guestSlot, function);
    } else {
        WriteInternalFlags(internal, guestInternal.flags | GlyphOwnBitmap, function);
    }
    const auto bytes = static_cast<std::int64_t>(size);
    if (bytes > 0 && !memory) NotImplemented_nid_no_patch(function);
    std::int32_t error = 0;
    auto* buffer = static_cast<unsigned char*>(ft_mem_alloc_nid_postfix(memory, bytes, &error));
    if (!CopyOutPreservingErrno(slotBuffer, &buffer, sizeof(buffer))) {
        if (buffer) ft_mem_free_nid_postfix(memory, buffer);
        NotImplemented_nid_no_patch(function);
    }
    return error;
}

}
