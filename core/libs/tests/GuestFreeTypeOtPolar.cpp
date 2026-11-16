#include "prx/libSceFreeTypeOt/include/Polar.hpp"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

using FreeType::GuestVector;

constexpr std::int64_t Degree = 1 << 16;

void Require(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FreeType Ot polar: %s\n", message);
    std::exit(1);
}

template <typename TCall> bool Throws(TCall call) {
    try {
        call();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

std::int64_t Length(std::int64_t x, std::int64_t y) {
    GuestVector vector{x, y};
    return sceFreeTypeOtVectorLength_nid_no_patch(&vector);
}

bool Polar(std::int64_t x, std::int64_t y, std::int64_t length, std::int64_t angle) {
    GuestVector vector{x, y};
    std::int64_t gotLength = -1;
    std::int64_t gotAngle = -1;
    sceFreeTypeOtVectorPolarize_nid_no_patch(&vector, &gotLength, &gotAngle);
    return gotLength == length && gotAngle == angle && vector.x == x && vector.y == y;
}

bool Rotates(GuestVector vector, std::int64_t angle, GuestVector expected) {
    sceFreeTypeOtVectorRotate_nid_no_patch(&vector, angle);
    return vector.x == expected.x && vector.y == expected.y;
}

void Lengths() {
    Require(Length(3 * Degree, 4 * Degree) == 5 * Degree, "a 3-4-5 vector has length 5");
    Require(Length(-300, 400) == 500 && Length(-5, -12) == 13 && Length(1, 1) == 1, "small vectors round to the CORDIC length");
    Require(Length(0x12345, -0x6789) == 79136 && Length(100000000, 3) == 100000002, "the 2.4.11 CORDIC lengths are kept");
    Require(Length(0, -7) == 7 && Length(-9, 0) == 9 && Length(0, 0) == 0, "an axis vector returns the absolute component");
    Require(Length(0, std::numeric_limits<std::int64_t>::max()) == std::numeric_limits<std::int64_t>::max(), "the axis shortcut takes any magnitude");
    Require(Throws([] { sceFreeTypeOtVectorLength_nid_no_patch(nullptr); }), "a null vector throws");
}

void Polarize() {
    Require(Polar(3 * Degree, 4 * Degree, 5 * Degree, 3481920), "a 3-4-5 vector has its CORDIC angle");
    Require(Polar(-64, 0, 64, 180 * Degree) && Polar(0, -64, 64, -90 * Degree), "axis vectors have exact angles");
    Require(Polar(-100, 100, 141, 135 * Degree) && Polar(1000, -1, 1000, -3744), "the length is truncated and the angle padded to 32");
    GuestVector zero{0, 0};
    std::int64_t length = 11;
    std::int64_t angle = 22;
    sceFreeTypeOtVectorPolarize_nid_no_patch(&zero, &length, &angle);
    Require(length == 11 && angle == 22, "a zero vector leaves both outputs");
    sceFreeTypeOtVectorPolarize_nid_no_patch(&zero, nullptr, nullptr);
    GuestVector vector{3, 4};
    Require(Throws([&] { sceFreeTypeOtVectorPolarize_nid_no_patch(&vector, &length, nullptr); }) && length == 11, "a null angle throws before writing");
    Require(Throws([&] { sceFreeTypeOtVectorPolarize_nid_no_patch(nullptr, &length, &angle); }), "a null vector throws");
}

void Rotate() {
    Require(Rotates({Degree, 0}, 90 * Degree, {0, Degree}), "a quarter turn is exact");
    Require(Rotates({Degree, 0}, 30 * Degree, {56756, 32768}), "a 30 degree turn uses the CORDIC");
    Require(Rotates({100, 200}, -45 * Degree, {212, 71}) && Rotates({-640, 64}, 180 * Degree, {640, -64}), "turns round each component");
    Require(Rotates({64, 64}, -270 * Degree, {-64, 64}), "negative turns past a half turn wrap");
    Require(Rotates({Degree, 0}, 30 * Degree + 360 * Degree * 1000000000LL, {56756, 32768}), "whole turns are reduced without looping");
    Require(Rotates({5 * Degree, 0}, 1, {5 * Degree, 0}), "a tiny turn keeps the vector");
    Require(Rotates({7, -9}, 0, {7, -9}) && Rotates({0, 0}, 45 * Degree, {0, 0}), "a zero angle or vector is unchanged");
    Require(Rotates({0x7FFFFFFFFFFFLL, 3}, 0, {0x7FFFFFFFFFFFLL, 3}), "a zero angle takes any magnitude");
    Require(Throws([] { sceFreeTypeOtVectorRotate_nid_no_patch(nullptr, Degree); }), "a null vector throws");

    GuestVector vector{-1, -1};
    sceFreeTypeOtVectorFromPolar_nid_no_patch(&vector, Degree, 30 * Degree);
    Require(vector.x == 56756 && vector.y == 32768, "FT_Vector_From_Polar rotates the length");
    sceFreeTypeOtVectorFromPolar_nid_no_patch(&vector, -5, 0);
    Require(vector.x == -5 && vector.y == 0, "FT_Vector_From_Polar with no angle stores the length");
    Require(Throws([] { sceFreeTypeOtVectorFromPolar_nid_no_patch(nullptr, Degree, 0); }), "FT_Vector_From_Polar throws for a null vector");
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
        Require(base != nullptr, "guarded page allocates");
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
        DWORD previous = 0;
        Require(VirtualProtect(base, size, PAGE_READONLY, &previous) != 0, "guarded page becomes read-only");
#else
        Require(mprotect(base, size, PROT_READ) == 0, "guarded page becomes read-only");
#endif
    }
};

template <typename TCall>
bool ThrowsPreservingErrno(TCall call) {
    errno = 71;
    try {
        call();
    } catch (const std::runtime_error&) {
        return errno == 71;
    }
    return false;
}

void GuestPointers() {
    auto* badVector = reinterpret_cast<GuestVector*>(std::uintptr_t{0x10});
    auto* badValue = reinterpret_cast<std::int64_t*>(std::uintptr_t{0x10});
    GuestVector vector{3 * Degree, 4 * Degree};
    std::int64_t length = 11;
    std::int64_t angle = 22;

    Require(ThrowsPreservingErrno([&] { sceFreeTypeOtVectorLength_nid_no_patch(badVector); }),
            "length rejects an unmapped vector without changing errno");
    Require(ThrowsPreservingErrno([&] { sceFreeTypeOtVectorPolarize_nid_no_patch(badVector, &length, &angle); }) &&
                length == 11 && angle == 22,
            "polarize rejects an unmapped vector without modifying outputs or errno");
    Require(ThrowsPreservingErrno([&] { sceFreeTypeOtVectorPolarize_nid_no_patch(&vector, badValue, &angle); }) &&
                angle == 22,
            "polarize rejects an unmapped length without modifying angle or errno");
    Require(ThrowsPreservingErrno([&] { sceFreeTypeOtVectorPolarize_nid_no_patch(&vector, &length, badValue); }) &&
                length == 11,
            "polarize rejects an unmapped angle before writing length or modifying errno");
    Require(ThrowsPreservingErrno([&] { sceFreeTypeOtVectorFromPolar_nid_no_patch(badVector, Degree, 0); }),
            "fromPolar rejects an unmapped vector without changing errno");
    Require(ThrowsPreservingErrno([&] { sceFreeTypeOtVectorRotate_nid_no_patch(badVector, Degree); }),
            "rotate rejects an unmapped vector without changing errno");

    GuardedPage vectorPage;
    *vectorPage.As<GuestVector>() = {3 * Degree, 4 * Degree};
    vectorPage.SetReadOnly();
    auto* roVector = vectorPage.As<GuestVector>();
    errno = 71;
    Require(sceFreeTypeOtVectorLength_nid_no_patch(roVector) == 5 * Degree && errno == 71,
            "length reads a read-only vector and preserves errno");
    Require(ThrowsPreservingErrno([&] { sceFreeTypeOtVectorFromPolar_nid_no_patch(roVector, Degree, 30 * Degree); }) &&
                roVector->x == 3 * Degree && roVector->y == 4 * Degree,
            "fromPolar rejects a read-only vector without changing errno");
    Require(ThrowsPreservingErrno([&] { sceFreeTypeOtVectorRotate_nid_no_patch(roVector, 30 * Degree); }) &&
                roVector->x == 3 * Degree && roVector->y == 4 * Degree,
            "rotate rejects a read-only vector without changing errno");

    GuardedPage scalarPage;
    *scalarPage.As<std::int64_t>() = 77;
    scalarPage.SetReadOnly();
    auto* roScalar = scalarPage.As<std::int64_t>();
    Require(ThrowsPreservingErrno([&] { sceFreeTypeOtVectorPolarize_nid_no_patch(&vector, roScalar, &angle); }) &&
                *roScalar == 77 && angle == 22,
            "polarize rejects a read-only length without modifying outputs or errno");
    Require(ThrowsPreservingErrno([&] { sceFreeTypeOtVectorPolarize_nid_no_patch(&vector, &length, roScalar); }) &&
                length == 11 && *roScalar == 77,
            "polarize rejects a read-only angle before writing length or modifying errno");

    errno = 71;
    sceFreeTypeOtVectorPolarize_nid_no_patch(&vector, &length, &angle);
    Require(length == 5 * Degree && angle == 3481920 && errno == 71, "polarize preserves errno on success");
    errno = 71;
    sceFreeTypeOtVectorRotate_nid_no_patch(&vector, 90 * Degree);
    Require(vector.x == -4 * Degree && vector.y == 3 * Degree && errno == 71, "rotate preserves errno on success");
    errno = 71;
    sceFreeTypeOtVectorFromPolar_nid_no_patch(&vector, Degree, 0);
    Require(vector.x == Degree && vector.y == 0 && errno == 71, "fromPolar preserves errno on success");
}

}

int main() {
    Lengths();
    Polarize();
    Rotate();
    GuestPointers();
    std::puts("Guest FreeType Ot polar tests passed");
    return 0;
}
