#ifndef STARLET_COMPAT_GX_FIFO_H
#define STARLET_COMPAT_GX_FIFO_H

/* The write-gather pipe at 0xCC008000. Writes append big-endian command bytes
 * to the current CPU FIFO target: a staging chunk the GPU thread decodes, or a
 * display list being recorded. */

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct StarletGxStage {
    uint8_t* cur;
    uint8_t* end;
} StarletGxStage;

extern StarletGxStage starlet_gx_stage;

typedef struct StarletGxStateCache {
    uint32_t words[264], generations[264], last_tev[8], generation;
    uint32_t xf_words[88], xf_generations[88];
    uint32_t cp_layout[10], cp_known, cp_words[256], cp_generations[256];
    uint32_t matrix_words[1536], matrix_generations[1536];
    int full_mask;
} StarletGxStateCache;
extern StarletGxStateCache starlet_gx_state_cache;

/* Lists and FIFO switches can change registers without going through SDK setters. */
static inline void starlet_gx_state_forget(StarletGxStateCache* cache)
{
    if (++cache->generation == 0) {
        memset(cache->generations, 0, sizeof cache->generations);
        memset(cache->xf_generations, 0, sizeof cache->xf_generations);
        memset(cache->cp_generations, 0, sizeof cache->cp_generations);
        memset(cache->matrix_generations, 0, sizeof cache->matrix_generations);
        cache->generation = 1;
    }
    cache->full_mask = 0;
    cache->cp_known = 0;
}

static inline int starlet_gx_bp_keep(StarletGxStateCache* cache, uint32_t word, int recording)
{
    unsigned address = word >> 24, slot = address;
    int full, same_tev = 1;
    if (recording) return 1;
    if (!cache->generation) starlet_gx_state_forget(cache);
    if (address == 0xfe) {
        cache->full_mask = (word & 0xffffff) == 0xffffff;
        return 1;
    }
    full = cache->full_mask;
    cache->full_mask = 1;
    /* Only pure draw state; texture loads, cache operations and sync always emit. */
    if (!(address == 0 || (address >= 6 && address <= 0x44) || address == 0x59 ||
          address == 0x68 || (address >= 0xc0 && address < 0xfe))) return 1;
    if (address >= 0xe0 && address <= 0xe7) {
        if (!full) {
            cache->generations[address] = cache->generations[256 + address - 0xe0] = 0;
            return 1;
        }
        if (word & 0x800000) slot = 256 + address - 0xe0;
        same_tev = cache->last_tev[address - 0xe0] == word;
        cache->last_tev[address - 0xe0] = word;
    }
    if (!full) {
        cache->generations[slot] = 0;
        return 1;
    }
    if (same_tev && cache->generations[slot] == cache->generation && cache->words[slot] == word) return 0;
    cache->words[slot] = word;
    cache->generations[slot] = cache->generation;
    return 1;
}

static inline int starlet_gx_xf_keep(StarletGxStateCache* cache, unsigned address, uint32_t word, int recording)
{
    /* Matrix-index writes restore per-vertex latches even when their words repeat. */
    if (recording || !(address == 9 || (address >= 0x0e && address <= 0x12) || address == 0x3f ||
                       (address >= 0x40 && address < 0x48) || (address >= 0x50 && address < 0x58))) return 1;
    if (!cache->generation) starlet_gx_state_forget(cache);
    if (cache->xf_generations[address] == cache->generation && cache->xf_words[address] == word) return 0;
    cache->xf_words[address] = word;
    cache->xf_generations[address] = cache->generation;
    return 1;
}

static inline void starlet_gx_cp_note(StarletGxStateCache* cache, unsigned opcode, unsigned address, uint32_t word, int recording)
{
    unsigned slot;
    if (recording || opcode != 8) return;
    if (!cache->generation) starlet_gx_state_forget(cache);
    if (address == 0x50) slot = 0;
    else if (address == 0x60) slot = 1;
    else if (address >= 0x70 && address < 0x78) slot = 2 + address - 0x70;
    else return;
    cache->cp_layout[slot] = word;
    cache->cp_known |= 1u << slot;
}

static inline int starlet_gx_cp_keep(StarletGxStateCache* cache, unsigned opcode, unsigned address, uint32_t word, int recording)
{
    if (recording || opcode != 8 || !(address == 0x50 || address == 0x60 ||
        (address >= 0x70 && address <= 0x97 && (address & 15) < 8) ||
        (address >= 0xa0 && address <= 0xbf))) return 1;
    if (!cache->generation) starlet_gx_state_forget(cache);
    if (cache->cp_generations[address] == cache->generation && cache->cp_words[address] == word) return 0;
    cache->cp_words[address] = word;
    cache->cp_generations[address] = cache->generation;
    starlet_gx_cp_note(cache, opcode, address, word, 0);
    return 1;
}

