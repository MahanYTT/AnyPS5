#include "prx/libSceFreeTypeOt/include/Stream.hpp"
#include "prx/libc/include/General.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

using namespace FreeType;

namespace {

constexpr int InvalidArgument = 0x06;
constexpr int OutOfMemory = 0x40;
constexpr int InvalidStreamOperation = 0x55;

constexpr std::uint8_t FrameStart = 4;
constexpr std::uint8_t FrameBytes = 24;
constexpr std::uint8_t FrameSkip = 25;
constexpr std::uint8_t FrameSigned = 1;

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

[[noreturn]] void Unsupported(const char* function, const char* what) {
    throw std::invalid_argument(std::string(function) + ": " + what);
}

GuestStreamRec ReadStream(const GuestStreamRec* stream, const char* function) {
    if (!stream) Unsupported(function, "null stream");
    GuestStreamRec record{};
    if (!CopyInPreservingErrno(&record, stream, sizeof(record))) Unsupported(function, "unreadable stream");
    return record;
}

GuestStreamRec ReadWritableStream(GuestStreamRec* stream, const char* function) {
    if (!stream) Unsupported(function, "null stream");
    GuestStreamRec record{};
    if (!CopyInPreservingErrno(&record, stream, sizeof(record))) Unsupported(function, "unreadable stream");
    if (!WritablePreservingErrno(stream, sizeof(record))) Unsupported(function, "unwritable stream");
    return record;
}

void WriteStream(GuestStreamRec* stream, const GuestStreamRec& record, const char* function) {
    if (!CopyOutPreservingErrno(stream, &record, sizeof(record))) Unsupported(function, "unwritable stream");
}

std::uintptr_t Address(const void* pointer) {
    return reinterpret_cast<std::uintptr_t>(pointer);
}

std::uint8_t* Offset(const void* pointer, std::uint64_t count) {
    return reinterpret_cast<std::uint8_t*>(Address(pointer) + count);
}

std::uint64_t BigEndian(const std::uint8_t* bytes, unsigned count) {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < count; ++index) value = value << 8 | bytes[index];
    return value;
}

std::uint64_t LittleEndian(const std::uint8_t* bytes, unsigned count) {
    std::uint64_t value = 0;
    for (unsigned index = count; index > 0; --index) value = value << 8 | bytes[index - 1];
    return value;
}

std::int64_t SignExtend(std::uint64_t value, unsigned bytes) {
    const unsigned shift = 64 - bytes * 8;
    return static_cast<std::int64_t>(value << shift) >> shift;
}

void CopyFromBase(std::uint8_t* buffer, const GuestStreamRec& stream, std::uint64_t pos, std::uint64_t count, const char* function) {
    if (count == 0) return;
    if (!buffer) Unsupported(function, "null buffer");
    if (!stream.base) Unsupported(function, "null memory stream base");
    const auto baseAddress = Address(stream.base);
    if (pos > std::numeric_limits<std::uintptr_t>::max() - baseAddress ||
        count > std::numeric_limits<std::uintptr_t>::max() - (baseAddress + static_cast<std::uintptr_t>(pos))) {
        Unsupported(function, "invalid memory stream range");
    }
    const auto* source = stream.base + pos;
    if (!WritablePreservingErrno(buffer, static_cast<std::size_t>(count))) Unsupported(function, "unwritable buffer");
    std::array<std::uint8_t, 4096> chunk{};
    const auto srcAddress = Address(source);
    const auto dstAddress = Address(buffer);
    const auto total = static_cast<std::size_t>(count);
    if (dstAddress > srcAddress && dstAddress < srcAddress + total) {
        std::size_t remaining = total;
        while (remaining > 0) {
            const std::size_t step = std::min(remaining, chunk.size());
            remaining -= step;
            if (!CopyInPreservingErrno(chunk.data(), source + remaining, step)) Unsupported(function, "unreadable memory stream base");
            if (!CopyOutPreservingErrno(buffer + remaining, chunk.data(), step)) Unsupported(function, "unwritable buffer");
        }
        return;
    }
    for (std::size_t offset = 0; offset < total;) {
        const std::size_t step = std::min(total - offset, chunk.size());
        if (!CopyInPreservingErrno(chunk.data(), source + offset, step)) Unsupported(function, "unreadable memory stream base");
        if (!CopyOutPreservingErrno(buffer + offset, chunk.data(), step)) Unsupported(function, "unwritable buffer");
        offset += step;
    }
}

