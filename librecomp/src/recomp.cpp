#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <memory>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <sstream>
#include <iostream>
#include <optional>
#include <mutex>
#include <thread>
#include <vector>
#include <array>
#include <cinttypes>
#include <cuchar>
#include <charconv>

// HH: capa de instrumentacion/diagnostico (canary, watchpoint de hardware, logs). Para una build
// de release limpia: compilar con -DHH_DEBUG_TOOLS=0 (no hay salida por consola ni ficheros).
#ifndef HH_DEBUG_TOOLS
#define HH_DEBUG_TOOLS 1
#endif

#include "recomp.h"
#include "librecomp/overlays.hpp"
#include "ultramodern/ultra64.h"
#include "librecomp/game.hpp"
#include "xxHash/xxh3.h"

extern "C" void init_mmio(uint8_t *rdram);
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/error_handling.hpp"
#include "librecomp/addresses.hpp"
#include "librecomp/mods.hpp"
#include "recompiler/live_recompiler.h"

#ifdef _WIN32
#    define WIN32_LEAN_AND_MEAN
#    include <Windows.h>
#else
#    include <sys/mman.h>
#endif

#if defined(_WIN32)
#define PATHFMT "%ls"
#else
#define PATHFMT "%s"
#endif

enum GameStatus {
    None,
    Running,
    Quit
};

// Mutexes
std::mutex game_roms_mutex;
std::mutex current_game_mutex;
std::mutex mod_context_mutex{};

// Simple file logger used by the boot path so progress is visible even without a console.
// Opt-in via HH_BOOTLOG=<path>; when unset, boot logging is a no-op (no file is created).
static FILE* g_boot_log = nullptr;
static void boot_log(const char* fmt, ...) {
    static const char* log_path = getenv("HH_BOOTLOG");
    if (log_path == nullptr) {
        return;
    }
    if (g_boot_log == nullptr) {
        // Truncate on first open so each run starts fresh.
        g_boot_log = std::fopen(log_path, "w");
    }
    if (g_boot_log == nullptr) {
        return;
    }
    // Prefix with a millisecond timestamp.
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    std::fprintf(g_boot_log, "[%s.%03u] ", buf, (unsigned)ms.count());

    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_boot_log, fmt, args);
    va_end(args);
    std::fflush(g_boot_log);
}

// Global variables
std::filesystem::path config_path;
// Maps game_id to the game's entry.
std::unordered_map<std::u8string, recomp::GameEntry> game_roms {};
// The global mod context.
std::unique_ptr<recomp::mods::ModContext> mod_context = std::make_unique<recomp::mods::ModContext>();
// The project's version.
recomp::Version project_version;
// The current game's save type.
recomp::SaveType save_type = recomp::SaveType::None;

std::u8string recomp::GameEntry::stored_filename() const {
    return game_id + u8".z64";
}

void recomp::register_config_path(std::filesystem::path path) {
    config_path = path;
}

std::filesystem::path recomp::get_config_path() {
    return config_path;
}

bool recomp::register_game(const recomp::GameEntry& entry) {
    if (entry.display_name.empty()) {
        ultramodern::error_handling::message_box("Game display name was not set.");
        ULTRAMODERN_QUICK_EXIT();
    }
    // TODO verify that there's no game with this ID already.
    {
        std::lock_guard<std::mutex> lock(game_roms_mutex);
        game_roms.insert({ entry.game_id, entry });
    }
    if (!entry.mod_game_id.empty()) {
        std::lock_guard<std::mutex> lock(mod_context_mutex);
        mod_context->register_game(entry.mod_game_id);
    }
    return true;
}

void recomp::mods::initialize_mods() {
    N64Recomp::live_recompiler_init();
    std::filesystem::create_directories(config_path / mods_directory);
    std::filesystem::create_directories(config_path / mod_config_directory);
    mod_context->set_mods_config_path(config_path / "mods.json");
    mod_context->set_mod_config_directory(config_path / mod_config_directory);
}

void recomp::mods::register_embedded_mod(const std::string &mod_id, std::span<const uint8_t> mod_bytes) {
    std::lock_guard<std::mutex> lock(mod_context_mutex);
    mod_context->register_embedded_mod(mod_id, mod_bytes);
}

void recomp::mods::register_deprecated_mod(const std::string& mod_id, recomp::mods::DeprecationStatus deprecation_status, const Version& maximum_version) {
    std::lock_guard<std::mutex> lock(mod_context_mutex);
    mod_context->register_deprecated_mod(mod_id, deprecation_status, maximum_version);
}

void recomp::mods::scan_mods() {
    std::vector<recomp::mods::ModOpenErrorDetails> mod_open_errors;
    {
        std::lock_guard mod_lock{ mod_context_mutex };
        mod_open_errors = mod_context->scan_mod_folder(config_path / mods_directory);
    }
    for (const auto& cur_error : mod_open_errors) {
        printf("Error opening mod " PATHFMT ": %s (%s)\n", cur_error.mod_path.c_str(), recomp::mods::error_to_string(cur_error.error).c_str(), cur_error.error_param.c_str());
    }

    mod_context->load_mods_config();
}

void recomp::mods::close_mods() {
    {
        std::lock_guard mod_lock{ mod_context_mutex };
        mod_context->close_mods();
    }
}

std::filesystem::path recomp::mods::get_mods_directory() {
    return config_path / mods_directory;
}

recomp::mods::ModContentTypeId recomp::mods::register_mod_content_type(const ModContentType& type) {
    std::lock_guard mod_lock{ mod_context_mutex };
    return mod_context->register_content_type(type);
}

bool recomp::mods::register_mod_container_type(const std::string& extension, const std::vector<ModContentTypeId>& content_types, bool requires_manifest) {
    std::lock_guard mod_lock{ mod_context_mutex };
    return mod_context->register_container_type(extension, content_types, requires_manifest);
}

std::string recomp::mods::get_mod_display_name(size_t mod_index) {
    std::lock_guard mod_lock{ mod_context_mutex };
    return mod_context->get_mod_display_name(mod_index);
}

std::filesystem::path recomp::mods::get_mod_path(size_t mod_index) {
    std::lock_guard mod_lock{ mod_context_mutex };
    return mod_context->get_mod_path(mod_index);
}

std::pair<std::string, std::string> recomp::mods::get_mod_import_info(size_t mod_index, size_t import_index) {
    std::lock_guard mod_lock{ mod_context_mutex };
    return mod_context->get_mod_import_info(mod_index, import_index);
}

recomp::mods::DependencyStatus recomp::mods::is_dependency_met(size_t mod_index, const std::string& dependency_id) {
    std::lock_guard mod_lock{ mod_context_mutex };
    return mod_context->is_dependency_met(mod_index, dependency_id);
}

bool check_hash(const std::vector<uint8_t>& rom_data, uint64_t expected_hash) {
    uint64_t calculated_hash = XXH3_64bits(rom_data.data(), rom_data.size());
    return calculated_hash == expected_hash;
}

static std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::vector<uint8_t> ret;

    std::ifstream file{ path, std::ios::binary};

    if (file.good()) {
        file.seekg(0, std::ios::end);
        ret.resize(file.tellg());
        file.seekg(0, std::ios::beg);

        file.read(reinterpret_cast<char*>(ret.data()), ret.size());
    }

    return ret;
}

bool write_file(const std::filesystem::path& path, const std::vector<uint8_t>& data) {
    std::ofstream out_file{ path, std::ios::binary };

    if (!out_file.good()) {
        return false;
    }

    out_file.write(reinterpret_cast<const char*>(data.data()), data.size());

    return true;
}

bool check_stored_rom(const recomp::GameEntry& game_entry) {
    std::vector stored_rom_data = read_file(config_path / game_entry.stored_filename());

    if (!check_hash(stored_rom_data, game_entry.rom_hash)) {
        // Incorrect hash, remove the stored ROM file if it exists.
        std::filesystem::remove(config_path / game_entry.stored_filename());
        return false;
    }

    return true;
}

static std::unordered_set<std::u8string> valid_game_roms;

bool recomp::is_rom_valid(const std::u8string& game_id) {
    return valid_game_roms.contains(game_id);
}

void recomp::check_all_stored_roms() {
    for (const auto& cur_rom_entry: game_roms) {
        if (check_stored_rom(cur_rom_entry.second)) {
            valid_game_roms.insert(cur_rom_entry.first);
        }
    }
}

bool recomp::load_stored_rom(const std::u8string& game_id) {
    auto find_it = game_roms.find(game_id);

    if (find_it == game_roms.end()) {
        return false;
    }
    
    std::vector<uint8_t> stored_rom_data = read_file(config_path / find_it->second.stored_filename());

    if (!check_hash(stored_rom_data, find_it->second.rom_hash)) {
        // The ROM no longer has the right hash, delete it.
        std::filesystem::remove(config_path / find_it->second.stored_filename());
        return false;
    }

    recomp::set_rom_contents(std::move(stored_rom_data));
    return true;
}

const recomp::Version& recomp::get_project_version() {
    return project_version;
}

bool recomp::Version::from_string(const std::string& str, Version& out) {
    std::array<size_t, 2> period_indices;
    size_t cur_pos = 0;
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
    std::string suffix;

    // Find the 2 required periods.
    cur_pos = str.find('.', cur_pos);
    period_indices[0] = cur_pos;
    cur_pos = str.find('.', cur_pos + 1);
    period_indices[1] = cur_pos;

    // Check that both were found.
    if (period_indices[0] == std::string::npos || period_indices[1] == std::string::npos) {
        return false;
    }

    // Parse the 3 numbers formed by splitting the string via the periods.
    std::array<std::from_chars_result, 3> parse_results; 
    std::array<size_t, 3> parse_starts { 0, period_indices[0] + 1, period_indices[1] + 1 };
    std::array<size_t, 3> parse_ends { period_indices[0], period_indices[1], str.size() };
    parse_results[0] = std::from_chars(str.data() + parse_starts[0], str.data() + parse_ends[0], major);
    parse_results[1] = std::from_chars(str.data() + parse_starts[1], str.data() + parse_ends[1], minor);
    parse_results[2] = std::from_chars(str.data() + parse_starts[2], str.data() + parse_ends[2], patch);

    // Check that the first two parsed correctly.
    auto did_parse = [&](size_t i) {
        return parse_results[i].ec == std::errc{} && parse_results[i].ptr == str.data() + parse_ends[i];
    };
    
    if (!did_parse(0) || !did_parse(1)) {
        return false;
    }

    // Check that the third had a successful parse, but not necessarily read all the characters.
    if (parse_results[2].ec != std::errc{}) {
        return false;
    }

    // Allow a plus or minus directly after the third number.
    if (parse_results[2].ptr != str.data() + parse_ends[2]) {
        if (*parse_results[2].ptr == '+' || *parse_results[2].ptr == '-') {
            suffix = str.substr(std::distance(str.data(), parse_results[2].ptr));
        }
        // Failed to parse, as nothing is allowed directly after the last number besides a plus or minus.
        else {
            return false;
        }
    }

    out.major = major;
    out.minor = minor;
    out.patch = patch;
    out.suffix = std::move(suffix);
    return true;
}