static inline int starlet_gx_matrix_keep(StarletGxStateCache* cache, unsigned address, const void* data,
                                         unsigned rows, unsigned columns, unsigned stride, int recording)
{
    const uint8_t* bytes = (const uint8_t*)data;
    unsigned row, column, count;
    int changed = 0;
    if (recording || !rows || rows > 16 || !columns || columns > 16 || rows > 16 / columns || stride < columns) return 1;
    count = rows * columns;
    if (address >= 1536 || count > 1536 - address) return 1;
    if (!cache->generation) starlet_gx_state_forget(cache);
    for (row = 0; row < rows; ++row) for (column = 0; column < columns; ++column) {
        uint32_t word;
        const unsigned slot = address + row * columns + column;
        memcpy(&word, bytes + (row * stride + column) * 4, 4);
        changed |= cache->matrix_generations[slot] != cache->generation || cache->matrix_words[slot] != word;
        cache->matrix_words[slot] = word;
        cache->matrix_generations[slot] = cache->generation;
    }
    return changed;
}

/* Index-only geometry lists leave BP and cached texgen registers unchanged. */
static inline int starlet_gx_indexed_draw_list(const StarletGxStateCache* cache, const void* data, unsigned size)
{
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t vcd_low = cache->cp_layout[0], vcd_high = cache->cp_layout[1];
    unsigned offset = 0, base_stride = 0, normal_mode = (vcd_low >> 11) & 3, i;
    if (!data || size > 65536 || (cache->cp_known & 3) != 3) return 0;
    for (i = 0; i < 9; ++i) base_stride += (vcd_low >> i) & 1;
    for (i = 0; i < 12; ++i) {
        unsigned mode = i < 4 ? (vcd_low >> (9 + i * 2)) & 3 : (vcd_high >> ((i - 4) * 2)) & 3;
        if (mode == 1) return 0;
        if (mode) base_stride += mode - 1;
    }
    while (offset < size) {
        unsigned opcode = bytes[offset], primitive = opcode & 0xf8, count, stride = base_stride;
        if (!opcode) { ++offset; continue; }
        if (primitive != 0x80 && primitive != 0x90 && primitive != 0x98 && primitive != 0xa0 &&
            primitive != 0xa8 && primitive != 0xb0 && primitive != 0xb8) return 0;
        if (size - offset < 3 || ((vcd_low >> 9) & 3) < 2) return 0;
        if (normal_mode) {
            if (!(cache->cp_known & (4u << (opcode & 7)))) return 0;
            if ((cache->cp_layout[2 + (opcode & 7)] & 0x80000200u) == 0x80000200u) stride += 2 * (normal_mode - 1);
        }
        count = ((unsigned)bytes[offset + 1] << 8) | bytes[offset + 2];
        if (!stride || count > (size - offset - 3) / stride) return 0;
        offset += 3 + count * stride;
    }
    return 1;
}

int starlet_gx_draw_list_keeps_state(const void* data, unsigned size);

/* Arrays of 32-bit numbers the game fills natively for GXSetArray (not file
 * data): the GX converts them to canonical order while the registration lasts.
 * Release before the memory is reused. */
void starlet_gx_host_order_array(const void* base, unsigned long bytes);
void starlet_gx_release_array(const void* base);
/* With GpuSetup::arrays_host_order: an array that holds canonical big-endian GX bytes (retail
 * bytes the port does not convert). Release before the memory is reused. */
void starlet_gx_big_endian_array(const void* base, unsigned long bytes);

/* Makes room for bytes more bytes (submits the chunk, or flags list overflow). */
void starlet_gx_reserve(unsigned bytes);

static inline void starlet_gx_u8(uint8_t value)
{
    if (starlet_gx_stage.end - starlet_gx_stage.cur < 1) starlet_gx_reserve(1);
    *starlet_gx_stage.cur++ = value;
}

static inline void starlet_gx_u16(uint16_t value)
{
    if (starlet_gx_stage.end - starlet_gx_stage.cur < 2) starlet_gx_reserve(2);
    starlet_gx_stage.cur[0] = (uint8_t)(value >> 8);
    starlet_gx_stage.cur[1] = (uint8_t)value;
    starlet_gx_stage.cur += 2;
}

static inline void starlet_gx_u32(uint32_t value)
{
    if (starlet_gx_stage.end - starlet_gx_stage.cur < 4) starlet_gx_reserve(4);
    starlet_gx_stage.cur[0] = (uint8_t)(value >> 24);
    starlet_gx_stage.cur[1] = (uint8_t)(value >> 16);
    starlet_gx_stage.cur[2] = (uint8_t)(value >> 8);
    starlet_gx_stage.cur[3] = (uint8_t)value;
    starlet_gx_stage.cur += 4;
}

static inline void starlet_gx_f32(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    starlet_gx_u32(bits);
}

#ifdef __cplusplus
}
#endif

/* The SDK's matrix writers take the pipe's address as a destination they ignore. */
#define GXWGFifo starlet_gx_stage

#define GX_WRITE_U8(val)  starlet_gx_u8((uint8_t)(val))
#define GX_WRITE_S8(val)  starlet_gx_u8((uint8_t)(int8_t)(val))
#define GX_WRITE_U16(val) starlet_gx_u16((uint16_t)(val))
#define GX_WRITE_S16(val) starlet_gx_u16((uint16_t)(int16_t)(val))
#define GX_WRITE_U32(val) starlet_gx_u32((uint32_t)(val))
#define GX_WRITE_S32(val) starlet_gx_u32((uint32_t)(int32_t)(val))
#define GX_WRITE_F32(val) starlet_gx_f32((float)(val))

#endif