std::uint8_t* QAlloc(GuestMemoryRec* memory, std::int64_t size, int& error, const char* function) {
    error = 0;
    if (size > 0) {
        GuestMemoryRec hostMemory{};
        if (!memory || !CopyInPreservingErrno(&hostMemory, memory, sizeof(hostMemory)) || !hostMemory.alloc || !hostMemory.free) {
            Unsupported(function, "null memory allocator or release");
        }
        auto* block = static_cast<std::uint8_t*>(hostMemory.alloc(memory, size));
        if (!block) error = OutOfMemory;
        return block;
    }
    if (size < 0) error = InvalidArgument;
    return nullptr;
}

void Free(GuestMemoryRec* memory, void* block, const char* function) {
    if (!block) return;
    GuestMemoryRec hostMemory{};
    if (!memory || !CopyInPreservingErrno(&hostMemory, memory, sizeof(hostMemory)) || !hostMemory.free) {
        Unsupported(function, "null memory release");
    }
    hostMemory.free(memory, block);
}

int Seek(GuestStreamRec* stream, std::uint64_t pos, const char* function) {
    auto record = ReadWritableStream(stream, function);
    int error = 0;
    if (record.read) {
        if (record.read(stream, pos, nullptr, 0)) error = InvalidStreamOperation;
    } else if (pos > record.size) {
        error = InvalidStreamOperation;
    }
    if (!error) {
        record = ReadWritableStream(stream, function);
        record.pos = pos;
        WriteStream(stream, record, function);
    }
    return error;
}

int ReadAt(GuestStreamRec* stream, std::uint64_t pos, std::uint8_t* buffer, std::uint64_t count, const char* function) {
    auto record = ReadWritableStream(stream, function);
    if (pos >= record.size) return InvalidStreamOperation;
    const std::uint64_t available = record.size - pos;
    const std::uint64_t maxRead = available > count ? count : available;
    if (count > 0) {
        if (!buffer) Unsupported(function, "null buffer");
        if (!WritablePreservingErrno(buffer, static_cast<std::size_t>(maxRead))) Unsupported(function, "unwritable buffer");
    }
    std::uint64_t readBytes;
    if (record.read) {
        readBytes = record.read(stream, pos, buffer, count);
        record = ReadWritableStream(stream, function);
    } else {
        readBytes = maxRead;
        CopyFromBase(buffer, record, pos, readBytes, function);
    }
    record.pos = pos + readBytes;
    WriteStream(stream, record, function);
    return readBytes < count ? InvalidStreamOperation : 0;
}

int EnterFrame(GuestStreamRec* stream, std::uint64_t count, const char* function) {
    auto record = ReadWritableStream(stream, function);
    int error = 0;
    if (record.read) {
        auto* memory = record.memory;
        if (count > record.size) return InvalidStreamOperation;
        auto* allocated = QAlloc(memory, static_cast<std::int64_t>(count), error, function);
        record = ReadWritableStream(stream, function);
        record.base = allocated;
        WriteStream(stream, record, function);
        if (error) return error;
        const std::uint64_t readBytes = record.read(stream, record.pos, allocated, count);
        record = ReadWritableStream(stream, function);
        if (readBytes < count) {
            Free(memory, allocated, function);
            record.base = nullptr;
            record.cursor = nullptr;
            record.limit = nullptr;
            record.pos += readBytes;
            WriteStream(stream, record, function);
            return InvalidStreamOperation;
        }
        record.base = allocated;
        record.cursor = allocated;
        record.limit = Offset(allocated, count);
        record.pos += readBytes;
        WriteStream(stream, record, function);
    } else {
        if (record.pos >= record.size || record.size - record.pos < count) return InvalidStreamOperation;
        if (!record.base && record.pos != 0) Unsupported(function, "null memory stream base");
        record.cursor = Offset(record.base, record.pos);
        record.limit = Offset(record.cursor, count);
        record.pos += count;
        WriteStream(stream, record, function);
    }
    return error;
}

void ExitFrame(GuestStreamRec* stream, const char* function) {
    auto record = ReadWritableStream(stream, function);
    if (record.read) {
        Free(record.memory, record.base, function);
        record.base = nullptr;
    }
    record.cursor = nullptr;
    record.limit = nullptr;
    WriteStream(stream, record, function);
}

