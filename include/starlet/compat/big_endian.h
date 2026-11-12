#ifndef STARLET_COMPAT_BIG_ENDIAN_H
#define STARLET_COMPAT_BIG_ENDIAN_H

/* Game resource files stay big-endian in memory, as on the console: the GX
 * reads vertex arrays, textures and display lists straight from them. Structs
 * that overlay file data declare their multi-byte fields with BE<T>, which has
 * T's size and alignment and converts on every read and write. */

#ifdef __cplusplus

#include <stdint.h>
#include <string.h>
#include <type_traits>

namespace starlet_compat {

template <unsigned Size> struct BeWord;
template <> struct BeWord<1> { typedef uint8_t type; static constexpr type swap(type v) { return v; } };
template <> struct BeWord<2> { typedef uint16_t type; static constexpr type swap(type v) { return __builtin_bswap16(v); } };
template <> struct BeWord<4> { typedef uint32_t type; static constexpr type swap(type v) { return __builtin_bswap32(v); } };
template <> struct BeWord<8> { typedef uint64_t type; static constexpr type swap(type v) { return __builtin_bswap64(v); } };

/* Reverses T's bytes; its own inverse. */
template <typename T> constexpr T swap_bytes(T value)
{
    typedef BeWord<sizeof(T)> W;
    return __builtin_bit_cast(T, W::swap(__builtin_bit_cast(typename W::type, value)));
}

template <typename T> constexpr T from_big_endian(T stored) { return swap_bytes(stored); }
template <typename T> constexpr T to_big_endian(T value) { return swap_bytes(value); }

} // namespace starlet_compat

template <typename T>
struct BE {
    static_assert(std::is_arithmetic<T>::value || std::is_enum<T>::value, "BE<T> holds numbers");
    T raw;

    operator T() const { return starlet_compat::from_big_endian(raw); }
    T value() const { return starlet_compat::from_big_endian(raw); }
    BE& operator=(T v)
    {
        raw = starlet_compat::to_big_endian(v);
        return *this;
    }
    BE& operator+=(T v) { return *this = T(value() + v); }
    BE& operator-=(T v) { return *this = T(value() - v); }
    BE& operator*=(T v) { return *this = T(value() * v); }
    BE& operator/=(T v) { return *this = T(value() / v); }
    BE& operator|=(T v) { return *this = T(value() | v); }
    BE& operator&=(T v) { return *this = T(value() & v); }
    BE& operator^=(T v) { return *this = T(value() ^ v); }
    BE& operator<<=(int v) { return *this = T(value() << v); }
    BE& operator>>=(int v) { return *this = T(value() >> v); }
    BE& operator++() { return *this = T(value() + 1); }
    BE& operator--() { return *this = T(value() - 1); }
    T operator++(int)
    {
        T old = value();
        *this = T(old + 1);
        return old;
    }
    T operator--(int)
    {
        T old = value();
        *this = T(old - 1);
        return old;
    }
};

/* An element of a constant data table holding file-format words (the system
 * font, default envelopes): stored big-endian at compile time and read like
 * file data through BE<T>. */
template <typename T>
struct BETable : BE<T> {
    constexpr BETable(T value) : BE<T>{starlet_compat::to_big_endian(value)} { }
};
static_assert(sizeof(BETable<int>) == sizeof(int), "table elements keep the word size");

typedef BE<uint16_t> BEu16;
typedef BE<int16_t> BEs16;
typedef BE<uint32_t> BEu32;
typedef BE<int32_t> BEs32;
typedef BE<float> BEf32;

/* A pointer the game keeps inside a 32-bit slot of file data (a RARC file
 * entry's cached data pointer, for example). Game memory lies below 4 GiB
 * (the port maps it there), so the native 32-bit value holds the pointer. */
template <typename T>
struct Ptr32 {
    uint32_t raw;

    operator T*() const { return reinterpret_cast<T*>(static_cast<uintptr_t>(raw)); }
    T* get() const { return *this; }
    T* operator->() const { return *this; }
    Ptr32& operator=(T* pointer)
    {
        raw = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pointer));
        return *this;
    }
};

/* A big-endian word read from file data at an arbitrary (possibly unaligned) address. */
template <typename T> inline T read_be(const void* address)
{
    T stored;
    memcpy(&stored, address, sizeof stored);
    return starlet_compat::from_big_endian(stored);
}

template <typename T> inline void write_be(void* address, T value)
{
    T stored = starlet_compat::to_big_endian(value);
    memcpy(address, &stored, sizeof stored);
}

#endif

#endif