const std::array<uint8_t, 4> first_rom_bytes { 0x80, 0x37, 0x12, 0x40 };

enum class ByteswapType {
    NotByteswapped,
    Byteswapped4,
    Byteswapped2,
    Invalid
};

ByteswapType check_rom_start(const std::vector<uint8_t>& rom_data) {
    if (rom_data.size() < 4) {
        return ByteswapType::Invalid;
    }

    auto check_match = [&](uint8_t index0, uint8_t index1, uint8_t index2, uint8_t index3) {
        return
            rom_data[0] == first_rom_bytes[index0] &&
            rom_data[1] == first_rom_bytes[index1] &&
            rom_data[2] == first_rom_bytes[index2] &&
            rom_data[3] == first_rom_bytes[index3];
    };

    // Check if the ROM is already in the correct byte order.
    if (check_match(0,1,2,3)) {
        return ByteswapType::NotByteswapped;
    }

    // Check if the ROM has been byteswapped in groups of 4 bytes.
    if (check_match(3,2,1,0)) {
        return ByteswapType::Byteswapped4;
    }

    // Check if the ROM has been byteswapped in groups of 2 bytes.
    if (check_match(1,0,3,2)) {
        return ByteswapType::Byteswapped2;
    }

    // No match found.
    return ByteswapType::Invalid;
}

void byteswap_data(std::vector<uint8_t>& rom_data, size_t index_xor) {
    for (size_t rom_pos = 0; rom_pos < rom_data.size(); rom_pos += 4) {
        uint8_t temp0 = rom_data[rom_pos + 0];
        uint8_t temp1 = rom_data[rom_pos + 1];
        uint8_t temp2 = rom_data[rom_pos + 2];
        uint8_t temp3 = rom_data[rom_pos + 3];

        rom_data[rom_pos + (0 ^ index_xor)] = temp0;
        rom_data[rom_pos + (1 ^ index_xor)] = temp1;
        rom_data[rom_pos + (2 ^ index_xor)] = temp2;
        rom_data[rom_pos + (3 ^ index_xor)] = temp3;
    }
}

recomp::RomValidationError recomp::select_rom(const std::filesystem::path& rom_path, const std::u8string& game_id) {
    auto find_it = game_roms.find(game_id);

    if (find_it == game_roms.end()) {
        return recomp::RomValidationError::OtherError;
    }

    const recomp::GameEntry& game_entry = find_it->second;

    std::vector<uint8_t> rom_data = read_file(rom_path);

    if (rom_data.empty()) {
        return recomp::RomValidationError::FailedToOpen;
    }

    // Pad the rom to the nearest multiple of 4 bytes.
    rom_data.resize((rom_data.size() + 3) & ~3);

    ByteswapType byteswap_type = check_rom_start(rom_data);

    switch (byteswap_type) {
        case ByteswapType::Invalid:
            return recomp::RomValidationError::NotARom;
        case ByteswapType::Byteswapped2:
            byteswap_data(rom_data, 1);
            break;
        case ByteswapType::Byteswapped4:
            byteswap_data(rom_data, 3);
            break;
        case ByteswapType::NotByteswapped:
            break;
    }

    if (!check_hash(rom_data, game_entry.rom_hash)) {
        const std::string_view name{ reinterpret_cast<const char*>(rom_data.data()) + 0x20, game_entry.internal_name.size()};
        if (name == game_entry.internal_name) {
            return recomp::RomValidationError::IncorrectVersion;
        }
        else {
            if (game_entry.is_enabled && std::string_view{ reinterpret_cast<const char*>(rom_data.data()) + 0x20, 19 } == game_entry.internal_name) {
                return recomp::RomValidationError::NotYet;
            }
            else {
                return recomp::RomValidationError::IncorrectRom;
            }
        }
    }

    write_file(config_path / game_entry.stored_filename(), rom_data);
    
    return recomp::RomValidationError::Good;
}

extern "C" void osGetMemSize_recomp(uint8_t * rdram, recomp_context * ctx) {
    // Hybrid Heaven's boot checks osGetMemSize() against 0x400000 (4MB) and enters an
    // infinite wait loop otherwise. The retail game targets the base 4MB machine (no
    // Expansion Pak), so report 4MB even though the runtime allocates 8MB.
    (void)rdram;
    ctx->r2 = 4 * 1024 * 1024;
}

enum class StatusReg {
    FR = 0x04000000,
};

extern "C" void cop0_status_write(recomp_context* ctx, gpr value) {
    uint32_t old_sr = ctx->status_reg;
    uint32_t new_sr = (uint32_t)value;
    uint32_t changed = old_sr ^ new_sr;

    // Check if the FR bit changed
    if (changed & (uint32_t)StatusReg::FR) {
        // Check if the FR bit was set
        if (new_sr & (uint32_t)StatusReg::FR) {
            // FR = 1, odd single floats point to their own registers
            ctx->f_odd = &ctx->f1.u32l;
            ctx->mips3_float_mode = true;
        }
        // Otherwise, it was cleared
        else {
            // FR = 0, odd single floats point to the upper half of the previous register
            ctx->f_odd = &ctx->f0.u32h;
            ctx->mips3_float_mode = false;
        }

        // Remove the FR bit from the changed bits as it's been handled
        changed &= ~(uint32_t)StatusReg::FR;
    }

    // Update the status register in the context
    ctx->status_reg = new_sr;
}

extern "C" gpr cop0_status_read(recomp_context* ctx) {
    return (gpr)(int32_t)ctx->status_reg;
}

extern "C" void cop0_cause_write(recomp_context* ctx, gpr value) {
    ctx->cause_reg = (uint32_t)value;
}

extern "C" gpr cop0_cause_read(recomp_context* ctx) {
    // Cause register: not otherwise modeled. Return a well-defined value (initially
    // 0 = no pending exceptions/interrupts) so reads are safe.
    return (gpr)(int32_t)ctx->cause_reg;
}

extern "C" gpr cop0_register_read(recomp_context* ctx, uint32_t reg) {
    reg &= 31;
    if (reg == 12) {
        return cop0_status_read(ctx);
    }
    if (reg == 13) {
        return (gpr)(int32_t)ctx->cause_reg;
    }
    return (gpr)(int32_t)ctx->cop0_regs[reg];
}

extern "C" void cop0_register_write(recomp_context* ctx, uint32_t reg, gpr value) {
    reg &= 31;
    if (reg == 12) {
        cop0_status_write(ctx, value);
        return;
    }
    if (reg == 13) {
        ctx->cause_reg = (uint32_t)value;
        return;
    }
    ctx->cop0_regs[reg] = (uint32_t)value;
}

extern "C" void switch_error(const char* func, uint32_t vram, uint32_t jtbl) {
    fprintf(stderr, "Switch-case out of bounds in %s at 0x%08X for jump table at 0x%08X\n", func, vram, jtbl);
    fflush(stderr);
    assert(false);
    exit(EXIT_FAILURE);
}

extern "C" void do_break(uint32_t vram) {    // TODO: properly handle break by restoring PC to caller and continuing.
    // For now, just warn and return to avoid killing the process.
    printf("do_break: unhandled break at original vram 0x%08X (ignored)\n", vram);
    // HH: registrar en fichero (dedup) los break/stubs ejecutados: un simbolo mal acotado queda en
    // stub do_break y rompe el flujo en silencio (asi se cuelga el NPC). hh_stub.log junto al exe.
    {
        static std::mutex stub_mutex;
        static std::unordered_set<uint32_t> stub_seen;
        std::lock_guard<std::mutex> lock(stub_mutex);
        if (stub_seen.insert(vram).second) {
            FILE* f = fopen("hh_stub.log", "a");
            if (f != nullptr) {
                fprintf(f, "[STUB] vram=0x%08X\n", vram);
                fflush(f);
                fclose(f);
            }
        }
    }
}

// HH: handler de `syscall` (N64Recomp emite `recomp_syscall_handler`). El modulo 56 (fichero 57,
// combate) contiene palabras que el recompilador decodifica como `syscall` (probablemente datos
// mal acotados). Stub: registra y continua. Si aparecen syscalls REALES en ejecucion, implementar.
extern "C" void recomp_syscall_handler(uint8_t* rdram, recomp_context* ctx, int32_t instruction_vram) {
    (void)rdram;
    (void)ctx;
    static std::unordered_set<uint32_t> seen;
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    if (seen.insert((uint32_t)instruction_vram).second) {
        fprintf(stderr, "[SYSCALL] unhandled syscall at vram 0x%08X (ignored)\n", (uint32_t)instruction_vram);
    }
}

std::string current_game_mode_id;
std::optional<std::u8string> current_game = std::nullopt;
std::atomic<GameStatus> game_status = GameStatus::None;

// HH: contexto de registros MIPS del hilo de juego en ejecucion (para volcados de crash).
static thread_local recomp_context* hh_current_ctx = nullptr;

extern "C" recomp_context* hh_get_current_ctx() {
    return hh_current_ctx;
}

// HH: sp (r29) del contexto del hilo actual, o 0 si no hay. Para diagnosticos sin exponer el tipo.
extern "C" uint32_t hh_current_sp() {
    return hh_current_ctx != nullptr ? (uint32_t)hh_current_ctx->r29 : 0;
}

// HH: ra (r31) del contexto del hilo actual, o 0. Para saber quien llama (p.ej. a osSendMesg con una
// cola corrupta).
extern "C" uint32_t hh_current_ra() {
    return hh_current_ctx != nullptr ? (uint32_t)hh_current_ctx->r31 : 0;
}

// HH: registro de los contextos de todos los hilos de juego vivos. El watchdog de cuelgue del
// port lo usa para volcar donde esta atascado cada hilo (RA/SP = punto de bloqueo).
// HH: anillo de las ultimas llamadas guest por hilo (para el watchdog: localizar bucles que no
// ceden). En este port todas las llamadas pasan por get_function (use_lookup_for_all_function_calls).
struct HhCallRing {
    std::atomic<uint32_t> entries[16];
    std::atomic<uint32_t> n{0};
};
struct HhCtxSlot {
    std::atomic<recomp_context*> ctx{nullptr};
    std::atomic<int> tid{-1};
    std::atomic<uintptr_t> thr{0};
    HhCallRing ring;
};
static HhCtxSlot hh_ctx_slots[32];
static thread_local HhCallRing* hh_my_ring = nullptr;