template <unsigned Bytes, bool Little>
std::uint64_t Get(GuestStreamRec* stream, const char* function) {
    auto record = ReadWritableStream(stream, function);
    auto* cursor = record.cursor;
    std::uint64_t value = 0;
    if (Address(cursor) + (Bytes - 1) < Address(record.limit)) {
        if (!cursor) Unsupported(function, "null frame cursor");
        std::uint8_t bytes[Bytes]{};
        if (!CopyInPreservingErrno(bytes, cursor, Bytes)) Unsupported(function, "unreadable frame cursor");
        value = Little ? LittleEndian(bytes, Bytes) : BigEndian(bytes, Bytes);
        cursor += Bytes;
        record.cursor = cursor;
        WriteStream(stream, record, function);
    }
    return value;
}

template <unsigned Bytes, bool Little>
std::uint64_t Read(GuestStreamRec* stream, int* error, const char* function) {
    auto record = ReadWritableStream(stream, function);
    if (!error) Unsupported(function, "null error");
    if (!WritablePreservingErrno(error, sizeof(*error))) Unsupported(function, "unwritable error");
    int errorCode = 0;
    if (record.pos + (Bytes - 1) >= record.size) {
        errorCode = InvalidStreamOperation;
        if (!CopyOutPreservingErrno(error, &errorCode, sizeof(errorCode))) Unsupported(function, "unwritable error");
        return 0;
    }
    std::uint8_t reads[Bytes]{};
    bool haveBytes = false;
    if (record.read) {
        if (record.read(stream, record.pos, reads, Bytes) != Bytes) {
            errorCode = InvalidStreamOperation;
            if (!CopyOutPreservingErrno(error, &errorCode, sizeof(errorCode))) Unsupported(function, "unwritable error");
            return 0;
        }
        record = ReadWritableStream(stream, function);
        haveBytes = true;
    } else {
        const auto* bytes = Offset(record.base, record.pos);
        if (bytes && !record.base) Unsupported(function, "null memory stream base");
        if (bytes) {
            if (!CopyInPreservingErrno(reads, bytes, Bytes)) Unsupported(function, "unreadable memory stream base");
            haveBytes = true;
        }
    }
    const std::uint64_t value = haveBytes ? (Little ? LittleEndian(reads, Bytes) : BigEndian(reads, Bytes)) : 0;
    record.pos += Bytes;
    WriteStream(stream, record, function);
    if (!CopyOutPreservingErrno(error, &errorCode, sizeof(errorCode))) Unsupported(function, "unwritable error");
    return value;
}

template <class T>
void Store(std::uint8_t* target, std::uint64_t value) {
    const auto stored = static_cast<T>(value);
    std::memcpy(target, &stored, sizeof(stored));
}

}

