// mmio.cpp — direct-MMIO register model for the flat-RDRAM recompiled memory model.
//
// WHY THIS EXISTS
// ---------------
// N64Recomp lowers every guest load/store to the MEM_* macros in recomp.h, which index a flat
// `rdram` array at (guest_vaddr - 0x80000000). That model backs KSEG0 RAM (0x80000000..) and — after
// recomp.cpp commits the KSEG1 window [0xA0000000,0xC0000000) — also backs raw KSEG1 accesses, but
// only as ZEROED memory. Conventional libultra games (Pokémon Stadium 2, Paperboy) don't always go
// through the osPi/osSi HLE for their innermost boot/timing loops: they poll the RCP + cartridge
// HARDWARE REGISTERS DIRECTLY. Two concrete walls, both diagnosed from the recompiled source:
//
//   * Stadium 2 main thread func_80064FB0 busy-waits on func_80064C20, which does
//       lw 0xA4600010  (PI_STATUS)  — loops unless (val & 3) == 0   [not busy / not error]
//       lw 0xB0000E38  (cart word)  — proceeds only if (val & 0xFFFF) == 0x828A  [ROM magic]
//     The real ROM word at file-offset 0xE38 is 0x1060828A (low half == 0x828A), i.e. the game reads
//     its OWN ROM header through the PI cartridge domain window. func_80069750 has the same shape at
//     cart offset 0x504.
//
//   * Paperboy's boot polls SI_STATUS at 0xA4800018, looping while (val & 3) != 0.
//
// A zeroed window already satisfies both STATUS polls (0 == not-busy), which is why the KSEG1 commit
// got Paperboy past its SI wall. Stadium 2's second condition needs a NON-zero value — the actual ROM
// bytes in the cartridge window — so a zeroed window is not enough. This file is the register model:
// the single source of truth for what a direct MMIO read returns, plus a seeding step that writes
// those values into the committed shadow so the existing MEM_* fast path reads them with no per-load
// branch (keeping the hot path — every load in the whole program — untouched).
//
// SCOPE (intentionally bounded)
//   RCP register block  0xA4000000..0xA4FFFFFF  (SP/DP/MI/VI/AI/PI 0xA46xxxxx/SI 0xA48xxxxx/RI)
//   Cartridge/PI domain 0xB0000000.. and physical ROM 0x10000000..  -> backed by real ROM bytes
//   Everything else in-window: left as the zeroed default (a sensible "idle/ready" value).
//
// This does NOT touch the working HLE'd osPiRead/osSiRawRead/DMA paths — those still run through
// librecomp as before. This only backs the DIRECT reads the recompiled code makes.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <span>
#include <algorithm>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"

namespace {

// KSEG0/KSEG1 both alias the same 512MB physical space; strip the segment bits to get a physical
// address (0x1FFFFFFF mask, the N64 29-bit physical space) for classification.
inline uint32_t seg_to_phys(uint32_t vaddr) { return vaddr & 0x1FFFFFFF; }

// The flat-rdram byte offset a guest vaddr maps to under the MEM_* macros: (vaddr - 0x80000000),
// taken modulo the 32-bit space so KSEG1 (0xA…/0xB…) and KSEG0 (0x8…/0x9…) both land in range.
inline uint32_t rdram_offset(uint32_t vaddr) { return (uint32_t)(vaddr - 0x80000000u); }

// Physical layout of the classes we model.
constexpr uint32_t RCP_PHYS_BASE = 0x04000000u; // 0xA4000000 stripped
constexpr uint32_t RCP_PHYS_END  = 0x05000000u; // 0xA5000000 stripped (exclusive)
constexpr uint32_t ROM_PHYS_BASE = 0x10000000u; // cartridge PI domain 1 / recomp::rom_base
constexpr uint32_t ROM_PHYS_END  = 0x1FC00000u; // up to PIF (exclusive); covers the 64MB cart

// Specific registers the games poll (physical offsets within the RCP block).
constexpr uint32_t PI_STATUS_PHYS = 0x04600010u; // 0xA4600010 : DMA/IO busy+error bits [1:0]
constexpr uint32_t SI_STATUS_PHYS = 0x04800018u; // 0xA4800018 : SI busy bit
constexpr uint32_t SP_STATUS_PHYS = 0x04040010u; // 0xA4040010 : RSP halt/broke status
constexpr uint32_t MI_INTR_PHYS   = 0x04300008u; // 0xA4300008 : MI interrupt (none pending)

std::atomic<uint32_t> g_mmio_write_log_count{0};

} // namespace