extern "C" void hh_callring_record(uint32_t addr) {
    if (hh_my_ring == nullptr) return;
    uint32_t n = hh_my_ring->n.load(std::memory_order_relaxed);
    hh_my_ring->entries[n & 15].store(addr, std::memory_order_relaxed);
    hh_my_ring->n.store(n + 1, std::memory_order_relaxed);
}

extern "C" int hh_get_callring(recomp_context* c, uint32_t* out, int max) {
    for (auto& slot : hh_ctx_slots) {
        if (slot.ctx.load() == c) {
            uint32_t n = slot.ring.n.load();
            int cnt = (int)std::min<uint32_t>(n, 16);
            for (int i = 0; i < cnt && i < max; i++) {
                out[i] = slot.ring.entries[(n - cnt + i) & 15].load();
            }
            return cnt;
        }
    }
    return 0;
}

static void hh_register_ctx(recomp_context* ctx, int tid, uintptr_t thr) {
    for (auto& slot : hh_ctx_slots) {
        recomp_context* expected = nullptr;
        if (slot.ctx.compare_exchange_strong(expected, ctx)) {
            slot.tid.store(tid);
            slot.thr.store(thr);
            hh_my_ring = &slot.ring;
            return;
        }
    }
}

static void hh_unregister_ctx(recomp_context* ctx) {
    for (auto& slot : hh_ctx_slots) {
        recomp_context* expected = ctx;
        if (slot.ctx.compare_exchange_strong(expected, nullptr)) {
            slot.tid.store(-1);
            slot.thr.store(0);
            if (hh_my_ring == &slot.ring) hh_my_ring = nullptr;
            return;
        }
    }
}

extern "C" int hh_get_thread_ctxs(recomp_context** out, int max) {
    int n = 0;
    for (auto& slot : hh_ctx_slots) {
        recomp_context* c = slot.ctx.load();
        if (c != nullptr && n < max) {
            out[n++] = c;
        }
    }
    return n;
}

// HH: como hh_get_thread_ctxs pero devuelve tambien el id y el puntero de OSThread (watchdog).
extern "C" int hh_get_thread_ctxs_tids(recomp_context** out, int* tids, int max) {
    int n = 0;
    for (auto& slot : hh_ctx_slots) {
        recomp_context* c = slot.ctx.load();
        if (c != nullptr && n < max) {
            out[n] = c;
            tids[n] = slot.tid.load();
            n++;
        }
    }
    return n;
}

extern "C" int hh_get_thread_ctxs_full(recomp_context** out, int* tids, uintptr_t* thrs, int max) {
    int n = 0;
    for (auto& slot : hh_ctx_slots) {
        recomp_context* c = slot.ctx.load();
        if (c != nullptr && n < max) {
            out[n] = c;
            tids[n] = slot.tid.load();
            thrs[n] = slot.thr.load();
            n++;
        }
    }
    return n;
}

void run_thread_function(uint8_t* rdram, uint64_t addr, uint64_t sp, uint64_t arg) {
    HH_LOG("[RT] thread addr=0x%llx sp=0x%llx arg=0x%llx rdram=%p\n",
        (unsigned long long)addr, (unsigned long long)sp, (unsigned long long)arg, (void*)rdram);
    auto find_it = game_roms.find(current_game.value());
    const recomp::GameEntry& game_entry = find_it->second;
    
    recomp_context ctx{};
    ctx.r29 = sp;
    ctx.r4 = arg;
    ctx.mips3_float_mode = 0;
    ctx.f_odd = &ctx.f0.u32h;

    if (game_entry.thread_create_callback != nullptr) {
        game_entry.thread_create_callback(rdram, &ctx);
    }

    recomp_func_t* func = get_function(addr);
    hh_current_ctx = &ctx;
    hh_register_ctx(&ctx, (int)hh_sh_get_id(TO_PTR(OSThread, ultramodern::this_thread())), (uintptr_t)TO_PTR(OSThread, ultramodern::this_thread()));
    func(rdram, &ctx);
    hh_unregister_ctx(&ctx);
    hh_current_ctx = nullptr;
}

// HH: watchpoint de accesos de codigo recompilado (ver recomp.h). Config: HH_WATCH_ADDR=0x...
// Loguea a hh_watch.log (acotado) la direccion accedida, el ra guest del contexto actual y la
// direccion de retorno host (mapeable con el .map). Sirve para cazar quien pisa una direccion.
extern "C" int hh_watch_active = 0;
extern "C" unsigned int hh_watch_lo = 0;
extern "C" unsigned int hh_watch_hi = 0;
extern "C" uint8_t* hh_get_rdram_base(void);  // definido en events.cpp (valor actual en el watch)

#if defined(_MSC_VER)
#include <intrin.h>
static void* hh_watch_ret() { return _ReturnAddress(); }
#else
static void* hh_watch_ret() { return __builtin_return_address(0); }
#endif

// Base del modulo (para traducir direcciones host con el .map).
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
static uintptr_t hh_module_base() {
    HMODULE hm = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&hh_watch_ret), &hm) && hm != nullptr) {
        return reinterpret_cast<uintptr_t>(hm);
    }
    return 0;
}
#else
#include <dlfcn.h>
static uintptr_t hh_module_base() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&hh_watch_ret), &info) != 0) {
        return reinterpret_cast<uintptr_t>(info.dli_fbase);
    }
    return 0;
}
#endif

static void hh_watch_init() {
    const char* a = getenv("HH_WATCH_ADDR");
    if (a == nullptr || *a == '\0') return;
    unsigned long v = strtoul(a, nullptr, 0);
    hh_watch_lo = (unsigned int)(v & 0x1FFFFFFFu);
    // HH_WATCH_SIZE permite vigilar una sola palabra (p. ej. 4) para no saturar el log.
    unsigned long sz = 0x40;
    if (const char* s = getenv("HH_WATCH_SIZE")) {
        if (*s != '\0') sz = strtoul(s, nullptr, 0);
    }
    if (sz < 4) sz = 4;
    hh_watch_hi = hh_watch_lo + (unsigned int)sz;
    hh_watch_active = 1;
    fprintf(stderr, "[WATCH] vigilando RDRAM 0x%08X..0x%08X\n", hh_watch_lo, hh_watch_hi);
}

static void hh_watch_log(const char* what, uint64_t off, uint32_t extra) {
    static FILE* f = nullptr;
    static long n = 0;
    static std::chrono::steady_clock::time_point t0{};
    if (f == nullptr) {
        f = fopen("hh_watch.log", "w");
        if (f == nullptr) { hh_watch_active = 0; return; }
        t0 = std::chrono::steady_clock::now();
    }
    if (n >= 500000) { hh_watch_active = 0; return; }
    n++;
    recomp_context* c = hh_get_current_ctx();
    uint32_t ra = (c != nullptr) ? (uint32_t)c->r31 : 0;
    uint32_t sp = (c != nullptr) ? (uint32_t)c->r29 : 0;
    uint32_t a0 = (c != nullptr) ? (uint32_t)c->r4 : 0;
    uint32_t a1 = (c != nullptr) ? (uint32_t)c->r5 : 0;
    uint32_t a2 = (c != nullptr) ? (uint32_t)c->r6 : 0;
    uint32_t a3 = (c != nullptr) ? (uint32_t)c->r7 : 0;
    static uintptr_t hh_base = 0;
    if (hh_base == 0) hh_base = hh_module_base();
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    void* ret = hh_watch_ret();
    unsigned long long ret_off = (hh_base != 0) ? (unsigned long long)((uintptr_t)ret - hh_base) : 0;
    // HH: valor ACTUAL en esa direccion (antes del acceso). Al cambiar a basura, la entrada justo
    // anterior en el log identifica quien la escribio.
    uint32_t val = 0;
    uint8_t* rb = hh_get_rdram_base();
    if (rb != nullptr) {
        unsigned int o = (unsigned int)off & 0x1FFFFFFFu;
        if (o + 4 <= 0x40000000u) val = *(uint32_t*)(rb + o);
    }
    // HH: cuando el watchpoint ve el veneno 0xFFFF84CD/0xFF7F84CD, volcar la PILA GUEST completa
    // (cadena de retornos: revela quien llama a FUN_800058dc) y el anillo de llamadas. Gated por
    // HH_WATCH_VENOM=1 para no ensuciar. Ver notes/2026-09-18-diferencial-*.md.
    if (getenv("HH_WATCH_VENOM") != nullptr && rb != nullptr && c != nullptr &&
        (a1 == 0xFFFF84CDu || a1 == 0xFF7F84CDu)) {
        static FILE* vf = nullptr;
        static int vn = 0;
        if (vf == nullptr) vf = fopen("hh_venom.log", "w");
        if (vf != nullptr && vn < 8) {
            vn++;
            uint32_t sp = (uint32_t)c->r29;
            fprintf(vf, "=== VENOM #%d t=%.3f off=%05llX val(prev)=%08X a0=%08X a1=%08X a2=%08X a3=%08X sp=%08X ===\n",
                    vn, t, (unsigned long long)off, val, a0, a1, a2, a3, sp);
            // Pila guest: palabras que parecen direcciones de codigo (RA guardadas) en [sp, sp+0x400).
            fprintf(vf, "  stack-RA:");
            int cnt = 0;
            for (uint32_t a = sp; a < sp + 0x400u && a + 4 <= 0x80800000u; a += 4) {
                uint32_t v = *(uint32_t*)(rb + (a - 0x80000000u));
                if (v >= 0x80000400u && v < 0x80800000u) {
                    if ((cnt % 5) == 0) fprintf(vf, "\n    +%03X:", a - sp);
                    fprintf(vf, " %08X", v);
                    if (++cnt >= 40) break;
                }
            }
            fprintf(vf, "\n");
            uint32_t ring[16];
            int rn = hh_get_callring(c, ring, 16);
            if (rn > 0) {
                fprintf(vf, "  callring:");
                for (int k = 0; k < rn; k++) fprintf(vf, " %08X", ring[k]);
                fprintf(vf, "\n");
            }
            fflush(vf);
        }
    }
    fprintf(f, "[WATCH] t=%.3f %s off=%05llX val=%08X ra=%08X sp=%08X a0=%08X a1=%08X a2=%08X a3=%08X extra=%08X ret=%p (exe+0x%llX)\n",
            t, what, (unsigned long long)off, val, ra, sp, a0, a1, a2, a3, extra, ret, ret_off);
#if defined(_WIN32)
    {
        void* frames[16];
        USHORT nf = CaptureStackBackTrace(0, 16, frames, nullptr);
        unsigned long long offs[16];
        for (USHORT i = 0; i < nf && i < 16; i++) {
            offs[i] = (hh_base != 0) ? (unsigned long long)((uintptr_t)frames[i] - hh_base) : 0;
        }
        fprintf(f, "         bt:");
        for (USHORT i = 0; i < nf && i < 16; i++) fprintf(f, " 0x%llX", offs[i]);
        fprintf(f, "\n");
    }
#endif
    fflush(f);
}

