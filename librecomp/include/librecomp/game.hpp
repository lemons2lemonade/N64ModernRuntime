#ifndef __RECOMP_GAME__
#define __RECOMP_GAME__

#include <vector>
#include <filesystem>

#include "recomp.h"
#include "rsp.hpp"
#include <ultramodern/ultramodern.hpp>

namespace recomp {
    enum class SaveType {
        None,
        Eep4k,
        Eep16k,
        Sram,
        Flashram,
        AllowAll, // Allows all save types to work and reports eeprom size as 16kbit.
    };

    struct GameEntry {
        uint64_t rom_hash;
        std::string internal_name;
        std::u8string game_id;
        std::string mod_game_id;
        SaveType save_type = SaveType::None;
        bool is_enabled;
        // Only needed for mod function hooking support, not needed if `has_compressed_code` is false.
        std::vector<uint8_t> (*decompression_routine)(std::span<const uint8_t> compressed_rom) = nullptr;
        bool has_compressed_code = false;

        gpr entrypoint_address;
        void (*entrypoint)(uint8_t* rdram, recomp_context* context) = nullptr;

        void (*thread_create_callback)(uint8_t* rdram, recomp_context* context) = nullptr;

        void (*on_init_callback)(uint8_t* rdram, recomp_context* context) = nullptr;

        std::u8string stored_filename() const;
    };
    struct Version {
        int major = -1;
        int minor = -1;
        int patch = -1;
        std::string suffix;

        std::string to_string() const {
            return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch) + suffix;
        }

        static bool from_string(const std::string& str, Version& out);

        auto operator<=>(const Version& rhs) const {
            if (major != rhs.major) {
                return major <=> rhs.major;
            }
            if (minor != rhs.minor) {
                return minor <=> rhs.minor;
            }
            return patch <=> rhs.patch;
        } 
    };
    enum class RomValidationError {
        Good,
        FailedToOpen,
        NotARom,
        IncorrectRom,
        NotYet,
        IncorrectVersion,
        OtherError
    };
    void register_config_path(std::filesystem::path path);
    bool register_game(const recomp::GameEntry& entry);
    void check_all_stored_roms();
    bool load_stored_rom(std::u8string& game_id);
    RomValidationError select_rom(const std::filesystem::path& rom_path, std::u8string& game_id);
    bool is_rom_valid(std::u8string& game_id);
    bool is_rom_loaded();
    void set_rom_contents(std::vector<uint8_t>&& new_rom);
    std::span<const uint8_t> get_rom();
    void do_rom_read(uint8_t* rdram, gpr ram_address, uint32_t physical_addr, size_t num_bytes);
    void do_rom_pio(uint8_t* rdram, gpr ram_address, uint32_t physical_addr);

    // Direct-MMIO register model (librecomp/src/mmio.cpp). Conventional games poll RCP + cartridge
    // hardware registers directly (not via the osPi/osSi HLE); the flat-rdram model backs those
    // addresses only as zeroed memory. mmio_init seeds the committed MMIO shadow with the correct
    // values (RCP status = idle/ready; cartridge window = the real ROM bytes) so those direct reads
    // resolve. Call once, after the ROM is loaded and before the game entrypoint runs.
    void mmio_init(uint8_t* rdram);
    uint32_t mmio_read(uint32_t vaddr, int size);
    void mmio_write(uint32_t vaddr, int size, uint32_t val);
    bool mmio_is_register(uint32_t vaddr);

    // Commit (make READ/WRITE) a guest-address window inside the reserved 4 GiB rdram allocation
    // and optionally populate a sub-range with ROM bytes. General mechanism; games opt in via the
    // per-game GameEntry::on_init_callback (so other games' out-of-bounds guard is unchanged). The
    // flat MEM_* model backs only KSEG0 + the KSEG1 raw-MMIO window; a game whose code/data runs in
    // a KUSEG band (e.g. GoldenEye's Rare engine at guest 0x70200000 / 0x7F000000, TLB-mapped on
    // hardware) would otherwise fault into the PROT_NONE guard on its first band access. Committing
    // + populating that band window lets the recompiled band code run in the flat model.
    //   guest_vaddr : first guest virtual address of the window (e.g. 0x7F000000)
    //   size        : window length in bytes (page-rounded outward for the protect call)
    //   rom_src     : ROM file offset to copy from (only used when rom_len != 0)
    //   rom_len     : number of ROM bytes to copy (0 => commit only, no populate)
    //   dst_off     : byte offset within the window where the ROM copy lands
    // Returns false on protect/copy failure. Populates via the same per-byte swap do_rom_read uses
    // (MEM_B's ^3) so bytes read back correctly through MEM_W/MEM_H/MEM_B.
    bool commit_and_populate_window(uint8_t* rdram, uint32_t guest_vaddr, uint32_t size,
                                    uint32_t rom_src, uint32_t rom_len, uint32_t dst_off);
    const Version& get_project_version();

    /// Specify the input configuration to the recomp runtime.
    /// 
    /// The following callback fields are mandatory (i.e., fail on empty()):
    /// - `rsp_callbacks`
    /// - `renderer_callbacks`
    /// 
    struct Configuration {
        Version project_version;
        ultramodern::renderer::WindowHandle window_handle;
        recomp::rsp::callbacks_t rsp_callbacks;
        ultramodern::renderer::callbacks_t renderer_callbacks;
        ultramodern::audio_callbacks_t audio_callbacks;
        ultramodern::input::callbacks_t input_callbacks;
        ultramodern::gfx_callbacks_t gfx_callbacks;
        ultramodern::events::callbacks_t events_callbacks;
        ultramodern::error_handling::callbacks_t error_handling_callbacks;
        ultramodern::threads::callbacks_t threads_callbacks;
        ultramodern::MessageQueueControl message_queue_control;
    };

    /// Start the recomp runtime.
    /// 
    /// This routine must be called only once and it must be called before
    /// `ultramodern::preinit`.
    /// 
    void start(const Configuration& cfg);

    SaveType get_save_type();
    bool eeprom_allowed();
    bool sram_allowed();
    bool flashram_allowed();

    void start_game(const std::u8string& game_id);
    std::u8string current_game_id();
    std::string current_mod_game_id();
}

#endif