extern "C" {

void APS5_VABI FT_Stream_OpenMemory_nid_postfix(GuestStreamRec* stream, const std::uint8_t* base, std::uint64_t size) {
    auto record = ReadWritableStream(stream, __func__);
    record.base = const_cast<std::uint8_t*>(base);
    record.size = size;
    record.pos = 0;
    record.cursor = nullptr;
    record.read = nullptr;
    record.close = nullptr;
    WriteStream(stream, record, __func__);
}

void APS5_VABI FT_Stream_Close_nid_postfix(GuestStreamRec* stream) {
    if (!stream) return;
    const auto record = ReadStream(stream, __func__);
    if (record.close) record.close(stream);
}

int APS5_VABI FT_Stream_Seek_nid_postfix(GuestStreamRec* stream, std::uint64_t pos) {
    return Seek(stream, pos, __func__);
}

int APS5_VABI FT_Stream_Skip_nid_postfix(GuestStreamRec* stream, std::int64_t distance) {
    if (distance < 0) return InvalidStreamOperation;
    const auto record = ReadStream(stream, __func__);
    return Seek(stream, record.pos + static_cast<std::uint64_t>(distance), __func__);
}

std::int64_t APS5_VABI FT_Stream_Pos_nid_postfix(GuestStreamRec* stream) {
    return static_cast<std::int64_t>(ReadStream(stream, __func__).pos);
}

int APS5_VABI FT_Stream_Read_nid_postfix(GuestStreamRec* stream, std::uint8_t* buffer, std::uint64_t count) {
    return ReadAt(stream, ReadStream(stream, __func__).pos, buffer, count, __func__);
}

int APS5_VABI FT_Stream_ReadAt_nid_postfix(GuestStreamRec* stream, std::uint64_t pos, std::uint8_t* buffer, std::uint64_t count) {
    return ReadAt(stream, pos, buffer, count, __func__);
}

std::uint64_t APS5_VABI FT_Stream_TryRead_nid_postfix(GuestStreamRec* stream, std::uint8_t* buffer, std::uint64_t count) {
    auto record = ReadWritableStream(stream, __func__);
    if (record.pos >= record.size) return 0;
    const std::uint64_t available = record.size - record.pos;
    const std::uint64_t request = available > count ? count : available;
    if (request > 0) {
        if (!buffer) Unsupported(__func__, "null buffer");
        if (!WritablePreservingErrno(buffer, static_cast<std::size_t>(request))) Unsupported(__func__, "unwritable buffer");
    }
    std::uint64_t readBytes;
    if (record.read) {
        readBytes = record.read(stream, record.pos, buffer, count);
        record = ReadWritableStream(stream, __func__);
    } else {
        readBytes = request;
        CopyFromBase(buffer, record, record.pos, readBytes, __func__);
    }
    record.pos += readBytes;
    WriteStream(stream, record, __func__);
    return readBytes;
}

int APS5_VABI FT_Stream_EnterFrame_nid_postfix(GuestStreamRec* stream, std::uint64_t count) {
    return EnterFrame(stream, count, __func__);
}

void APS5_VABI FT_Stream_ExitFrame_nid_postfix(GuestStreamRec* stream) {
    ExitFrame(stream, __func__);
}

int APS5_VABI FT_Stream_ExtractFrame_nid_postfix(GuestStreamRec* stream, std::uint64_t count, std::uint8_t** bytes) {
    if (!bytes) Unsupported(__func__, "null frame pointer");
    if (!WritablePreservingErrno(bytes, sizeof(*bytes))) Unsupported(__func__, "unwritable frame pointer");
    const int error = EnterFrame(stream, count, __func__);
    if (!error) {
        auto record = ReadWritableStream(stream, __func__);
        std::uint8_t* frame = record.cursor;
        record.cursor = nullptr;
        record.limit = nullptr;
        WriteStream(stream, record, __func__);
        if (!CopyOutPreservingErrno(bytes, &frame, sizeof(frame))) Unsupported(__func__, "unwritable frame pointer");
    }
    return error;
}

void APS5_VABI FT_Stream_ReleaseFrame_nid_postfix(GuestStreamRec* stream, std::uint8_t** bytes) {
    if (!bytes) Unsupported(__func__, "null frame pointer");
    std::uint8_t* frame = nullptr;
    if (!CopyInPreservingErrno(&frame, bytes, sizeof(frame))) Unsupported(__func__, "unreadable frame pointer");
    if (!WritablePreservingErrno(bytes, sizeof(*bytes))) Unsupported(__func__, "unwritable frame pointer");
    if (stream) {
        const auto record = ReadStream(stream, __func__);
        if (record.read) Free(record.memory, frame, __func__);
    }
    std::uint8_t* cleared = nullptr;
    if (!CopyOutPreservingErrno(bytes, &cleared, sizeof(cleared))) Unsupported(__func__, "unwritable frame pointer");
}

std::int8_t APS5_VABI FT_Stream_GetChar_nid_postfix(GuestStreamRec* stream) {
    auto record = ReadWritableStream(stream, __func__);
    std::int8_t value = 0;
    if (Address(record.cursor) < Address(record.limit)) {
        if (!record.cursor) Unsupported(__func__, "null frame cursor");
        std::uint8_t byte = 0;
        if (!CopyInPreservingErrno(&byte, record.cursor, sizeof(byte))) Unsupported(__func__, "unreadable frame cursor");
        value = static_cast<std::int8_t>(byte);
        record.cursor += 1;
        WriteStream(stream, record, __func__);
    }
    return value;
}

std::int16_t APS5_VABI FT_Stream_GetShort_nid_postfix(GuestStreamRec* stream) {
    return static_cast<std::int16_t>(Get<2, false>(stream, __func__));
}

std::int16_t APS5_VABI FT_Stream_GetShortLE_nid_postfix(GuestStreamRec* stream) {
    return static_cast<std::int16_t>(Get<2, true>(stream, __func__));
}

std::uint16_t APS5_VABI FT_Stream_GetUShort_nid_postfix(GuestStreamRec* stream) {
    return static_cast<std::uint16_t>(Get<2, false>(stream, __func__));
}

std::uint16_t APS5_VABI FT_Stream_GetUShortLE_nid_postfix(GuestStreamRec* stream) {
    return static_cast<std::uint16_t>(Get<2, true>(stream, __func__));
}

std::int64_t APS5_VABI FT_Stream_GetOffset_nid_postfix(GuestStreamRec* stream) {
    return SignExtend(Get<3, false>(stream, __func__), 3);
}

std::uint64_t APS5_VABI FT_Stream_GetUOffset_nid_postfix(GuestStreamRec* stream) {
    return Get<3, false>(stream, __func__);
}

std::int64_t APS5_VABI FT_Stream_GetLong_nid_postfix(GuestStreamRec* stream) {
    return SignExtend(Get<4, false>(stream, __func__), 4);
}

std::int64_t APS5_VABI FT_Stream_GetLongLE_nid_postfix(GuestStreamRec* stream) {
    return SignExtend(Get<4, true>(stream, __func__), 4);
}

std::uint64_t APS5_VABI FT_Stream_GetULong_nid_postfix(GuestStreamRec* stream) {
    return Get<4, false>(stream, __func__);
}

std::uint64_t APS5_VABI FT_Stream_GetULongLE_nid_postfix(GuestStreamRec* stream) {
    return Get<4, true>(stream, __func__);
}

std::int8_t APS5_VABI FT_Stream_ReadChar_nid_postfix(GuestStreamRec* stream, int* error) {
    auto record = ReadWritableStream(stream, __func__);
    if (!error) Unsupported(__func__, "null error");
    if (!WritablePreservingErrno(error, sizeof(*error))) Unsupported(__func__, "unwritable error");
    int errorCode = 0;
    std::uint8_t value = 0;
    if (record.read) {
        if (record.read(stream, record.pos, &value, 1) != 1) {
            errorCode = InvalidStreamOperation;
            if (!CopyOutPreservingErrno(error, &errorCode, sizeof(errorCode))) Unsupported(__func__, "unwritable error");
            return 0;
        }
        record = ReadWritableStream(stream, __func__);
    } else {
        if (record.pos >= record.size) {
            errorCode = InvalidStreamOperation;
            if (!CopyOutPreservingErrno(error, &errorCode, sizeof(errorCode))) Unsupported(__func__, "unwritable error");
            return 0;
        }
        if (!record.base) Unsupported(__func__, "null memory stream base");
        if (!CopyInPreservingErrno(&value, Offset(record.base, record.pos), 1)) Unsupported(__func__, "unreadable memory stream base");
    }
    record.pos += 1;
    WriteStream(stream, record, __func__);
    if (!CopyOutPreservingErrno(error, &errorCode, sizeof(errorCode))) Unsupported(__func__, "unwritable error");
    return static_cast<std::int8_t>(value);
}

std::int16_t APS5_VABI FT_Stream_ReadShort_nid_postfix(GuestStreamRec* stream, int* error) {
    return static_cast<std::int16_t>(Read<2, false>(stream, error, __func__));
}

std::int16_t APS5_VABI FT_Stream_ReadShortLE_nid_postfix(GuestStreamRec* stream, int* error) {
    return static_cast<std::int16_t>(Read<2, true>(stream, error, __func__));
}

std::uint16_t APS5_VABI FT_Stream_ReadUShort_nid_postfix(GuestStreamRec* stream, int* error) {
    return static_cast<std::uint16_t>(Read<2, false>(stream, error, __func__));
}

std::uint16_t APS5_VABI FT_Stream_ReadUShortLE_nid_postfix(GuestStreamRec* stream, int* error) {
    return static_cast<std::uint16_t>(Read<2, true>(stream, error, __func__));
}

std::int64_t APS5_VABI FT_Stream_ReadOffset_nid_postfix(GuestStreamRec* stream, int* error) {
    return SignExtend(Read<3, false>(stream, error, __func__), 3);
}

std::uint64_t APS5_VABI FT_Stream_ReadUOffset_nid_postfix(GuestStreamRec* stream, int* error) {
    return Read<3, false>(stream, error, __func__);
}

std::int64_t APS5_VABI FT_Stream_ReadLong_nid_postfix(GuestStreamRec* stream, int* error) {
    return SignExtend(Read<4, false>(stream, error, __func__), 4);
}

std::int64_t APS5_VABI FT_Stream_ReadLongLE_nid_postfix(GuestStreamRec* stream, int* error) {
    return SignExtend(Read<4, true>(stream, error, __func__), 4);
}

std::uint64_t APS5_VABI FT_Stream_ReadULong_nid_postfix(GuestStreamRec* stream, int* error) {
    return Read<4, false>(stream, error, __func__);
}

std::uint64_t APS5_VABI FT_Stream_ReadULongLE_nid_postfix(GuestStreamRec* stream, int* error) {
    return Read<4, true>(stream, error, __func__);
}

int APS5_VABI FT_Stream_ReadFields_nid_postfix(GuestStreamRec* stream, const GuestFrameField* fields, void* structure) {
    const char* function = __func__;
    if (!fields || !stream) return InvalidArgument;
    auto record = ReadWritableStream(stream, function);
    int error = 0;
    bool frameAccessed = false;
    auto* cursor = record.cursor;
    const auto finish = [&] {
        if (frameAccessed) ExitFrame(stream, function);
        return error;
    };
    const auto refuse = [&](const char* what) {
        if (frameAccessed) ExitFrame(stream, function);
        Unsupported(function, what);
    };
    for (;; ++fields) {
        GuestFrameField field{};
        if (!CopyInPreservingErrno(&field, fields, sizeof(field))) refuse("unreadable field");
        if (field.value == FrameStart) {
            error = EnterFrame(stream, field.offset, function);
            if (error) return finish();
            frameAccessed = true;
            record = ReadWritableStream(stream, function);
            cursor = record.cursor;
            continue;
        }
        if (field.value == FrameBytes || field.value == FrameSkip) {
            const std::uint64_t length = field.size;
            if (Address(cursor) + length > Address(record.limit)) {
                error = InvalidStreamOperation;
                return finish();
            }
            if (field.value == FrameBytes && length != 0) {
                if (!structure) refuse("null structure");
                if (!cursor) refuse("null frame cursor");
                std::array<std::uint8_t, 256> raw{};
                if (!CopyInPreservingErrno(raw.data(), cursor, static_cast<std::size_t>(length))) refuse("unreadable frame cursor");
                if (!CopyOutPreservingErrno(static_cast<std::uint8_t*>(structure) + field.offset, raw.data(), static_cast<std::size_t>(length))) {
                    refuse("unwritable structure");
                }
            }
            cursor = Offset(cursor, length);
            continue;
        }
        unsigned bytes = 0;
        bool little = false;
        switch (field.value) {
        case 8: case 9: bytes = 1; break;
        case 12: case 13: bytes = 2; break;
        case 14: case 15: bytes = 2; little = true; break;
        case 16: case 17: bytes = 4; break;
        case 18: case 19: bytes = 4; little = true; break;
        case 20: case 21: bytes = 3; break;
        case 22: case 23: bytes = 3; little = true; break;
        default:
            if (!frameAccessed) {
                record.cursor = cursor;
                WriteStream(stream, record, function);
            }
            return finish();
        }
        if (!cursor) refuse("null frame cursor");
        std::uint8_t raw[4]{};
        if (!CopyInPreservingErrno(raw, cursor, bytes)) refuse("unreadable frame cursor");
        std::uint64_t value = little ? LittleEndian(raw, bytes) : BigEndian(raw, bytes);
        cursor += bytes;
        if (field.value & FrameSigned) {
            const unsigned shift = 32 - bytes * 8;
            value = static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(value << shift)) >> shift));
        }
        if (!structure) refuse("null structure");
        auto* target = static_cast<std::uint8_t*>(structure) + field.offset;
        std::uint8_t encoded[8]{};
        std::size_t storeSize = 8;
        switch (field.size) {
        case 1: Store<std::uint8_t>(encoded, value); storeSize = 1; break;
        case 2: Store<std::uint16_t>(encoded, value); storeSize = 2; break;
        case 4: Store<std::uint32_t>(encoded, value); storeSize = 4; break;
        default: Store<std::uint64_t>(encoded, value); break;
        }
        if (!CopyOutPreservingErrno(target, encoded, storeSize)) refuse("unwritable structure");
    }
}

void APS5_VABI FT_Stream_Free_nid_postfix(GuestStreamRec* stream, std::int32_t external) {
    if (!stream) return;
    const auto record = ReadStream(stream, __func__);
    auto* memory = record.memory;
    if (!external) {
        GuestMemoryRec hostMemory{};
        if (!memory || !CopyInPreservingErrno(&hostMemory, memory, sizeof(hostMemory)) || !hostMemory.free) {
            Unsupported(__func__, "null memory release");
        }
    }
    FT_Stream_Close_nid_postfix(stream);
    if (!external) Free(memory, stream, __func__);
}

}