// HH: ring buffer de las ultimas llamadas (target + sp) para volcar la secuencia que hunde la pila.
// Contadores uint64_t: get_function se llama ~43M/s; un `int` desborda en ~50s y `% 96` con un
// negativo indexa fuera del array (SEGV en hh_ring2_record+0x51, visto 2026-09-18 con watchpoint).
struct HhCallRec { uint32_t target; uint32_t sp; };
static thread_local HhCallRec hh_ring[96];
static thread_local uint64_t hh_ring_n = 0;
extern "C" void hh_ring_record(uint32_t target, uint32_t sp) {
    hh_ring[hh_ring_n % 96] = { target, sp };
    hh_ring_n++;
}
extern "C" void hh_ring_dump(FILE* f) {
    uint64_t start = hh_ring_n > 96 ? hh_ring_n - 96 : 0;
    for (uint64_t i = start; i < hh_ring_n; i++) {
        const HhCallRec& r = hh_ring[i % 96];
        fprintf(f, "   call -> 0x%08X sp=%08X\n", r.target, r.sp);
    }
}

// HH: ring grande (por hilo) de llamadas para capturar un frame entero y localizar la fuga de pila.
static const int HH_RING2_SIZE = 1 << 16;
static thread_local HhCallRec hh_ring2[HH_RING2_SIZE];
static thread_local uint64_t hh_ring2_n = 0;
extern "C" void hh_ring2_record(uint32_t target, uint32_t sp) {
    hh_ring2[hh_ring2_n % HH_RING2_SIZE] = { target, sp };
    hh_ring2_n++;
}
extern "C" int hh_ring2_count() { return (int)(hh_ring2_n % HH_RING2_SIZE); }
extern "C" void hh_ring2_dump_last(FILE* f, int count) {
    if (count < 0) count = 0;
    if ((uint64_t)count > hh_ring2_n) count = (int)(hh_ring2_n % HH_RING2_SIZE);
    if (count > HH_RING2_SIZE) count = HH_RING2_SIZE;
    uint64_t start = hh_ring2_n - (uint64_t)count;
    for (uint64_t i = start; i < hh_ring2_n; i++) {
        const HhCallRec& r = hh_ring2[i % HH_RING2_SIZE];
        fprintf(f, "0x%08X %08X\n", r.target, r.sp);
    }
}

extern "C" void hh_watch_hit(uint64_t off) {
    // HH: la primera vez que la pila entra en la zona del struct, volcar las ultimas llamadas.
    recomp_context* c = hh_get_current_ctx();
    static bool dumped = false;
    if (!dumped && c != nullptr && (uint32_t)c->r29 != 0 && (uint32_t)c->r29 < 0x8005A000u) {
        dumped = true;
        FILE* f = fopen("hh_ring.log", "w");
        if (f != nullptr) {
            fprintf(f, "=== HH ring de llamadas (sp=%08X, off=%05llX) ===\n",
                    (uint32_t)c->r29, (unsigned long long)off);
            hh_ring_dump(f);
            fflush(f);
            fclose(f);
        }
    }
    hh_watch_log("mem", off, 0);
}

extern "C" void hh_watch_dma(uint32_t dram, uint32_t size) {
    if (hh_watch_active == 0) return;
    uint32_t lo = dram & 0x1FFFFFFFu;
    uint32_t hi = lo + size;
    if (hi > hh_watch_lo && lo < hh_watch_hi) {
        hh_watch_log("dma", lo, size);
    }
}

#if HH_DEBUG_TOOLS
// =====================================================================================
// HH: capa de debug "quien corrompe" (port nativo).
//   HH_CANARY=0xADDR:SIZE[,0xADDR:SIZE...]  vigila el buffer RDRAM por frame (comparacion
//                                           contra sombra) y caza escrituras que NO pasan
//                                           por los MEM_* (runtime/do_send/DMA).
//   HH_DRWATCH=0xADDR[,0xADDR...]           watchpoint de HARDWARE (solo Windows): para en la
//                                           instruccion exacta que escribe esos 4 bytes.
//   hh_direct_write(...) lo llaman las escrituras directas del runtime (do_send/do_recv).
// Salidas: hh_canary.log (canary + escrituras directas) y hh_drwatch.log (hardware) junto al exe.
// =====================================================================================

extern "C" uint64_t hh_get_vi_count(void);
extern "C" int hh_get_callring(recomp_context* c, uint32_t* out, int max);

struct HhCanaryRange {
    uint32_t addr;      // direccion guest (base 0x80000000)
    uint32_t size;      // bytes
    uint32_t off;       // offset fisico en RDRAM
    uint8_t* shadow;    // copia del frame anterior
};

static HhCanaryRange hh_canary[4];
static int hh_canary_n = 0;
static int hh_canary_inited = 0;
static FILE* hh_canary_fp = nullptr;
static long hh_canary_lines = 0;

static FILE* hh_canary_file() {
    if (hh_canary_fp == nullptr) hh_canary_fp = fopen("hh_canary.log", "w");
    return hh_canary_fp;
}

// Volca el anillo de las ultimas 16 llamadas guest de cada hilo de juego vivo. Da la ventana
// (funciones) en la que ocurrio el cambio.
static void hh_dump_thread_rings(FILE* f) {
    for (auto& slot : hh_ctx_slots) {
        recomp_context* c = slot.ctx.load();
        if (c == nullptr) continue;
        uint32_t ring[16];
        int n = hh_get_callring(c, ring, 16);
        fprintf(f, "      tid=%d sp=%08X last:", slot.tid.load(), (uint32_t)c->r29);
        for (int i = 0; i < n; i++) fprintf(f, " %08X", ring[i]);
        fprintf(f, "\n");
    }
}

// -------------------------------------------------------------------------------------
// HH: vigilancia del NODO de la lista de suscriptores del event-dispatch. El nodo que se corrompe
// (p.ej. 0x8005BF14, cola del bucle principal) vive en la PILA de un hilo, por lo que el watchpoint
// por acceso (HH_WATCH_ADDR) da SEGV. Aqui, al registrar un nodo con hh_nodewatch_set (desde el
// wrapper de FUN_80000934), se compara next/q una vez por VI y, si cambian, se vuelca la ventana de
// llamadas de TODOS los hilos en el instante de la deteccion (candidato a autor del pisado).
// -------------------------------------------------------------------------------------
struct HhNodeWatch { uint32_t addr; uint32_t next; uint32_t q; };
static HhNodeWatch hh_nodewatch[4];
static int hh_nodewatch_n = 0;
static FILE* hh_nodewatch_fp = nullptr;
static long hh_nodewatch_lines = 0;

extern "C" void hh_nodewatch_set(uint32_t addr) {
    if (addr == 0) return;
    for (int i = 0; i < hh_nodewatch_n; i++) if (hh_nodewatch[i].addr == addr) return;
    if (hh_nodewatch_n >= 4) return;
    uint8_t* rdram = hh_get_rdram_base();
    if (rdram == nullptr) return;
    uint32_t o = addr & 0x1FFFFFFFu;
    if (o + 8 > 0x800000u) return;
    HhNodeWatch& w = hh_nodewatch[hh_nodewatch_n++];
    w.addr = addr;
    w.next = *(uint32_t*)(rdram + o);
    w.q = *(uint32_t*)(rdram + o + 4);
    fprintf(stderr, "[NODEWATCH] vigilando node=%08X next=%08X q=%08X\n", addr, w.next, w.q);
}

static void hh_nodewatch_check(void) {
    if (hh_nodewatch_n == 0) return;
    uint8_t* rdram = hh_get_rdram_base();
    if (rdram == nullptr) return;
    uint64_t vi = hh_get_vi_count();
    for (int i = 0; i < hh_nodewatch_n; i++) {
        HhNodeWatch& w = hh_nodewatch[i];
        uint32_t o = w.addr & 0x1FFFFFFFu;
        if (o + 8 > 0x800000u) continue;
        uint32_t next = *(uint32_t*)(rdram + o);
        uint32_t q = *(uint32_t*)(rdram + o + 4);
        if (next == w.next && q == w.q) continue;
        if (hh_nodewatch_fp == nullptr) hh_nodewatch_fp = fopen("hh_nodewatch.log", "w");
        FILE* f = hh_nodewatch_fp;
        if (f != nullptr && hh_nodewatch_lines <= 50000) {
            hh_nodewatch_lines++;
            fprintf(f, "[NODEWATCH] vi=%llu node=%08X next=%08X->%08X q=%08X->%08X\n",
                    (unsigned long long)vi, w.addr, w.next, next, w.q, q);
            hh_dump_thread_rings(f);
            fflush(f);
        }
        w.next = next;
        w.q = q;
    }
}

static bool hh_watched_hit(uint32_t guest_addr, uint32_t len) {
    if (hh_canary_n == 0 && hh_watch_active == 0) return false;
    uint32_t lo = guest_addr & 0x1FFFFFFFu;
    uint32_t hi = lo + len;
    for (int i = 0; i < hh_canary_n; i++) {
        if (hi > hh_canary[i].off && lo < hh_canary[i].off + hh_canary[i].size) return true;
    }
    if (hh_watch_active && hi > hh_watch_lo && lo < hh_watch_hi) return true;
    return false;
}

// Escritura directa del runtime a RDRAM (do_send/do_recv/DMA). `guest_addr` = destino guest.
// Devuelve 1 si el destino cae en una zona vigilada.
extern "C" int hh_direct_write(uint32_t guest_addr, uint32_t val, const char* where) {
    if (!hh_watched_hit(guest_addr, 4)) return 0;
    FILE* f = hh_canary_file();
    if (f == nullptr || hh_canary_lines > 200000) return 1;
    hh_canary_lines++;
    recomp_context* c = hh_get_current_ctx();
    uint32_t ra = c != nullptr ? (uint32_t)c->r31 : 0;
    uint32_t sp = c != nullptr ? (uint32_t)c->r29 : 0;
    uintptr_t base = hh_module_base();
    void* ret = hh_watch_ret();
    unsigned long long ret_off = base != 0 ? (unsigned long long)((uintptr_t)ret - base) : 0;
    fprintf(f, "[WW] %s guest=%08X val=%08X ra=%08X sp=%08X ret=exe+0x%llX\n",
            where, guest_addr, val, ra, sp, ret_off);
    hh_dump_thread_rings(f);
    fflush(f);
    return 1;
}