namespace recomp {

// Classify: does this guest address fall in a window we model?
bool mmio_is_register(uint32_t vaddr) {
    const uint32_t phys = seg_to_phys(vaddr);
    if (phys >= RCP_PHYS_BASE && phys < RCP_PHYS_END) return true;
    if (phys >= ROM_PHYS_BASE && phys < ROM_PHYS_END) return true;
    return false;
}

// The authoritative register model: what a direct read of `size` bytes at guest `vaddr` returns.
// Returns a value already in guest (big-endian-word) semantics — i.e. the value the recompiled
// MEM_W(vaddr) would yield. Callers that seed the shadow must byte-lay it accordingly.
uint32_t mmio_read(uint32_t vaddr, int size) {
    const uint32_t phys = seg_to_phys(vaddr);

    // --- Cartridge / ROM domain: back with the real ROM bytes. ---
    if (phys >= ROM_PHYS_BASE && phys < ROM_PHYS_END) {
        std::span<const uint8_t> rom = recomp::get_rom();
        const uint32_t rom_off = phys - ROM_PHYS_BASE;
        if (!rom.empty() && (size_t)rom_off + 4 <= rom.size()) {
            // ROM is stored big-endian (z64). Assemble a big-endian word so (& 0xFFFF) etc. match
            // the game's expectation (e.g. 0x1060828A at 0xE38 -> low16 0x828A).
            const uint8_t* p = rom.data() + rom_off;
            uint32_t w = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                         ((uint32_t)p[2] << 8) | ((uint32_t)p[3] << 0);
            switch (size) {
                case 1: return (w >> 24) & 0xFF;
                case 2: return (w >> 16) & 0xFFFF;
                default: return w;
            }
        }
        return 0;
    }

    // --- RCP register block. ---
    if (phys >= RCP_PHYS_BASE && phys < RCP_PHYS_END) {
        switch (phys) {
            case PI_STATUS_PHYS: return 0; // not busy, not error  (game loops while & 3 != 0)
            case SI_STATUS_PHYS: return 0; // not busy             (game loops while & 3 != 0)
            case SP_STATUS_PHYS: return 0x1; // HALT set, no other flags — RSP idle/available
            case MI_INTR_PHYS:   return 0; // no interrupts pending
            default:             return 0; // sensible default for unmodeled RCP regs (idle/ready)
        }
    }

    return 0;
}

// Direct MMIO writes are no-ops in this model (the working state lives in the HLE). Log a bounded
// number so a game that pokes a register we haven't modeled is visible without flooding.
void mmio_write(uint32_t vaddr, int size, uint32_t val) {
    uint32_t n = g_mmio_write_log_count.fetch_add(1);
    if (n < 32) {
        std::fprintf(stderr, "[mmio] write ignored: [0x%08X] size=%d val=0x%08X\n", vaddr, size, val);
    }
}

// Seed the committed MMIO shadow so the existing MEM_* fast path reads the modeled values with no
// per-load branch. Call once, after the ROM is loaded and rdram is committed, before the game
// entrypoint runs. Uses the MEM_B macro (identical byte-swap semantics to recomp::do_rom_read) so
// the seeded bytes read back correctly through MEM_W/MEM_H/MEM_B.
void mmio_init(uint8_t* rdram) {
    // A/B / debugging escape hatch: MC_MMIO_DISABLE=1 skips the cartridge-magic seed so the
    // direct-MMIO busy-wait is provably the thing this unblocks (with it set, Stadium 2 falls back
    // to its infinite poll; cleared, it advances). Not a shipping switch — a diagnostic control.
    if (const char* d = getenv("MC_MMIO_DISABLE"); d && d[0] == '1') {
        std::fprintf(stderr, "[mmio] DISABLED via MC_MMIO_DISABLE=1 (no register/cart seed)\n");
        return;
    }
    // 1. RCP status registers -> their idle/ready model values (written as big-endian words via the
    //    per-byte MEM_B store, matching how the recompiled code reads them).
    auto seed_word = [&](uint32_t vaddr, uint32_t w) {
        MEM_B(0, (int64_t)(int32_t)vaddr) = (int8_t)((w >> 24) & 0xFF);
        MEM_B(1, (int64_t)(int32_t)vaddr) = (int8_t)((w >> 16) & 0xFF);
        MEM_B(2, (int64_t)(int32_t)vaddr) = (int8_t)((w >> 8) & 0xFF);
        MEM_B(3, (int64_t)(int32_t)vaddr) = (int8_t)((w >> 0) & 0xFF);
    };
    seed_word(0xA4600010u, recomp::mmio_read(0xA4600010u, 4)); // PI_STATUS
    seed_word(0xA4800018u, recomp::mmio_read(0xA4800018u, 4)); // SI_STATUS
    seed_word(0xA4040010u, recomp::mmio_read(0xA4040010u, 4)); // SP_STATUS
    seed_word(0xA4300008u, recomp::mmio_read(0xA4300008u, 4)); // MI_INTR

    // 2. Cartridge window 0xB0000000+ -> the real ROM bytes, so DIRECT cart reads (the game reading
    //    its own header magic at 0xB0000E38 == 0x828A, and the 0x504 poll) resolve. PI DMA already
    //    handles the bulk data copies; this only backs the direct-register-style reads at low
    //    offsets, so a bounded prefix (header + boot/IPL) is sufficient and cheap. Mirror the exact
    //    do_rom_read byte loop against guest 0xB0000000 so endianness is byte-identical.
    std::span<const uint8_t> rom = recomp::get_rom();
    if (!rom.empty()) {
        constexpr uint32_t kSeedBytes = 0x100000u; // 1 MB: covers header, IPL3, and boot segment
        const uint32_t n = (uint32_t)std::min<size_t>(rom.size(), kSeedBytes);
        const uint32_t cart_base = 0xB0000000u; // KSEG1 cartridge domain == phys rom_base
        const uint8_t* src = rom.data();
        for (uint32_t i = 0; i < n; i++) {
            MEM_B(i, (int64_t)(int32_t)cart_base) = (int8_t)src[i];
        }
        std::fprintf(stderr, "[mmio] seeded RCP status regs + %u ROM bytes into cart window 0x%08X\n",
                     n, cart_base);
    } else {
        std::fprintf(stderr, "[mmio] WARNING: ROM not loaded at mmio_init; cart-window reads will be 0\n");
    }
}

// General mechanism (see game.hpp): commit a KUSEG/KSEG band window in the reserved allocation and
// optionally seed it with ROM bytes. The boot-time sibling of mmio_init for games with TLB-mapped
// run-VRAM bands the flat model would otherwise leave PROT_NONE.
bool commit_and_populate_window(uint8_t* rdram, uint32_t guest_vaddr, uint32_t size,
                                uint32_t rom_src, uint32_t rom_len, uint32_t dst_off) {
    // Flat rdram offset a guest vaddr maps to under MEM_*: (vaddr - 0x80000000) mod 2^32.
    const uint64_t flat = (uint64_t)(uint32_t)(guest_vaddr - 0x80000000u);
    // Page-align the protect range outward so a sub-page window still gets fully committed.
    constexpr uint64_t pagesz = 0x1000ULL;
    const uint64_t start = flat & ~(pagesz - 1);
    const uint64_t end   = (flat + size + pagesz - 1) & ~(pagesz - 1);
    if (end > recomp::allocation_size) {
        std::fprintf(stderr, "[window] ERROR: guest 0x%08X window exceeds the rdram allocation\n", guest_vaddr);
        return false;
    }
#ifdef _WIN32
    DWORD old = 0;
    if (VirtualProtect(rdram + start, (SIZE_T)(end - start), PAGE_READWRITE, &old) == 0) {
        std::fprintf(stderr, "[window] ERROR: VirtualProtect failed for guest 0x%08X\n", guest_vaddr);
        return false;
    }
#else
    if (mprotect(rdram + start, (size_t)(end - start), PROT_READ | PROT_WRITE) == -1) {
        std::perror("[window] mprotect");
        return false;
    }
#endif
    if (rom_len) {
        std::span<const uint8_t> rom = recomp::get_rom();
        if (rom.empty() || (size_t)rom_src + rom_len > rom.size()) {
            std::fprintf(stderr, "[window] ERROR: ROM range 0x%X..0x%X out of bounds for guest 0x%08X\n",
                         rom_src, rom_src + rom_len, guest_vaddr);
            return false;
        }
        const uint8_t* src = rom.data() + rom_src;
        const int64_t base = (int64_t)(int32_t)(guest_vaddr + dst_off);
        for (uint32_t i = 0; i < rom_len; i++) {
            MEM_B(i, base) = (int8_t)src[i];
        }
    }
    std::fprintf(stderr, "[window] committed guest 0x%08X size 0x%X (flat 0x%llX..0x%llX)%s\n",
                 guest_vaddr, size, (unsigned long long)start, (unsigned long long)end,
                 rom_len ? " + ROM-populated" : "");
    return true;
}

} // namespace recomp