extern "C" void hh_canary_init(void) {
    const char* spec = getenv("HH_CANARY");
    if (spec == nullptr || *spec == '\0') return;
    char buf[512];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (char* tok = strtok(buf, ","); tok != nullptr && hh_canary_n < 4; tok = strtok(nullptr, ",")) {
        char* colon = strchr(tok, ':');
        uint32_t size = 4;
        if (colon != nullptr) { *colon = 0; size = (uint32_t)strtoul(colon + 1, nullptr, 16); }
        uint32_t addr = (uint32_t)strtoul(tok, nullptr, 16);
        if (size == 0) size = 4;
        hh_canary[hh_canary_n].addr = addr;
        hh_canary[hh_canary_n].size = size;
        hh_canary[hh_canary_n].off = addr & 0x1FFFFFFFu;
        hh_canary[hh_canary_n].shadow = nullptr;
        hh_canary_n++;
    }
}

// Llamado una vez por VI desde el hilo VI (events.cpp). Compara las regiones vigiladas word a
// word contra la sombra y loguea los cambios con la ventana de llamadas de cada hilo.
extern "C" void hh_canary_tick(void) {
    hh_nodewatch_check();
    if (hh_canary_n == 0) return;
    uint8_t* rdram = hh_get_rdram_base();
    if (rdram == nullptr) return;
    if (!hh_canary_inited) {
        hh_canary_inited = 1;
        for (int i = 0; i < hh_canary_n; i++) {
            hh_canary[i].shadow = (uint8_t*)malloc(hh_canary[i].size);
            if (hh_canary[i].shadow != nullptr) {
                memcpy(hh_canary[i].shadow, rdram + hh_canary[i].off, hh_canary[i].size);
            }
        }
        fprintf(stderr, "[CANARY] vigilando %d rango(s)\n", hh_canary_n);
        return;
    }
    static uint64_t last_vi = 0;
    uint64_t vi = hh_get_vi_count();
    if (vi == last_vi) return;
    last_vi = vi;
    for (int i = 0; i < hh_canary_n; i++) {
        if (hh_canary[i].shadow == nullptr) continue;
        uint8_t* cur = rdram + hh_canary[i].off;
        for (uint32_t j = 0; j + 4 <= hh_canary[i].size; j += 4) {
            uint32_t oldv, newv;
            memcpy(&oldv, hh_canary[i].shadow + j, 4);
            memcpy(&newv, cur + j, 4);
            if (oldv == newv) continue;
            FILE* f = hh_canary_file();
            if (f != nullptr && hh_canary_lines <= 200000) {
                hh_canary_lines++;
                fprintf(f, "[CANARY] vi=%llu guest=%08X old=%08X new=%08X\n",
                        (unsigned long long)vi, hh_canary[i].addr + j, oldv, newv);
                hh_dump_thread_rings(f);
                fflush(f);
            }
            memcpy(hh_canary[i].shadow + j, cur + j, 4);
        }
    }
}

// -------------------------------------------------------------------------------------
// HH: traza por funcion guest (HH_TRACE=0xADDR:etiqueta[,0xADDR:etiqueta...]). Se llama desde
// get_function; vuelca args, ra/sp y, si procede, el estado del objeto de combate y los globales.
// Sirve para ver la secuencia exacta del state machine (armer, modo, indice de tabla, setter...).
// -------------------------------------------------------------------------------------
struct HhTraceEntry { uint32_t addr; char label[32]; };
static HhTraceEntry hh_trace_fn_list[16];
static int hh_trace_fn_n = 0;
struct HhTraceRange { uint32_t base; uint32_t size; };
static HhTraceRange hh_trace_ranges[4];
static int hh_trace_range_n = 0;
static FILE* hh_trace_fp = nullptr;
static long hh_trace_lines = 0;

// HH: HH_DIAG=1 habilita los logs de diagnostico always-on (hh_sched/hh_pi/hh_mq/hh_cmds/hh_ovl/
// hh_rsp). Por defecto OFF: escribirlos con fflush en cada evento degrada el pacing del juego, sobre
// todo en filesystems lentos (9p/red). hh_state, hh_crash/hh_hang y los volcados de error siguen
// siempre activos. Los logs opt-in (HH_TRACE, HH_WAITLOG, HH_MQLOG_ALL, HH_WATCH...) no cambian.
extern "C" int hh_diag_enabled(void) {
    static const int on = getenv("HH_DIAG") != nullptr ? 1 : 0;
    return on;
}

// HH: RA del contexto guest en ejecucion (para instrumentar cesiones/bloqueos desde ultramodern).
extern "C" uint32_t hh_guest_ra(void) {
    recomp_context* c = hh_get_current_ctx();
    return c ? (uint32_t)c->r31 : 0;
}

// HH: reloj monotono compartido por toda la instrumentacion (origen = primer uso). Permite
// correlacionar trazas de subsistemas distintos ([TRACE], [GATE], [GATE2], [SUBM], [SCH]).
extern "C" double hh_time_now(void) {
    static std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

extern "C" void hh_trace_init(void) {
    const char* spec = getenv("HH_TRACE");
    if (spec != nullptr && *spec != '\0') {
        char buf[512];
        strncpy(buf, spec, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        for (char* tok = strtok(buf, ","); tok != nullptr && hh_trace_fn_n < 16; tok = strtok(nullptr, ",")) {
            char* colon = strchr(tok, ':');
            if (colon == nullptr) continue;
            *colon = 0;
            hh_trace_fn_list[hh_trace_fn_n].addr = (uint32_t)strtoul(tok, nullptr, 16);
            strncpy(hh_trace_fn_list[hh_trace_fn_n].label, colon + 1, sizeof(hh_trace_fn_list[0].label) - 1);
            hh_trace_fn_list[hh_trace_fn_n].label[sizeof(hh_trace_fn_list[0].label) - 1] = 0;
            hh_trace_fn_n++;
        }
    }
    // HH_TRACE_RANGE=0xBASE:0xSIZE[,0xBASE:0xSIZE...] -> loguea CUALQUIER llamada dentro del rango
    // (util para volcar toda la actividad de un modulo/overlay en una pasada).
    const char* rspec = getenv("HH_TRACE_RANGE");
    if (rspec != nullptr && *rspec != '\0') {
        char buf[512];
        strncpy(buf, rspec, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = 0;
        for (char* tok = strtok(buf, ","); tok != nullptr && hh_trace_range_n < 4; tok = strtok(nullptr, ",")) {
            char* colon = strchr(tok, ':');
            if (colon == nullptr) continue;
            *colon = 0;
            hh_trace_ranges[hh_trace_range_n].base = (uint32_t)strtoul(tok, nullptr, 16);
            hh_trace_ranges[hh_trace_range_n].size = (uint32_t)strtoul(colon + 1, nullptr, 16);
            if (hh_trace_ranges[hh_trace_range_n].size == 0) hh_trace_ranges[hh_trace_range_n].size = 0x10000;
            hh_trace_range_n++;
        }
    }
    if (hh_trace_fn_n > 0 || hh_trace_range_n > 0) {
        fprintf(stderr, "[TRACE] %d funcion(es) + %d rango(s) vigilados\n", hh_trace_fn_n, hh_trace_range_n);
    }
}

extern "C" void hh_trace_fn(uint32_t addr) {
    if (hh_trace_fn_n == 0 && hh_trace_range_n == 0) return;
    char rng_label[32];
    const char* label = nullptr;
    for (int i = 0; i < hh_trace_fn_n; i++) {
        if (hh_trace_fn_list[i].addr == addr) { label = hh_trace_fn_list[i].label; break; }
    }
    if (label == nullptr) {
        for (int i = 0; i < hh_trace_range_n; i++) {
            uint32_t off = addr - hh_trace_ranges[i].base;
            if (off < hh_trace_ranges[i].size) {
                snprintf(rng_label, sizeof(rng_label), "RANGE+%X", off);
                label = rng_label;
                break;
            }
        }
    }
    if (label == nullptr) return;
    if (hh_trace_fp == nullptr) hh_trace_fp = fopen("hh_trace.log", "w");
    if (hh_trace_fp == nullptr || hh_trace_lines > 2000000) return;
    hh_trace_lines++;
    uint8_t* r = hh_get_rdram_base();
    recomp_context* c = hh_get_current_ctx();
    const double hh_t = hh_time_now();
    const int hh_tid = (r != nullptr && ultramodern::is_game_thread())
                           ? (int)hh_sh_get_id(
                                 (OSThread*)(r + hh_to_ptr_off((uint32_t)ultramodern::this_thread())))
                           : -1;
    uint32_t a0 = c ? (uint32_t)c->r4 : 0, a1 = c ? (uint32_t)c->r5 : 0;
    uint32_t a2 = c ? (uint32_t)c->r6 : 0, a3 = c ? (uint32_t)c->r7 : 0;
    uint32_t ra = c ? (uint32_t)c->r31 : 0, sp = c ? (uint32_t)c->r29 : 0;
    if (r != nullptr) {
        auto wp = [&](uint32_t a) -> uint32_t {
            uint32_t o = a & 0x1FFFFFFFu;
            return (o + 4 <= 0x800000u) ? *(uint32_t*)(r + o) : 0;
        };
        auto hp = [&](uint32_t a) -> uint16_t {
            uint32_t o = (a ^ 2) & 0x1FFFFFFFu;
            return (o + 2 <= 0x800000u) ? *(uint16_t*)(r + o) : 0;
        };
        auto hb = [&](uint32_t a) -> uint8_t {
            uint32_t o = (a ^ 3) & 0x1FFFFFFFu;
            return (o < 0x800000u) ? r[o] : 0;
        };
        // Estado del OBJETO REAL de la llamada (a0), dinamico (la direccion varia por partida).
        bool ok = (a0 >= 0x80000000u && a0 + 0x90u <= 0x80800000u && (a0 & 3u) == 0);
        auto wo = [&](uint32_t off) -> uint32_t { return ok ? wp(a0 + off) : 0; };
        auto ho = [&](uint32_t off) -> uint16_t { return ok ? hp(a0 + off) : 0; };
        fprintf(hh_trace_fp,
                "[TRACE] t=%.3f tid=%d %-10s addr=%08X a0=%08X a1=%08X ra=%08X sp=%08X"
                " | obj: 1C=%08X 20=%08X 24=%08X 2C=%08X 36=%04X 8C=%08X"
                " | mode=%04X gD0=%08X flag=%02X\n",
                hh_t, hh_tid, label, addr, a0, a1, ra, sp,
                wo(0x1C), wo(0x20), wo(0x24), wo(0x2C), ho(0x36), wo(0x8C),
                hp(0x801BBC1Cu), wp(0x801BBCD0u), hb(0x8017DD92u));
    } else {
        fprintf(hh_trace_fp, "[TRACE] t=%.3f tid=%d %-10s addr=%08X a0=%08X a1=%08X ra=%08X sp=%08X\n",
                hh_t, hh_tid, label, addr, a0, a1, ra, sp);
    }
#if defined(_WIN32)
    {
        uintptr_t base = hh_module_base();
        void* frames[4];
        USHORT nf = CaptureStackBackTrace(0, 4, frames, nullptr);
        fprintf(hh_trace_fp, "      bt:");
        for (USHORT i = 0; i < nf; i++) {
            fprintf(hh_trace_fp, " 0x%llX", (unsigned long long)((uintptr_t)frames[i] - base));
        }
        fprintf(hh_trace_fp, "\n");
    }
#endif
    fflush(hh_trace_fp);
}

#if defined(_WIN32)
#include <tlhelp32.h>

// Watchpoint de hardware: DR0-DR3 apuntan a las palabras vigiladas (RW=escritura, 4 bytes).
static uintptr_t hh_dr_addrs[4] = {};
static int hh_dr_n = 0;
static FILE* hh_dr_fp = nullptr;
static long hh_dr_lines = 0;
static uint32_t hh_dr_last[4] = {};
static std::mutex hh_dr_mutex;
static std::vector<DWORD> hh_dr_armed;

static void hh_dr_arm(HANDLE th) {
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (!GetThreadContext(th, &c)) return;
    for (int i = 0; i < hh_dr_n; i++) (&c.Dr0)[i] = (DWORD64)hh_dr_addrs[i];
    DWORD64 dr7 = 0x100 | 0x200;  // LE | GE (reporte exacto)
    for (int i = 0; i < hh_dr_n; i++) {
        dr7 |= (DWORD64)1 << (2 * i);            // L_i (local enable)
        dr7 |= (DWORD64)0xD << (16 + 4 * i);     // RW=01 (write) + LEN=11 (4 bytes) -> bits 16..19
    }
    c.Dr7 = dr7;
    SetThreadContext(th, &c);
}

static LONG CALLBACK hh_dr_veh(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    if (hh_dr_n == 0) return EXCEPTION_CONTINUE_SEARCH;
    static thread_local int in_handler = 0;
    if (in_handler) return EXCEPTION_CONTINUE_EXECUTION;
    in_handler = 1;
    // IMPORTANTE: limpiar Dr6 SIEMPRE y poner RF; si no, el single-step se re-dispara sin parar
    // (era la causa de las "escrituras" espurias atribuidas a hh_canary_tick).
    DWORD64 dr6 = ep->ContextRecord->Dr6;
    ep->ContextRecord->Dr6 = 0;
    ep->ContextRecord->EFlags |= 0x10000;  // RF (resume flag)
    int slot = -1;
    for (int i = 0; i < hh_dr_n; i++) {
        if (dr6 & ((DWORD64)1 << i)) { slot = i; break; }
    }
    if (slot >= 0) {
        uint8_t* rdram = hh_get_rdram_base();
        uint32_t newv = 0;
        if (rdram != nullptr) memcpy(&newv, (void*)hh_dr_addrs[slot], 4);
        // Solo registrar cambios de valor (las escrituras del mismo valor son ruido).
        if (newv != hh_dr_last[slot]) {
            if (hh_dr_fp == nullptr) hh_dr_fp = fopen("hh_drwatch.log", "w");
            if (hh_dr_fp != nullptr && hh_dr_lines <= 200000) {
                hh_dr_lines++;
                recomp_context* gc = hh_get_current_ctx();
                uintptr_t base = hh_module_base();
                void* frames[24];
                USHORT nf = CaptureStackBackTrace(0, 24, frames, nullptr);
                uint32_t guest = (uint32_t)((uintptr_t)hh_dr_addrs[slot] - (uintptr_t)rdram) | 0x80000000u;
                fprintf(hh_dr_fp, "[DR] slot=%d guest=%08X old=%08X new=%08X tid=%lu rip=exe+0x%llX\n",
                        slot, guest, hh_dr_last[slot], newv, (unsigned long)GetCurrentThreadId(),
                        (unsigned long long)((uintptr_t)ep->ContextRecord->Rip - base));
                fprintf(hh_dr_fp, "     bt:");
                for (USHORT i = 0; i < nf; i++) {
                    fprintf(hh_dr_fp, " 0x%llX", (unsigned long long)((uintptr_t)frames[i] - base));
                }
                fprintf(hh_dr_fp, "\n");
                if (gc != nullptr) {
                    fprintf(hh_dr_fp, "     guest ra=%08X sp=%08X s0=%08X a0=%08X a1=%08X a2=%08X a3=%08X\n",
                            (uint32_t)gc->r31, (uint32_t)gc->r29, (uint32_t)gc->r16,
                            (uint32_t)gc->r4, (uint32_t)gc->r5, (uint32_t)gc->r6, (uint32_t)gc->r7);
                }
                hh_dump_thread_rings(hh_dr_fp);
                static int dumped = 0;
                if (!dumped && rdram != nullptr) {
                    dumped = 1;
                    FILE* df = fopen("hh_drwatch_rdram.bin", "wb");
                    if (df != nullptr) { fwrite(rdram, 1, 0x800000, df); fclose(df); }
                }
                fflush(hh_dr_fp);
            }
        }
        hh_dr_last[slot] = newv;
    }
    in_handler = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Arma los DR en los hilos que aparezcan despues (el runtime crea hilos nuevos).
static void hh_dr_watch_thread() {
    for (;;) {
        Sleep(250);
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) continue;
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                {
                    std::lock_guard<std::mutex> lock(hh_dr_mutex);
                    bool found = false;
                    for (DWORD id : hh_dr_armed) { if (id == te.th32ThreadID) { found = true; break; } }
                    if (found) continue;
                }
                HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                                       FALSE, te.th32ThreadID);
                if (th != nullptr) {
                    SuspendThread(th);
                    hh_dr_arm(th);
                    ResumeThread(th);
                    CloseHandle(th);
                    std::lock_guard<std::mutex> lock(hh_dr_mutex);
                    hh_dr_armed.push_back(te.th32ThreadID);
                }
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
    }
}

extern "C" void hh_drwatch_init(uint8_t* rdram) {
    const char* spec = getenv("HH_DRWATCH");
    // Preferir la base viva (la que usan los accesos); init() recibe la misma, pero por si acaso.
    if (uint8_t* rb = hh_get_rdram_base()) rdram = rb;
    if (spec == nullptr || *spec == '\0' || rdram == nullptr) return;
    char buf[256];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (char* tok = strtok(buf, ","); tok != nullptr && hh_dr_n < 4; tok = strtok(nullptr, ",")) {
        uint32_t addr = (uint32_t)strtoul(tok, nullptr, 16);
        if ((addr & 3u) != 0) continue;
        hh_dr_addrs[hh_dr_n] = (uintptr_t)(rdram + (addr & 0x1FFFFFFFu));
        memcpy(&hh_dr_last[hh_dr_n], (void*)hh_dr_addrs[hh_dr_n], 4);
        hh_dr_n++;
    }
    if (hh_dr_n == 0) return;
    AddVectoredExceptionHandler(1, hh_dr_veh);
    hh_dr_arm(GetCurrentThread());
    {
        std::lock_guard<std::mutex> lock(hh_dr_mutex);
        hh_dr_armed.push_back(GetCurrentThreadId());
    }
    std::thread(hh_dr_watch_thread).detach();
    fprintf(stderr, "[DRWATCH] %d watchpoint(s) de hardware armados\n", hh_dr_n);
}
#else
extern "C" void hh_drwatch_init(uint8_t* rdram) { (void)rdram; }
#endif

#else  // !HH_DEBUG_TOOLS: stubs (release limpio, sin I/O ni consola)
extern "C" int hh_direct_write(uint32_t, uint32_t, const char*) { return 0; }
extern "C" void hh_canary_init(void) {}
extern "C" void hh_canary_tick(void) {}
extern "C" void hh_nodewatch_set(uint32_t) {}
extern "C" void hh_drwatch_init(uint8_t*) {}
extern "C" void hh_trace_init(void) {}
extern "C" void hh_trace_fn(uint32_t) {}
#endif  // HH_DEBUG_TOOLS

void init(uint8_t* rdram, recomp_context* ctx, gpr entrypoint) {
    hh_watch_init();
    hh_canary_init();
    hh_drwatch_init(rdram);
    hh_trace_init();
    // Initialize the overlays
    recomp::overlays::init_overlays();
    init_mmio(rdram);

    // Register all flat (non-overlay) code sections at their absolute ram_addr. For flat code,
    // sections aren't entrypoint-relative, so they must be registered at their own ram_addr.
    recomp::overlays::register_flat_code();

    // Load overlays in the first 1MB
    load_overlays(0x1000, (int32_t)entrypoint, 1024 * 1024);

    // Initial 1MB DMA (rom address 0x1000 = physical address 0x10001000)
    recomp::do_rom_read(rdram, entrypoint, 0x10001000, 0x100000);

    // Read in any extra data from patches
    recomp::overlays::read_patch_data(rdram, (gpr)recomp::patch_rdram_start);

    // Set up context floats
    ctx->f_odd = &ctx->f0.u32h;
    ctx->mips3_float_mode = false;

    // The game's main (FUN_80001078) uses $a0 as the base pointer of a boot scratch structure and
    // the recompiled boot stub never sets it, so provide a valid writable RAM address here.
    // Must be sign-extended (like the S32()/lui+addiu pattern the game uses) so the MEM_* address
    // macros map it into RDRAM correctly. 0x800E5F40 is the end of the flat main code section.
    ctx->r4 = (gpr)(int32_t)0x800E5F40;

    // Initialize variables normally set by IPL3
    constexpr int32_t osTvType = 0x80000300;
    //constexpr int32_t osRomType = 0x80000304;
    constexpr int32_t osRomBase = 0x80000308;
    constexpr int32_t osResetType = 0x8000030c;
    //constexpr int32_t osCicId = 0x80000310;
    //constexpr int32_t osVersion = 0x80000314;
    constexpr int32_t osMemSize = 0x80000318;
    //constexpr int32_t osAppNMIBuffer = 0x8000031c;
    MEM_W(osTvType, 0) = 1; // NTSC
    MEM_W(osRomBase, 0) = 0xB0000000u; // standard rom base
    MEM_W(osResetType, 0) = 0; // cold reset
    MEM_W(osMemSize, 0) = 4 * 1024 * 1024; // 4MB (base retail machine; see osGetMemSize_recomp)
}

std::u8string recomp::current_game_id() {
    std::lock_guard<std::mutex> lock(current_game_mutex);
    return current_game.value();
};

std::string recomp::current_mod_game_id() {
    auto find_it = game_roms.find(current_game_id());
    const recomp::GameEntry& game_entry = find_it->second;

    return game_entry.mod_game_id;
}

void recomp::start_game(const std::u8string& game_id, const std::string& game_mode_id) {
    std::lock_guard<std::mutex> lock(current_game_mutex);
    current_game_mode_id = game_mode_id;
    current_game = game_id;
    game_status.store(GameStatus::Running);
    game_status.notify_all();
    mods::set_latest_game_mode_id(game_mode_id);
}

bool ultramodern::is_game_started() {
    return game_status.load() != GameStatus::None;
}

std::atomic_bool exited = false;
moodycamel::LightweightSemaphore graphics_shutdown_ready;

void ultramodern::quit() {
    exited.store(true);
    GameStatus desired = GameStatus::None;
    game_status.compare_exchange_strong(desired, GameStatus::Quit);
    game_status.notify_all();
    std::lock_guard<std::mutex> lock(current_game_mutex);
    current_game.reset();
}

void recomp::mods::enable_mod(const std::string& mod_id, bool enabled) {
    std::lock_guard lock { mod_context_mutex };
    return mod_context->enable_mod(mod_id, enabled, true);
}

bool recomp::mods::is_mod_enabled(const std::string& mod_id) {
    std::lock_guard lock { mod_context_mutex };
    return mod_context->is_mod_enabled(mod_id);
}

bool recomp::mods::is_mod_auto_enabled(const std::string& mod_id) {
    std::lock_guard lock{ mod_context_mutex };
    return mod_context->is_mod_auto_enabled(mod_id);
}

bool recomp::mods::is_mod_deprecated(const std::string& mod_id, const recomp::Version& mod_version) {
    std::lock_guard lock{ mod_context_mutex };
    return mod_context->is_mod_deprecated(mod_id, mod_version);
}

recomp::mods::DeprecationStatus recomp::mods::get_mod_deprecation_status(const std::string& mod_id) {
    std::lock_guard lock{ mod_context_mutex };
    return mod_context->get_mod_deprecation_status(mod_id);
}

recomp::Version recomp::mods::get_mod_deprecation_version(const std::string& mod_id) {
    std::lock_guard lock{ mod_context_mutex };
    return mod_context->get_mod_deprecation_version(mod_id);
}

std::string recomp::mods::deprecation_status_to_message(DeprecationStatus deprecation_status) {
    switch (deprecation_status) {
    case DeprecationStatus::Integrated:
        return "This mod has already been integrated into the game";
    case DeprecationStatus::BrokenVersion:
        return "This version of the mod is known to cause issues. Please update it";
    case DeprecationStatus::BrokenPermanent:
        return "This mod is known to cause issues. Please uninstall it";
    default:
        return "Reason is unknown";
    }
}

const recomp::config::ConfigSchema &recomp::mods::get_mod_config_schema(const std::string &mod_id) {
    std::lock_guard lock{ mod_context_mutex };
    return mod_context->get_mod_config_schema(mod_id);
}

recomp::config::Config *recomp::mods::get_mod_config(const std::string &mod_id) {
    std::lock_guard lock{ mod_context_mutex };
    return mod_context->get_mod_config(mod_id);
}

const std::vector<char> &recomp::mods::get_mod_thumbnail(const std::string &mod_id) {
    std::lock_guard lock{ mod_context_mutex };
    return mod_context->get_mod_thumbnail(mod_id);
}

void recomp::mods::set_mod_config_value(size_t mod_index, const std::string &option_id, const recomp::config::ConfigValueVariant &value) {
    std::lock_guard lock{ mod_context_mutex };
    return mod_context->set_mod_config_value(mod_index, option_id, value);
}

void recomp::mods::set_mod_config_value(const std::string &mod_id, const std::string &option_id, const recomp::config::ConfigValueVariant &value) {
    std::lock_guard lock{ mod_context_mutex };
    return mod_context->set_mod_config_value(mod_id, option_id, value);
}

recomp::config::ConfigValueVariant recomp::mods::get_mod_config_value(size_t mod_index, const std::string &option_id) {
    std::lock_guard lock{ mod_context_mutex };
    return mod_context->get_mod_config_value(mod_index, option_id);
}

recomp::config::ConfigValueVariant recomp::mods::get_mod_config_value(const std::string &mod_id, const std::string &option_id) {
    std::lock_guard lock{ mod_context_mutex };
    return mod_context->get_mod_config_value(mod_id, option_id);
}

std::string recomp::mods::get_latest_game_mode_id() {
    std::lock_guard lock{ mod_context_mutex };
    return mod_context->get_latest_game_mode_id();
}

void recomp::mods::set_latest_game_mode_id(const std::string& game_mode_id) {
    std::lock_guard lock{ mod_context_mutex };
    mod_context->set_latest_game_mode_id(game_mode_id);
}

std::string recomp::mods::get_mod_id_from_filename(const std::filesystem::path& mod_filename) {
    std::lock_guard lock { mod_context_mutex };
    return mod_context->get_mod_id_from_filename(mod_filename);
}

std::filesystem::path recomp::mods::get_mod_filename(const std::string& mod_id) {
    std::lock_guard lock { mod_context_mutex };
    return mod_context->get_mod_filename(mod_id);
}

size_t recomp::mods::get_mod_order_index(const std::string& mod_id) {
    std::lock_guard lock { mod_context_mutex };
    return mod_context->get_mod_order_index(mod_id);
}

size_t recomp::mods::get_mod_order_index(size_t mod_index) {
    std::lock_guard lock { mod_context_mutex };
    return mod_context->get_mod_order_index(mod_index);
}

std::optional<recomp::mods::ModDetails> recomp::mods::get_details_for_mod(const std::string& mod_id) {
    std::lock_guard lock { mod_context_mutex };
    return mod_context->get_details_for_mod(mod_id);
}

std::vector<recomp::mods::ModDetails> recomp::mods::get_all_mod_details(const std::string& mod_game_id) {
    std::lock_guard lock { mod_context_mutex };
    return mod_context->get_all_mod_details(mod_game_id);
}

size_t recomp::mods::game_mode_count(const std::string& mod_game_id, bool include_disabled) {
    std::lock_guard lock { mod_context_mutex };
    return mod_context->game_mode_count(mod_game_id, include_disabled);
}

recomp::Version recomp::mods::get_mod_version(size_t mod_index) {
    std::lock_guard lock { mod_context_mutex };
    return mod_context->get_mod_version(mod_index);
}

std::string recomp::mods::get_mod_id(size_t mod_index) {
    std::lock_guard lock { mod_context_mutex };
    return mod_context->get_mod_id(mod_index);
}

void recomp::mods::set_mod_index(const std::string &mod_game_id, const std::string &mod_id, size_t index) {
    std::lock_guard lock{ mod_context_mutex };
    return mod_context->set_mod_index(mod_game_id, mod_id, index);
}

// HH: volcado de crash del port (definido en src/main/main.cpp).
extern "C" void hh_port_crash_dump(uint32_t n64_addr);

// Runs the game entrypoint, capturing a Windows access violation so the boot's faulting address
// can be reported. Extracted into its own noinline function because __try/__except cannot be used
// in a function that requires C++ object unwinding.
#ifdef _WIN32
static __declspec(noinline) uintptr_t run_entrypoint_seh(recomp_func_t* entrypoint, uint8_t* rdram, recomp_context* context, uintptr_t* crash_ip_out) {
    uintptr_t crash_addr = 0;
    uintptr_t crash_ip = 0;
    __try {
        entrypoint(rdram, context);
    } __except (
        GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ?
            (crash_addr = (uintptr_t)GetExceptionInformation()->ExceptionRecord->ExceptionInformation[1],
             crash_ip = (uintptr_t)GetExceptionInformation()->ExceptionRecord->ExceptionAddress,
             EXCEPTION_EXECUTE_HANDLER) :
            EXCEPTION_CONTINUE_SEARCH
    ) {
        // Handled above; crash_addr/crash_ip already captured in the filter.
    }
    *crash_ip_out = crash_ip;
    return crash_addr;
}
#endif

bool wait_for_game_started(uint8_t* rdram, recomp_context* context) {
    game_status.wait(GameStatus::None);

    switch (game_status.load()) {
        // TODO refactor this to allow a project to specify what entrypoint function to run for a give game.
        case GameStatus::Running:
            {
                if (!recomp::load_stored_rom(current_game.value())) {
                    ultramodern::error_handling::message_box("Error opening stored ROM! Please restart this program.");
                }

                auto find_it = game_roms.find(current_game.value());
                const recomp::GameEntry& game_entry = find_it->second;

                init(rdram, context, game_entry.entrypoint_address);
                if (game_entry.on_init_callback) {
                    game_entry.on_init_callback(rdram, context);
                }

                uint32_t mod_ram_used = 0;
                if (!game_entry.mod_game_id.empty()) {
                    std::vector<recomp::mods::ModLoadErrorDetails> mod_load_errors;
                    {
                        std::lock_guard lock { mod_context_mutex };
                        mod_load_errors = mod_context->load_mods(game_entry, current_game_mode_id, rdram, recomp::mod_rdram_start, mod_ram_used);
                    }

                    if (!mod_load_errors.empty()) {
                        std::ostringstream mod_error_stream;
                        mod_error_stream << "Error loading mods:\n\n";
                        for (const auto& cur_error : mod_load_errors) {
                            auto mod_details = recomp::mods::get_details_for_mod(cur_error.mod_id);
                            if (mod_details) {
                                mod_error_stream << mod_details->display_name;
                            }
                            else {
                                mod_error_stream << cur_error.mod_id.c_str();
                            }
                            mod_error_stream << ": " << recomp::mods::error_to_string(cur_error.error);
                            if (!cur_error.error_param.empty()) {
                                mod_error_stream << " (" << cur_error.error_param.c_str() << ")";
                            }
                            mod_error_stream << "\n";                                
                        }
                        ultramodern::error_handling::message_box(mod_error_stream.str().c_str());
                        game_status.store(GameStatus::None);
                        return false;
                    }
                }

                boot_log("init_heap at 0x%08X\n", recomp::mod_rdram_start + mod_ram_used);
                recomp::init_heap(rdram, recomp::mod_rdram_start + mod_ram_used);
                boot_log("init_heap done\n");

                save_type = game_entry.save_type;
                ultramodern::init_saving(rdram);
                boot_log("init_saving done\n");

                boot_log("Calling entrypoint\n");
#ifdef _WIN32
                uintptr_t crash_ip = 0;
                uintptr_t crash_addr = run_entrypoint_seh(game_entry.entrypoint, rdram, context, &crash_ip);
                if (crash_addr != 0) {
                    boot_log("Entrypoint ACCESS VIOLATION\n");
                    boot_log("Crash host IP = 0x%p\n", reinterpret_cast<void*>(crash_ip));
                    boot_log("Crash host addr = 0x%p (valid RDRAM = %p..%p)\n",
                        reinterpret_cast<void*>(crash_addr), rdram, rdram + recomp::mem_size);
                    // Convert the host pointer back to the N64 address the recompiled code was accessing.
                    int64_t n64_addr = (int64_t)(intptr_t)(crash_addr - (uintptr_t)rdram) + 0xFFFFFFFF80000000LL;
                    boot_log("Crash N64 addr = 0x%08X\n", (uint32_t)n64_addr);
                    // HH: que el port deje su volcado completo (hh_crash.log + RDRAM + DMEM).
                    hh_port_crash_dump((uint32_t)n64_addr);
                    std::exit(EXIT_FAILURE);
                }
                boot_log("Entrypoint returned\n");
#else
                try {
                    // The entrypoint is the boot/main thread. Hold the game lock so any threads it
                    // spawns (via osStartThread from a NULL thread_self) cannot run concurrently with
                    // it; they block on the lock until the entrypoint returns and releases it.
                    ultramodern::acquire_game_lock();
                    hh_current_ctx = context;
                    hh_register_ctx(context, (int)hh_sh_get_id(TO_PTR(OSThread, ultramodern::this_thread())), (uintptr_t)TO_PTR(OSThread, ultramodern::this_thread()));
                    game_entry.entrypoint(rdram, context);
                    hh_unregister_ctx(context);
                    hh_current_ctx = nullptr;
                    ultramodern::release_game_lock();
                    boot_log("Entrypoint returned\n");
                } catch (ultramodern::thread_terminated& terminated) {
                    ultramodern::release_game_lock();

                } catch (const std::exception& e) {
                    ultramodern::release_game_lock();
                    boot_log("Entrypoint threw: %s\n", e.what());
                } catch (...) {
                    ultramodern::release_game_lock();
                    boot_log("Entrypoint threw unknown exception\n");
                }
#endif
            }
            return true;

        case GameStatus::Quit:
            return true;

        case GameStatus::None:
            return true;
    }
}

recomp::SaveType recomp::get_save_type() {
    return save_type;
}

bool recomp::eeprom_allowed() {
    return
        save_type == SaveType::Eep4k || 
        save_type == SaveType::Eep16k ||
        save_type == SaveType::AllowAll;
}

bool recomp::sram_allowed() {
    return
        save_type == SaveType::Sram || 
        save_type == SaveType::AllowAll;
}

bool recomp::flashram_allowed() {
    return
        save_type == SaveType::Flashram || 
        save_type == SaveType::AllowAll;
}

void print_cli_game_options() {
    for (const auto &it : game_roms) {
        bool rom_valid = recomp::is_rom_valid(it.second.game_id);
        fprintf(stderr, "\"%s\" (%s)%s\n", it.second.mod_game_id.c_str(), it.second.display_name.c_str(), rom_valid ? "" : " (No ROM)");
    }
}

void print_cli_game_mode_options(const std::vector<recomp::mods::ModDetails> &mods) {
    for (const auto &mod : mods) {
        if (mod.custom_gamemode) {
            bool mod_enabled = recomp::mods::is_mod_enabled(mod.mod_id) || recomp::mods::is_mod_auto_enabled(mod.mod_id);
            fprintf(stderr, "\"%s\" (%s)%s\n", mod.mod_id.c_str(), mod.display_name.c_str(), mod_enabled ? "" : " (Disabled)");
        }
    }
}

void parse_cli(int argc, char **argv) {
    std::string cli_game_id;
    std::string cli_game_mode_id;
    bool game_mode_missing_argument = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--game") == 0) {
            if (i + 1 < argc) {
                cli_game_id = std::string(argv[++i]);
            }
            else {
                fprintf(stderr, "No argument was specified for the game. Possible options are:\n");
                print_cli_game_options();
            }
        }
        else if (strcmp(argv[i], "--game-mode") == 0) {
            if (i + 1 < argc) {
                cli_game_mode_id = std::string(argv[++i]);
            }
            else {
                game_mode_missing_argument = true;
            }
        }
    }

    if (!cli_game_id.empty()) {
        // We use the mod game ID instead to do the lookup since it's much easier for the user to remember.
        std::u8string game_id;
        for (const auto &it : game_roms) {
            if (it.second.mod_game_id == cli_game_id) {
                if (recomp::is_rom_valid(it.second.game_id)) {
                    game_id = it.second.game_id;
                }
                else {
                    fprintf(stderr, "Game \"%s\" does not have a valid ROM. Please select a ROM on the launcher first.\n", cli_game_id.c_str());
                }

                break;
            }
        }

        if (!game_id.empty()) {
            bool game_mode_error = false;
            std::string game_mode_id;
            if (!cli_game_mode_id.empty() || game_mode_missing_argument) {
                std::vector<recomp::mods::ModDetails> mods = recomp::mods::get_all_mod_details(cli_game_id);
                if (!cli_game_mode_id.empty()) {
                    for (const auto &mod : mods) {
                        if ((mod.mod_id == cli_game_mode_id) && mod.custom_gamemode) {
                            if (recomp::mods::is_mod_enabled(mod.mod_id) || recomp::mods::is_mod_auto_enabled(mod.mod_id)) {
                                game_mode_id = mod.mod_id;
                            }
                            else {
                                fprintf(stderr, "Game mode \"%s\" is disabled. Please enable it on the mods menu first.\n", cli_game_mode_id.c_str());
                            }

                            break;
                        }
                    }

                    if (game_mode_id.empty()) {
                        fprintf(stderr, "Game mode \"%s\" is not available. Possible options are:\n", cli_game_mode_id.c_str());
                        print_cli_game_mode_options(mods);
                        game_mode_error = true;
                    }
                }
                else if (game_mode_missing_argument) {
                    fprintf(stderr, "No argument was specified for the game mode. Possible options are:\n");
                    print_cli_game_mode_options(mods);
                    game_mode_error = true;
                }
            }

            if (!game_mode_error) {
                recomp::start_game(game_id, game_mode_id);
            }
        }
        else {
            fprintf(stderr, "Game \"%s\" is not available. Possible options are:\n", cli_game_id.c_str());
            print_cli_game_options();
        }
    }
    else if (game_mode_missing_argument) {
        fprintf(stderr, "No argument was specified for the game mode. The game must be specified first to show the options.\n");
    }
}

void recomp::start(const recomp::Configuration& cfg) {
    project_version = cfg.project_version;

    recomp::check_all_stored_roms();

    recomp::rsp::set_callbacks(cfg.rsp_callbacks);

    static const ultramodern::rsp::callbacks_t ultramodern_rsp_callbacks {
        .init = recomp::rsp::constants_init,
        .run_task = recomp::rsp::run_task,
    };

    ultramodern::set_callbacks(ultramodern_rsp_callbacks, cfg.renderer_callbacks, cfg.audio_callbacks, cfg.input_callbacks, cfg.gfx_callbacks, cfg.events_callbacks, cfg.error_handling_callbacks, cfg.threads_callbacks);

    ultramodern::gfx_callbacks_t gfx_callbacks = cfg.gfx_callbacks;

    ultramodern::gfx_callbacks_t::gfx_data_t gfx_data{};

    if (gfx_callbacks.create_gfx) {
        gfx_data = gfx_callbacks.create_gfx();
    }

    auto window_handle = cfg.window_handle;
    if (window_handle == ultramodern::renderer::WindowHandle{}) {
        if (gfx_callbacks.create_window) {
            window_handle = gfx_callbacks.create_window(gfx_data);
        }
        else {
            assert(false && "No create_window callback provided");
        }
    }

    ultramodern::set_message_queue_control(cfg.message_queue_control);

    recomp::mods::initialize_mods();
    recomp::mods::scan_mods();

    // Allocate rdram without comitting it. Use a platform-specific virtual allocation function
    // that initializes to zero. Protect the region above the memory size to catch accesses to invalid addresses.
    uint8_t* rdram;
    bool alloc_failed;
#ifdef _WIN32
    rdram = reinterpret_cast<uint8_t*>(VirtualAlloc(nullptr, allocation_size, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS));
    DWORD old_protect = 0;
    alloc_failed = (rdram == nullptr);
    if (!alloc_failed) {
        // VirtualProtect returns 0 on failure.
        alloc_failed = (VirtualProtect(rdram, mem_size, PAGE_READWRITE, &old_protect) == 0);
        if (alloc_failed) {
            VirtualFree(rdram, 0, MEM_RELEASE);
        }
    }
#else
    rdram = (uint8_t*)mmap(NULL, allocation_size, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    alloc_failed = rdram == reinterpret_cast<uint8_t*>(MAP_FAILED);
    if (!alloc_failed) {
        // mprotect returns -1 on failure.
        alloc_failed = (mprotect(rdram, mem_size, PROT_READ | PROT_WRITE) == -1);
        if (alloc_failed) {
            munmap(rdram, allocation_size);
        }
    }
#endif

    if (alloc_failed) {
        ultramodern::error_handling::message_box("Failed to allocate memory!");
        return;
    }

    recomp::register_heap_exports();
    recomp::mods::register_config_exports();
    recomp::mods::register_hook_exports();

    std::thread game_thread{[](ultramodern::renderer::WindowHandle window_handle, uint8_t* rdram) {
        debug_printf("[Recomp] Starting\n");

        ultramodern::set_native_thread_name("Game Start Thread");

        ultramodern::preinit(rdram, window_handle);

        recomp_context context{};

        // Loop until the game starts.
        while (!wait_for_game_started(rdram, &context)) {}
    }, window_handle, rdram};

    parse_cli(cfg.argc, cfg.argv);

    while (!exited) {
        ultramodern::sleep_milliseconds(1);
        if (gfx_callbacks.update_gfx != nullptr) {
            gfx_callbacks.update_gfx(gfx_data);
        }
    }

    graphics_shutdown_ready.signal();

    game_thread.join();
    ultramodern::join_event_threads();
    ultramodern::join_thread_cleaner_thread();
    ultramodern::join_saving_thread();
    
    // Free rdram.
    bool free_failed;
#ifdef _WIN32
    // VirtualFree returns zero on failure.
    free_failed = (VirtualFree(rdram, 0, MEM_RELEASE) == 0);
#else
    // munmap returns -1 on failure.
    free_failed = (munmap(rdram, allocation_size) == -1);
#endif

    if (free_failed) {
        printf("Failed to free rdram\n");
    }
}
