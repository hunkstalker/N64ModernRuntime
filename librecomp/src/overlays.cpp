#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
extern "C" char __ImageBase;  // base real de carga del .exe (ASLR incluida)
#define HH_RETURN_ADDR() _ReturnAddress()
#define HH_RETURN_RVA() ((uint64_t)((uintptr_t)_ReturnAddress() - (uintptr_t)&__ImageBase))
#elif defined(__GNUC__) || defined(__clang__)
#define HH_RETURN_ADDR() __builtin_return_address(0)
#define HH_RETURN_RVA() ((uint64_t)(uintptr_t)__builtin_return_address(0))
#else
#define HH_RETURN_ADDR() (nullptr)
#define HH_RETURN_RVA() (UINT64_C(0))
#endif

#include "ultramodern/ultramodern.hpp"

#include "recomp.h"
#include "recompiler/context.h"
#include "overlays.hpp"
#include "sections.h"

static recomp::overlays::overlay_section_table_data_t sections_info {};
static recomp::overlays::overlays_by_index_t overlays_info {};

// HH: registro con marca de tiempo de carga de overlays/modulos (hh_ovl.log, acotado) para
// correlacionar escenas y transiciones con hh_state.log y hh_pi.log. Gated por HH_DIAG=1.
extern "C" int hh_diag_enabled(void);
static void hh_ovl_log(const char* fmt, ...) {
    if (!hh_diag_enabled()) return;
    static FILE* f = nullptr;
    static long total = 0;
    static std::chrono::steady_clock::time_point t0{};
    if (f == nullptr) {
        f = fopen("hh_ovl.log", "w");
        if (f == nullptr) return;
        t0 = std::chrono::steady_clock::now();
    }
    if (total > (8L * 1024 * 1024)) return;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    va_list args;
    va_start(args, fmt);
    total += fprintf(f, "[OVL] t=%.3f ", t);
    total += vfprintf(f, fmt, args);
    va_end(args);
    fflush(f);
}

static SectionTableEntry* patch_code_sections = nullptr;
size_t num_patch_code_sections = 0;
static std::vector<char> patch_data;

struct LoadedSection {
    int32_t loaded_ram_addr;
    size_t section_table_index;

    LoadedSection(int32_t loaded_ram_addr_, size_t section_table_index_) {
        loaded_ram_addr = loaded_ram_addr_;
        section_table_index = section_table_index_;
    }

    bool operator<(const LoadedSection& rhs) {
        return loaded_ram_addr < rhs.loaded_ram_addr;
    }
};

static std::unordered_map<uint32_t, uint16_t> code_sections_by_rom{};
static std::unordered_map<uint32_t, uint16_t> patch_code_sections_by_rom{};
static std::vector<LoadedSection> loaded_sections{};
static std::unordered_map<int32_t, recomp_func_t*> func_map{};
static std::unordered_map<uint32_t, uint32_t> module_sources{};
static std::unordered_set<uint32_t> module_rom_addrs{};
static std::unordered_map<std::string, recomp_func_t*> base_exports{};
static std::unordered_map<std::string, recomp_func_ext_t*> ext_base_exports{};
static std::unordered_map<std::string, size_t> base_events;
static std::unordered_map<uint32_t, recomp_func_t*> manual_patch_symbols_by_vram;

extern "C" {
int32_t* section_addresses = nullptr;
}

void recomp::overlays::register_overlays(const overlay_section_table_data_t& sections, const overlays_by_index_t& overlays) {
    sections_info = sections;
    overlays_info = overlays;
}

void recomp::overlays::register_patches(const char* patch, std::size_t size, SectionTableEntry* sections, size_t num_sections) {
    patch_code_sections = sections;
    num_patch_code_sections = num_sections;

    patch_data.resize(size);
    std::memcpy(patch_data.data(), patch, size);

    patch_code_sections_by_rom.reserve(num_patch_code_sections);
    for (size_t i = 0; i < num_patch_code_sections; i++) {
        patch_code_sections_by_rom.emplace(patch_code_sections[i].rom_addr, i);
    }
}

void recomp::overlays::register_base_export(const std::string& name, recomp_func_t* func) {
    base_exports.emplace(name, func);
}

void recomp::overlays::register_ext_base_export(const std::string& name, recomp_func_ext_t* func) {
    ext_base_exports.emplace(name, func);
}

void recomp::overlays::register_base_exports(const FunctionExport* export_list) {
    std::unordered_map<uint32_t, recomp_func_t*> patch_func_vram_map{};

    // Iterate over all patch functions to set up a mapping of their vram address.
    for (size_t patch_section_index = 0; patch_section_index < num_patch_code_sections; patch_section_index++) {
        const SectionTableEntry* cur_section = &patch_code_sections[patch_section_index];

        for (size_t func_index = 0; func_index < cur_section->num_funcs; func_index++) {
            const FuncEntry* cur_func = &cur_section->funcs[func_index];
            patch_func_vram_map.emplace(cur_section->ram_addr + cur_func->offset, cur_func->func);
        }
    }

    // Iterate over exports, using the vram mapping to create a name mapping.
    for (const FunctionExport* cur_export = &export_list[0]; cur_export->name != nullptr; cur_export++) {
        auto it = patch_func_vram_map.find(cur_export->ram_addr);
        if (it == patch_func_vram_map.end()) {
            assert(false && "Failed to find exported function in patch function sections!");
        }
        base_exports.emplace(cur_export->name, it->second);
    }
}

recomp_func_t* recomp::overlays::get_base_export(const std::string& export_name) {
    auto it = base_exports.find(export_name);
    if (it == base_exports.end()) {
        return nullptr;
    }
    return it->second;
}

recomp_func_ext_t* recomp::overlays::get_ext_base_export(const std::string& export_name) {
    auto it = ext_base_exports.find(export_name);
    if (it == ext_base_exports.end()) {
        return nullptr;
    }
    return it->second;
}

void recomp::overlays::register_base_events(char const* const* event_names) {
    for (size_t event_index = 0; event_names[event_index] != nullptr; event_index++) {
        base_events.emplace(event_names[event_index], event_index);
    }
}

size_t recomp::overlays::get_base_event_index(const std::string& event_name) {
    auto it = base_events.find(event_name);
    if (it == base_events.end()) {
        return (size_t)-1;
    }
    return it->second;
}

size_t recomp::overlays::num_base_events() {
    return base_events.size();
}

const std::unordered_map<uint32_t, uint16_t>& recomp::overlays::get_vrom_to_section_map() {
    return code_sections_by_rom;
}

uint32_t recomp::overlays::get_section_ram_addr(uint16_t code_section_index) {
    return sections_info.code_sections[code_section_index].ram_addr;
}

std::span<const RelocEntry> recomp::overlays::get_section_relocs(uint16_t code_section_index) {
    if (code_section_index < sections_info.num_code_sections) {
        const auto& section = sections_info.code_sections[code_section_index];
        return std::span{ section.relocs, section.num_relocs };
    }
    assert(false);
    return {};
}

void recomp::overlays::add_loaded_function(int32_t ram, recomp_func_t* func) {
    func_map[ram] = func;
}

// HH: diagnostico de PROPIEDAD de direcciones en func_map (HH_FUNC_OWNER=0x...,...). Logea a stderr
// que seccion registra/borra cada direccion vigilada y a que funcion resuelve get_function. Sirve
// para cazar entradas rancias cuando dos modulos comparten base (p. ej. M8/M23 en 0x801C1EE0).
static bool hh_owner_watched(uint32_t a) {
    static uint32_t addrs[16];
    static size_t count = (size_t)-1;
    if (count == (size_t)-1) {
        count = 0;
        const char* spec = getenv("HH_FUNC_OWNER");
        if (spec != nullptr && *spec != '\0') {
            char buf[512];
            strncpy(buf, spec, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            for (char* tok = strtok(buf, ","); tok != nullptr && count < 16; tok = strtok(nullptr, ",")) {
                addrs[count++] = (uint32_t)strtoul(tok, nullptr, 16);
            }
        }
    }
    for (size_t i = 0; i < count; i++) {
        if (addrs[i] == a) return true;
    }
    return false;
}

extern "C" uint64_t hh_get_vi_count(void);
extern "C" uint64_t hh_get_game_frame_count(void);
extern "C" uint64_t hh_replay_get_sample(void);

// HH: traza BASE-AWARE por offset de modulo (HH_MODTRACE=[rom:]offset:label,...). Localiza la
// seccion cargada que contiene la direccion y compara el offset, asi funciona aunque el modulo
// este REUBICADO (p. ej. modulo 23 en 0x801FA948). Con rom=0 (u omitido) vale cualquier modulo.
static bool hh_modtrace_match(uint32_t addr, const char** label_out) {
    const char* spec = getenv("HH_MODTRACE");
    if (spec == nullptr || *spec == '\0') return false;
    char buf[512];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (char* tok = strtok(buf, ","); tok != nullptr; tok = strtok(nullptr, ",")) {
        char* c1 = strchr(tok, ':');
        if (c1 == nullptr) continue;
        *c1 = 0;
        char* c2 = strchr(c1 + 1, ':');
        uint32_t rom = 0, off = 0;
        const char* lab;
        if (c2 != nullptr) {
            *c2 = 0;
            rom = (uint32_t)strtoul(tok, nullptr, 16);
            off = (uint32_t)strtoul(c1 + 1, nullptr, 16);
            lab = c2 + 1;
        } else {
            off = (uint32_t)strtoul(tok, nullptr, 16);
            lab = c1 + 1;
        }
        for (const auto& ls : loaded_sections) {
            const SectionTableEntry& sec = sections_info.code_sections[ls.section_table_index];
            uint32_t base = (uint32_t)ls.loaded_ram_addr;
            if (addr >= base && addr < base + (uint32_t)sec.size) {
                if ((addr - base) == off && (rom == 0 || rom == (sec.rom_addr & 0x1FFFFFFFu))) {
                    *label_out = lab;
                    return true;
                }
            }
        }
    }
    return false;
}

static void hh_owner_log(const char* what, uint32_t addr, const void* func, long section = -1) {
    if (!hh_owner_watched(addr)) return;
    static long lines = 0;
    if (lines++ > 20000) return;
    fprintf(stderr, "[OWNER] %s section=%ld addr=%08X func=%p\n", what, section, addr, func);
}

// HH: registra el módulo recompilado que corresponde al blob de ROM que el juego carga.
void recomp::overlays::register_module_sources(const ModuleSource* sources, size_t count) {
    module_sources.clear();
    module_rom_addrs.clear();
    for (size_t i = 0; i < count; i++) {
        uint32_t src = sources[i].src_rom & 0x1FFFFFFFu;
        module_sources.emplace(src, sources[i].rom_addr);
        module_rom_addrs.insert(sources[i].rom_addr);
    }
}

// Registers every function of every code section at its absolute ram_addr. This is used for flat
// (non-overlay) code where each section has its own absolute ram_addr that isn't entrypoint-relative.
void recomp::overlays::register_flat_code() {
    for (size_t section_index = 0; section_index < sections_info.num_code_sections; section_index++) {
        // HH: los módulos con registro dinámico (module_sources) NO se registran en su base fija:
        // el juego puede reutilizar la misma VRAM para otro módulo (se registran al cargarse).
        if (module_rom_addrs.count(sections_info.code_sections[section_index].rom_addr) != 0) {
            continue;
        }
        const SectionTableEntry& section = sections_info.code_sections[section_index];
        for (size_t function_index = 0; function_index < section.num_funcs; function_index++) {
            const FuncEntry& func = section.funcs[function_index];
            if (hh_owner_watched(section.ram_addr + func.offset)) {
                hh_owner_log("flat-load", section.ram_addr + func.offset, (const void*)func.func, (long)section_index);
            }
            func_map[section.ram_addr + func.offset] = func.func;
        }
    }
}

void load_overlay(size_t section_table_index, int32_t ram) {
    const SectionTableEntry& section = sections_info.code_sections[section_table_index];
    hh_ovl_log("load section=%zu idx=%u rom=%08X ram=%08X funcs=%zu\n", section_table_index,
               (unsigned)section.index, (unsigned)section.rom_addr, (unsigned)ram,
               (size_t)section.num_funcs);

    // HH: borrado del rango al cargar (`HH_RANGECLEAR=1`, OPT-IN). Probado en el replay Linux: no
    // cambia el desenlace del CaC (estado 0x801CC8C4 igual, mismo freeze) y las entradas "rancias"
    // de 0x801C40EC/40F8 solo se usan ANTES de que cargue la seccion que reutiliza la base. Se deja
    // disponible para experimentos, desactivado por defecto para no alterar el comportamiento.
    if (getenv("HH_RANGECLEAR") != nullptr) {
        const uint32_t hh_lo = (uint32_t)ram;
        const uint32_t hh_hi = hh_lo + (uint32_t)section.size;
        for (auto it = func_map.begin(); it != func_map.end();) {
            uint32_t a = (uint32_t)it->first;
            if (a >= hh_lo && a < hh_hi) {
                it = func_map.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (size_t function_index = 0; function_index < section.num_funcs; function_index++) {
        const FuncEntry& func = section.funcs[function_index];
        if (hh_owner_watched(ram + func.offset)) {
            hh_owner_log("load", ram + func.offset, (const void*)func.func, (long)section_table_index);
        }
        func_map[ram + func.offset] = func.func;
    }

    loaded_sections.emplace_back(ram, section_table_index);
    section_addresses[section.index] = ram;
}

// HH: registra la sección del módulo que corresponde al blob cargado (ver overlays.hpp).
void recomp::overlays::load_module_by_source(uint32_t src_rom, int32_t ram_addr) {
    auto it = module_sources.find(src_rom & 0x1FFFFFFFu);
    if (it == module_sources.end()) {
        return;
    }
    // Las secciones están ordenadas por rom_addr (init_overlays): buscar la posición actual.
    for (size_t i = 0; i < sections_info.num_code_sections; i++) {
        if (sections_info.code_sections[i].rom_addr == it->second) {
            hh_ovl_log("module src=%06X section=%zu rom=%08X ram=%08X\n",
                       (unsigned)(src_rom & 0x1FFFFFFFu), i, (unsigned)it->second, (unsigned)ram_addr);
            if (getenv("HH_TBLTRACE") != nullptr) {
                fprintf(stderr, "[OVL] src=%06X -> section[%zu] rom=%08X at %08X\n",
                        (unsigned)(src_rom & 0x1FFFFFFFu), i, (unsigned)it->second, (unsigned)ram_addr);
            }
            // HH (revertido): se probó a desregistrar la sección anterior al reutilizar base, pero
            // NO cambió el bloqueo de combate (misma cadena de módulo 23, mismo estado). Se restaura
            // el comportamiento original para no dejar cambios no validados.
            load_overlay(i, ram_addr);
            return;
        }
    }
}

static void load_special_overlay(const SectionTableEntry& section, int32_t ram) {
    for (size_t function_index = 0; function_index < section.num_funcs; function_index++) {
        const FuncEntry& func = section.funcs[function_index];
        func_map[ram + func.offset] = func.func;
    }
}

static void load_patch_functions() {
    if (patch_code_sections == nullptr) {
        debug_printf("[Patch] No patch section was registered\n");
        return;
    }
    for (size_t i = 0; i < num_patch_code_sections; i++) {
        load_special_overlay(patch_code_sections[i], patch_code_sections[i].ram_addr);
    }
}

void recomp::overlays::read_patch_data(uint8_t* rdram, gpr patch_data_address) {
    for (size_t i = 0; i < patch_data.size(); i++) {
        MEM_B(i, patch_data_address) = patch_data[i];
    }
}

extern "C" void load_overlays(uint32_t rom, int32_t ram_addr, uint32_t size) {
    // Search for the first section that's included in the loaded rom range
    // Sections were sorted by `init_overlays` so we can use the bounds functions
    auto lower = std::lower_bound(&sections_info.code_sections[0], &sections_info.code_sections[sections_info.num_code_sections], rom,
        [](const SectionTableEntry& entry, uint32_t addr) {
            return entry.rom_addr < addr;
        }
    );
    auto upper = std::upper_bound(&sections_info.code_sections[0], &sections_info.code_sections[sections_info.num_code_sections], (uint32_t)(rom + size),
        [](uint32_t addr, const SectionTableEntry& entry) {
            return addr < entry.size + entry.rom_addr;
        }
    );
    // Load the overlays that were found
    for (auto it = lower; it != upper; ++it) {
        load_overlay(std::distance(&sections_info.code_sections[0], it), it->rom_addr - rom + ram_addr);
    }
}

extern "C" void unload_overlay_by_id(uint32_t id) {
    uint32_t section_table_index = overlays_info.table[id];
    const SectionTableEntry& section = sections_info.code_sections[section_table_index];

    auto find_it = std::find_if(loaded_sections.begin(), loaded_sections.end(), [section_table_index](const LoadedSection& s) { return s.section_table_index == section_table_index; });

    if (find_it != loaded_sections.end()) {
        // Determine where each function was loaded to and remove that entry from the function map
        for (size_t func_index = 0; func_index < section.num_funcs; func_index++) {
            const auto& func = section.funcs[func_index];
            uint32_t func_address = func.offset + find_it->loaded_ram_addr;
            if (hh_owner_watched(func_address)) hh_owner_log("unload", func_address, nullptr, (long)section_table_index);
            func_map.erase(func_address);
        }
        // Reset the section's address in the address table
        section_addresses[section.index] = section.ram_addr;
        // Remove the section from the loaded section map
        loaded_sections.erase(find_it);
    }
}

extern "C" void load_overlay_by_id(uint32_t id, uint32_t ram_addr) {
    uint32_t section_table_index = overlays_info.table[id];
    const SectionTableEntry& section = sections_info.code_sections[section_table_index];
    int32_t prev_address = section_addresses[section.index];
    if (/*ram_addr >= 0x80000000 && ram_addr < 0x81000000) {*/ prev_address == section.ram_addr) {
        load_overlay(section_table_index, ram_addr);
    }
    else {
        int32_t new_address = prev_address + ram_addr;
        unload_overlay_by_id(id);
        load_overlay(section_table_index, new_address);
    }
}

extern "C" void unload_overlays(int32_t ram_addr, uint32_t size) {
    for (auto it = loaded_sections.begin(); it != loaded_sections.end();) {
        const auto& section = sections_info.code_sections[it->section_table_index];

        // Check if the unloaded region overlaps with the loaded section
        if (ram_addr < (it->loaded_ram_addr + section.size) && (ram_addr + size) >= it->loaded_ram_addr) {
            // Check if the section isn't entirely in the loaded region
            if (ram_addr > it->loaded_ram_addr || (ram_addr + size) < (it->loaded_ram_addr + section.size)) {
                fprintf(stderr,
                    "Cannot partially unload section\n"
                    "  rom: 0x%08X size: 0x%08X loaded_addr: 0x%08X\n"
                    "  unloaded_ram: 0x%08X unloaded_size : 0x%08X\n",
                        section.rom_addr, section.size, it->loaded_ram_addr, ram_addr, size);
                assert(false);
                std::exit(EXIT_FAILURE);
            }
            // Determine where each function was loaded to and remove that entry from the function map
            for (size_t func_index = 0; func_index < section.num_funcs; func_index++) {
                const auto& func = section.funcs[func_index];
                uint32_t func_address = func.offset + it->loaded_ram_addr;
                func_map.erase(func_address);
            }
            // Reset the section's address in the address table
            section_addresses[section.index] = section.ram_addr;
            // Remove the section from the loaded section map
            it = loaded_sections.erase(it);
            // Skip incrementing the iterator
            continue;
        }
        ++it;
    }
}

void recomp::overlays::init_overlays() {
    func_map.clear();
    section_addresses = (int32_t *)calloc(sections_info.total_num_sections, sizeof(int32_t));

    // Sort the executable sections by rom address
    std::sort(&sections_info.code_sections[0], &sections_info.code_sections[sections_info.num_code_sections],
        [](const SectionTableEntry& a, const SectionTableEntry& b) {
            return a.rom_addr < b.rom_addr;
        }
    );

    for (size_t section_index = 0; section_index < sections_info.num_code_sections; section_index++) {
        SectionTableEntry* code_section = &sections_info.code_sections[section_index];

        section_addresses[sections_info.code_sections[section_index].index] = code_section->ram_addr;
        code_sections_by_rom[code_section->rom_addr] = section_index;        
    }

    load_patch_functions();
}

// Finds a function given a section's index and the function's offset into the section.
bool recomp::overlays::get_func_entry_by_section_index_function_offset(uint16_t code_section_index, uint32_t function_offset, FuncEntry& func_out) {
    if (code_section_index >= sections_info.num_code_sections) {
        return false;
    }

    SectionTableEntry* section = &sections_info.code_sections[code_section_index];
    if (function_offset >= section->size) {
        return false;
    }
    
    // TODO avoid a linear lookup here.
    for (size_t func_index = 0; func_index < section->num_funcs; func_index++) {
        if (section->funcs[func_index].offset == function_offset) {
            func_out = section->funcs[func_index];
            return true;
        }
    }

    return false;
}

void recomp::overlays::register_manual_patch_symbols(const ManualPatchSymbol* manual_patch_symbols) {
    for (size_t i = 0; manual_patch_symbols[i].func != nullptr; i++) {
        if (!manual_patch_symbols_by_vram.emplace(manual_patch_symbols[i].ram_addr, manual_patch_symbols[i].func).second) {
            printf("Duplicate manual patch symbol address: %08X\n", manual_patch_symbols[i].ram_addr);
            ultramodern::error_handling::message_box("Duplicate manual patch symbol address (syms.ld)!");
            assert(false && "Duplicate manual patch symbol address (syms.ld)!");
            ultramodern::error_handling::quick_exit(__FILE__, __LINE__, __FUNCTION__);
        }
    }
}

// TODO use N64Recomp::is_manual_patch_symbol instead after updating submodule.
bool is_manual_patch_symbol(uint32_t vram) {
    return vram >= 0x8F000000 && vram < 0x90000000;
}

// Finds a function given a section's index and the function's offset into the section and returns its native pointer.
recomp_func_t* recomp::overlays::get_func_by_section_index_function_offset(uint16_t code_section_index, uint32_t function_offset) {
    FuncEntry entry;
    
    if (get_func_entry_by_section_index_function_offset(code_section_index, function_offset, entry)) {
        return entry.func;
    }

    if (code_section_index == N64Recomp::SectionAbsolute && is_manual_patch_symbol(function_offset)) {
        auto find_it = manual_patch_symbols_by_vram.find(function_offset);
        if (find_it != manual_patch_symbols_by_vram.end()) {
            return find_it->second;
        }
    }

    return nullptr;
}

// Finds a function given a section's rom address and the function's vram address.
recomp_func_t* recomp::overlays::get_func_by_section_rom_function_vram(uint32_t section_rom, uint32_t function_vram) {
    auto find_section_it = code_sections_by_rom.find(section_rom);
    if (find_section_it == code_sections_by_rom.end()) {
        return nullptr;
    }

    SectionTableEntry* section = &sections_info.code_sections[find_section_it->second];
    int32_t func_offset = function_vram - section->ram_addr;
    
    return get_func_by_section_index_function_offset(find_section_it->second, func_offset);
}

// Stub para el modo soft-lookup (HH_SOFT_LOOKUP=1): permite que el boot continúe y revele más
// funciones faltantes en un solo run. Solo para depuración de símbolos.
static void soft_missing_func(uint8_t*, recomp_context*) {}

static FILE* hh_calltrace_fp = nullptr;
static unsigned long hh_calltrace_n = 0;
static void hh_calltrace(uint32_t a) {
    // HH: cachear el flag (getenv en cada llamada recompilada era overhead masivo).
    static int state = -1;  // -1 sin init, 0 off, 1 on
    if (state < 0) {
        const char* e = getenv("HH_CALLTRACE");
        if (e == nullptr || *e == 0) {
            state = 0;
        } else {
            hh_calltrace_fp = fopen(e, "wb");
            state = (hh_calltrace_fp != nullptr) ? 1 : 0;
        }
    }
    if (state != 1) return;
    fwrite(&a, 4, 1, hh_calltrace_fp);
    if ((++hh_calltrace_n & 0xFFF) == 0) fflush(hh_calltrace_fp);
}

// HH resource-table tracing (HH_TBLTRACE=1): runtime-side wrappers around the table helpers so the
// arguments/results can be observed without modifying the generated C.
static recomp_func_t* hh_real_FUN_80017064 = nullptr; // id -> index (-1 if absent)
static recomp_func_t* hh_real_FUN_80017014 = nullptr; // table[index] -> id (0xFFFF if oob)
static recomp_func_t* hh_real_FUN_800045e8 = nullptr; // register resource by id
static recomp_func_t* hh_real_FUN_80017384 = nullptr; // set id at index
static recomp_func_t* hh_real_FUN_800173b8 = nullptr; // set ptr at index
static recomp_func_t* hh_real_FUN_80125814 = nullptr; // registration step
static recomp_func_t* hh_real_FUN_80125808 = nullptr; // read-flag + fallthrough to register step
static recomp_func_t* hh_real_FUN_80124ed8 = nullptr; // resource registration state machine

static bool hh_trc() {
    static const bool enabled = getenv("HH_TRCTRACE") != nullptr;
    return enabled;
}
static bool hh_is_burst_id(uint32_t id);
static int hh_ms_now();
static void hh_wrap_FUN_80017064(uint8_t* rdram, recomp_context* ctx) {
    uint32_t id = (uint32_t)ctx->r4 & 0xFFFF;
    uint32_t sp_before = (uint32_t)ctx->r29;
    uint32_t ra_before = (uint32_t)ctx->r31;
    hh_real_FUN_80017064(rdram, ctx);
    if (getenv("HH_LSTTRACE") != nullptr && hh_is_burst_id(id)) {
        fprintf(stderr, "[LST] t=%d 17064 id=%u -> %08X\n", hh_ms_now(), id, (unsigned)ctx->r2);
    }
    if ((id != 0 && hh_trc()) || id == 0x74) {
        auto r16 = [&](uint32_t a){ return *(uint16_t*)&rdram[(a ^ 2) & 0x7FFFFF]; };
        fprintf(stderr, "[TRC] FIND(0x%04X) END -> 0x%08X sp=%08X->%08X ra=%08X->%08X tbl=%04X %04X %04X %04X\n", id,
            (unsigned)ctx->r2, sp_before, (unsigned)ctx->r29, ra_before, (unsigned)ctx->r31,
            r16(0x8008DFC0), r16(0x8008DFC8), r16(0x8008DFD0), r16(0x8008DFD8));
    }
}
static void hh_wrap_FUN_80017014(uint8_t* rdram, recomp_context* ctx) {
    uint32_t idx = (uint32_t)ctx->r4;
    hh_real_FUN_80017014(rdram, ctx);
    uint32_t val = (uint32_t)ctx->r2 & 0xFFFF;
    if (hh_trc() && (idx < 6 || (val != 0 && val != 0xFFFF))) {
        fprintf(stderr, "[TRC]   tbl[%u] = 0x%04X\n", idx, val);
    }
}
static void hh_wrap_FUN_800045e8(uint8_t* rdram, recomp_context* ctx) {
    if (hh_trc()) fprintf(stderr, "[TRC] REG  id=0x%04X\n", (unsigned)ctx->r4 & 0xFFFF);
    hh_real_FUN_800045e8(rdram, ctx);
}
static void hh_wrap_FUN_80017384(uint8_t* rdram, recomp_context* ctx) {
    uint32_t id = (uint32_t)ctx->r5 & 0xFFFF;
    if (id != 0 && hh_trc()) fprintf(stderr, "[TRC] SETID idx=%u id=0x%04X\n", (unsigned)ctx->r4, id);
    hh_real_FUN_80017384(rdram, ctx);
}
static void hh_wrap_FUN_800173b8(uint8_t* rdram, recomp_context* ctx) {
    if ((uint32_t)ctx->r5 != 0 && hh_trc()) fprintf(stderr, "[TRC] SETPTR idx=%u ptr=0x%08X\n", (unsigned)ctx->r4, (unsigned)ctx->r5);
    hh_real_FUN_800173b8(rdram, ctx);
}
static void hh_wrap_FUN_80125814(uint8_t* rdram, recomp_context* ctx) {
    uint32_t id = (uint32_t)ctx->r4 & 0xFFFF;
    uint32_t t6 = (uint32_t)ctx->r14;
    hh_real_FUN_80125814(rdram, ctx);
    if (id != 0 && hh_trc()) fprintf(stderr, "[TRC] REGSTEP id=0x%04X t6=%u -> 0x%08X\n", id, t6, (unsigned)ctx->r2);
}
static void hh_wrap_FUN_80125808(uint8_t* rdram, recomp_context* ctx) {
    uint32_t id = (uint32_t)ctx->r4 & 0xFFFF;
    auto rb = [&](uint32_t a){ return (uint32_t)rdram[(a ^ 3) & 0x7FFFFF]; };
    uint32_t flag = rb(0x8008D550);
    hh_real_FUN_80125808(rdram, ctx);
    if (hh_trc() || id == 0x74 || id == 0x104) {
        static int n = 0;
        if (n < 400) fprintf(stderr, "[TRC] STEP5808 n=%d id=0x%04X flag=0x%02X c0=%02X ff=%02X d558=%02X -> r=0x%08X\n",
            n, id, flag, rb(0x8008D570), rb(0x8008D5AF), rb(0x8008D558), (unsigned)ctx->r2);
        n++;
    }
}
static void hh_wrap_FUN_80124ed8(uint8_t* rdram, recomp_context* ctx) {
    if (hh_trc()) fprintf(stderr, "[TRC] SMED8 a0=0x%08X\n", (unsigned)ctx->r4);
    hh_real_FUN_80124ed8(rdram, ctx);
}

// HH: cola de peticiones del servidor RSP (+0x888) y su broadcast a las colas de completion
// (nod+4). Sirve para ver por qué el hilo 17 no despierta de su espera en obj+0x158.
static recomp_func_t* hh_real_FUN_80000a0c = nullptr; // broadcast a todos los nodos
static recomp_func_t* hh_real_FUN_80000934 = nullptr; // push request node
static recomp_func_t* hh_real_FUN_80000984 = nullptr; // pop request node
extern "C" void hh_nodewatch_set(uint32_t addr);
extern "C" recomp_context* hh_get_current_ctx(void);
extern "C" int hh_get_callring(recomp_context* c, uint32_t* out, int max);
static int hh_tid(uint8_t* rdram) {
    PTR(OSThread) t = ultramodern::this_thread();
    return t == NULLPTR ? -1 : (int)TO_PTR(OSThread, t)->id;
}
// HH: valida que `q` sea una OSMesgQueue plausible (validCount/first/msgCount coherentes). Sirve para
// marcar el PRIMER nodo corrupto de la lista de suscriptores sin depender del envio (BADMQ).
static bool hh_q_ok(uint8_t* rdram, uint32_t q) {
    if ((q & 3u) != 0 || q < 0x80040000u || q >= 0x80200000u) return false;
    uint32_t o = q & 0x1FFFFFFFu;
    if (o + 0x18 > 0x800000u) return false;
    int32_t valid = *(int32_t*)(rdram + o + 8);
    int32_t first = *(int32_t*)(rdram + o + 0xC);
    int32_t msgCount = *(int32_t*)(rdram + o + 0x10);
    uint32_t msg = *(uint32_t*)(rdram + o + 0x14);
    if (msgCount < 1 || msgCount > 0x1000) return false;
    if (valid < 0 || valid > msgCount) return false;
    if (first < 0 || first >= msgCount) return false;
    if (msg < 0x80000000u || msg >= 0x80800000u) return false;
    return true;
}
static int hh_bcorrupt_seen[16];
static int hh_bcorrupt_n = 0;
static void hh_wrap_FUN_80000a0c(uint8_t* rdram, recomp_context* ctx) {
    auto r32 = [&](uint32_t a){ return *(uint32_t*)&rdram[a & 0x1FFFFFFF]; };
    static uint64_t hh_n = 0;
    uint32_t obj = (uint32_t)ctx->r4, msg = (uint32_t)ctx->r5;
    uint32_t head = r32(obj + 0x888);
    // HH: recorre la lista (acotada) guardando el camino; detecta puntero/cola invalidos o CICLO.
    // Si hay problema, vuelca el camino completo (nodos + q) una vez por head.
    {
        uint32_t path[64];
        uint32_t pnext[64];
        uint32_t pq[64];
        int plen = 0;
        bool bad = false;
        uint32_t n = head;
        for (int i = 0; n != 0 && i < 64; i++) {
            bool node_ok = (n & 3u) == 0 && n >= 0x80040000u && n < 0x80800000u;
            if (!node_ok) { bad = true; path[plen] = n; pnext[plen] = 0; pq[plen] = 0; plen++; break; }
            for (int k = 0; k < plen; k++) if (path[k] == n) { bad = true; break; }
            if (bad) break;
            uint32_t q = r32(n + 4);
            path[plen] = n; pnext[plen] = r32(n); pq[plen] = q; plen++;
            if (!hh_q_ok(rdram, q)) { bad = true; break; }
            n = r32(n);
        }
        if (plen > 8) bad = true;
        if (bad) {
            bool seen = false;
            for (int k = 0; k < hh_bcorrupt_n; k++) if (hh_bcorrupt_seen[k] == (int)head) { seen = true; break; }
            if (!seen && hh_bcorrupt_n < 16) hh_bcorrupt_seen[hh_bcorrupt_n++] = (int)head;
            if (!seen) {
                FILE* f = fopen("hh_bcorrupt.log", "a");
                if (f != nullptr) {
                    uint32_t ring[16];
                    recomp_context* c = hh_get_current_ctx();
                    int rn = c ? hh_get_callring(c, ring, 16) : 0;
                    fprintf(f, "[BCORRUPT] n=%llu tid=%d obj=%08X msg=%08X head=%08X plen=%d sp=%08X\n",
                            (unsigned long long)hh_n, hh_tid(rdram), obj, msg, head, plen,
                            c ? (uint32_t)c->r29 : 0);
                    for (int k = 0; k < plen; k++)
                        fprintf(f, "      [%d] node=%08X next=%08X q=%08X qok=%d\n",
                                k, path[k], pnext[k], pq[k], hh_q_ok(rdram, pq[k]));
                    fprintf(f, "      last:");
                    for (int k = 0; k < rn; k++) fprintf(f, " %08X", ring[k]);
                    fprintf(f, "\n");
                    fflush(f);
                    fclose(f);
                }
            }
        }
    }
    if (hh_n < 10 || (hh_n % 200) == 0) {
        fprintf(stderr, "[BCAST] n=%llu tid=%d obj=%08X msg=%08X head=%08X", (unsigned long long)hh_n, hh_tid(rdram), obj, msg, head);
        for (uint32_t n = head, i = 0; n != 0 && i < 8; n = r32(n), i++) {
            fprintf(stderr, " [node=%08X q=%08X]", n, r32(n + 4));
        }
        fprintf(stderr, "\n");
    }
    hh_n++;
    hh_real_FUN_80000a0c(rdram, ctx);
}
static void hh_wrap_FUN_80000934(uint8_t* rdram, recomp_context* ctx) {
    auto r32 = [&](uint32_t a){ return *(uint32_t*)&rdram[a & 0x1FFFFFFF]; };
    uint32_t obj = (uint32_t)ctx->r4, node = (uint32_t)ctx->r5, q = (uint32_t)ctx->r6;
    fprintf(stderr, "[PUSH] tid=%d obj=%08X node=%08X q=%08X oldhead=%08X\n", hh_tid(rdram), obj, node, q, r32(obj + 0x888));
    if (obj == 0x8005C4B0 && (q == 0x8005C288u || q == 0x80091DA0u)) hh_nodewatch_set(node);
    hh_real_FUN_80000934(rdram, ctx);
}
static void hh_wrap_FUN_80000984(uint8_t* rdram, recomp_context* ctx) {
    uint32_t obj = (uint32_t)ctx->r4, node = (uint32_t)ctx->r5;
    fprintf(stderr, "[POP ] tid=%d obj=%08X node=%08X\n", hh_tid(rdram), obj, node);
    hh_real_FUN_80000984(rdram, ctx);
}
// HH: sondeo del estado de framebuffer del juego (VI context) vs el del runtime.
static recomp_func_t* hh_real_FUN_80035050 = nullptr; // [ [0x8004AED0]+4 ] (VI ctx del juego)
static int hh_fb_n = 0;
static void hh_wrap_FUN_80035050(uint8_t* rdram, recomp_context* ctx) {
    hh_real_FUN_80035050(rdram, ctx);
    if (hh_fb_n < 40) {
        fprintf(stderr, "[FB ] 35050 ret=%08X\n", (unsigned)ctx->r2);
        hh_fb_n++;
    }
}
// HH: args del loader (a0=src ROM, a1=dest, a2=size, a3=end descomp.). Tras la descompresión,
// registra la sección recompilada del módulo en la base real (reutilización de bases).
// HH: dispatcher de recarga por id (FUN_8000469C): a0=id, a1=dst, tablas en 0x80038FF0 (pares
// low/high con flag 0x80000000 en low) y 0x80037C5C. Traza para el diferencial CaC (carga #22).
static recomp_func_t* hh_real_FUN_8000469c = nullptr;
static void hh_wrap_FUN_8000469c(uint8_t* rdram, recomp_context* ctx) {
    uint32_t id = (uint32_t)ctx->r4, dst = (uint32_t)ctx->r5, ra = (uint32_t)ctx->r31;
    if (getenv("HH_DTTRACE") != nullptr) {
        auto r32 = [&](uint32_t a){ return *(uint32_t*)&rdram[a & 0x7FFFFF]; };
        uint32_t o = (id - 1) * 4, o2 = (id - 1) * 8;
        fprintf(stderr, "[DT] id=%u dst=%08X ra=%08X low=%08X high=%08X t2=%08X,%08X\n", id, dst, ra,
                r32(0x80038FF0 + o), r32(0x80038FF4 + o), r32(0x80037C5C + o2), r32(0x80037C5C + o2 + 4));
    }
    hh_real_FUN_8000469c(rdram, ctx);
    if (getenv("HH_DTTRACE") != nullptr) {
        fprintf(stderr, "[DT]   id=%u ret=%08X\n", id, (unsigned)ctx->r2);
    }
}
static recomp_func_t* hh_real_FUN_80003824 = nullptr;
// Fase B (ADR 0007): cache de assets + loader LZKN64 nativo (implementado en el port,
// src/game/trans_cache.cpp). El wrapper cede el control a hh_trans_load, que decide entre cache,
// nativo o loader original.
extern "C" void hh_trans_load(uint8_t* rdram, recomp_context* ctx, recomp_func_t* real_loader);
static void hh_wrap_FUN_80003824(uint8_t* rdram, recomp_context* ctx) {
    uint32_t src = (uint32_t)ctx->r4;
    uint32_t dst = (uint32_t)ctx->r5;
    if (getenv("HH_LDTRACE") != nullptr || getenv("HH_TBLTRACE") != nullptr) {
        fprintf(stderr, "[LD384] a0=%08X a1=%08X a2=%08X a3=%08X vi=%llu gframe=%llu s=%llu ra=%08X\n",
                src, dst, (unsigned)ctx->r6, (unsigned)ctx->r7,
                (unsigned long long)hh_get_vi_count(),
                (unsigned long long)hh_get_game_frame_count(),
                (unsigned long long)hh_replay_get_sample(), (unsigned)ctx->r31);
    }
    hh_trans_load(rdram, ctx, hh_real_FUN_80003824);
    // HH: tras la descompresion, el byte en 0x801CC8C4 (offset 0xD724 de la base 0x801BF1A0) dice
    // si el "estado" del selector es DATO del modulo cargado.
    if (getenv("HH_TBLTRACE") != nullptr) {
        uint32_t b = (dst == 0x801BF1A0u) ? (uint32_t)rdram[(0x1CC8C4 ^ 3) & 0x7FFFFF] : 0xFFFFFFFFu;
        fprintf(stderr, "[LD384] post dst=%08X 0x801CC8C4=%02X\n", dst, (unsigned)(b & 0xFF));
    }
    recomp::overlays::load_module_by_source(src, (int32_t)dst);
}
// HH: loader STREAMED FUN_80004838(id, dest): carga el fichero un trozo por llamada (su propio
// descompresor 0x80003F44). El juego lo usa para el FICHERO 57 (combate), que se solapa con el 56
// (exploracion) y NO pasa por FUN_80003824. Al completarse (r2 != 0) registramos el modulo en su
// base. Sin esto, 0x80379410 resolvia a M55_FUN_80379410 (mid-funcion del 56) -> fuga de pila.
// Ver notes/2026-09-20-nodo-8005bf14-origen-y-captura.md.
static recomp_func_t* hh_real_FUN_80004838 = nullptr;
static uint32_t hh_stream_id_to_src(uint32_t id) {
    switch (id) {
        case 57: return 0x69E416u; // .module56 (fichero 57, combate)
        default: return 0;
    }
}
static void hh_wrap_FUN_80004838(uint8_t* rdram, recomp_context* ctx) {
    uint32_t id = (uint32_t)ctx->r4, dst = (uint32_t)ctx->r5;
    hh_real_FUN_80004838(rdram, ctx);
    if (ctx->r2 != 0) {
        uint32_t src = hh_stream_id_to_src(id);
        if (src != 0) {
            if (getenv("HH_TBLTRACE") != nullptr) {
                fprintf(stderr, "[STREAM] id=%u dst=%08X -> registrar src=%06X\n", id, dst, src);
            }
            recomp::overlays::load_module_by_source(src, (int32_t)dst);
        }
    }
}
// HH: cadena del nodo de boot: setter de callback (FUN_800058DC), dispatcher y pasos del
// módulo 23 que deben avanzar 0x801CFE00/02.
extern "C" int hh_get_callring(recomp_context* c, uint32_t* out, int max);
// HH: historial largo de llamadas (hh_ring2, por hilo) para ver la recursion/reentrada de la cadena.
extern "C" void hh_ring2_dump_last(FILE* f, int count);
// HH: al ver el veneno 0xFFFF84CD/0xFF7F84CD en a1, volcar la PILA GUEST (cadena de retornos) y el
// anillo de llamadas a hh_venom.log. Es la via barata (solo el setter, ~1400 llamadas) para localizar
// quien dispara el disable. OJO: comparar truncando a 32 bits. ADD32 firma-extiende a 64 bits
// (0xFFFFFFFFFFFF84CD != 0xFFFF84CD), de modo que la comparacion directa nunca casaba: causa de que
// hh_venom.log saliera vacio pese a que el wrapper si se ejecutaba.
static void hh_dump_venom(const char* tag, uint8_t* rdram, recomp_context* ctx) {
    if ((uint32_t)ctx->r5 != 0xFFFF84CDu && (uint32_t)ctx->r5 != 0xFF7F84CDu) return;
    static FILE* vf = nullptr;
    static int vn = 0;
    if (vf == nullptr) vf = fopen("hh_venom.log", "w");
    if (vf == nullptr || vn >= 64) return;
    vn++;
    uint32_t sp = (uint32_t)ctx->r29;
    uint32_t a0 = (uint32_t)ctx->r4, a2 = (uint32_t)ctx->r6, a3 = (uint32_t)ctx->r7;
    fprintf(vf, "=== VENOM #%d [%s] obj=%08X cb=%08X a2=%08X a3=%08X ra=%08X sp=%08X ===\n",
            vn, tag, a0, (uint32_t)ctx->r5, a2, a3, (uint32_t)ctx->r31, sp);
    fprintf(vf, "  stack-RA:");
    int cnt = 0;
    for (uint32_t a = sp; a < sp + 0x600u && a + 4 <= 0x80800000u; a += 4) {
        uint32_t v = *(uint32_t*)(rdram + (a - 0x80000000u));
        if (v >= 0x80000400u && v < 0x80800000u) {
            if ((cnt % 5) == 0) fprintf(vf, "\n    +%03X:", a - sp);
            fprintf(vf, " %08X", v);
            if (++cnt >= 60) break;
        }
    }
    fprintf(vf, "\n");
    uint32_t ring[16];
    int rn = hh_get_callring(ctx, ring, 16);
    if (rn > 0) {
        fprintf(vf, "  callring:");
        for (int k = 0; k < rn; k++) fprintf(vf, " %08X", ring[k]);
        fprintf(vf, "\n");
    }
    // HH: historial largo del hilo actual (hasta 512 entradas) para ver la recursion/reentrada de la
    // cadena del disable (los 12 venenos tienen sp a +0x58). Formato: "target sp" por linea.
    fprintf(vf, "  ring2-last512:\n");
    hh_ring2_dump_last(vf, 512);
    fflush(vf);
}
static recomp_func_t* hh_real_FUN_800058dc = nullptr;
static void hh_wrap_FUN_800058dc(uint8_t* rdram, recomp_context* ctx) {
    if (getenv("HH_TBLTRACE") != nullptr) {
        fprintf(stderr, "[SETCB] obj=%08X cb=%08X\n", (unsigned)ctx->r4, (unsigned)ctx->r5);
    }
    // HH: quien PUBLICA el handler del disable (M10_FUN_8021b280) en el objeto del CaC. Al verlo en
    // a1, volcar el contexto del llamante + anillo a hh_b280set.log. Gated HH_B280TRACE.
    if ((uint32_t)ctx->r5 == 0x8021B280u && getenv("HH_B280TRACE") != nullptr) {
        static FILE* sf = nullptr;
        static int sn = 0;
        if (sf == nullptr) sf = fopen("hh_b280set.log", "w");
        if (sf != nullptr && sn < 32) {
            sn++;
            // HH: direccion de retorno NATIVA del llamante (la que invoca el puntero del wrapper),
            // relativa a la propia funcion para poder simbolizarla con nm/addr2line del binario.
            void* nret = HH_RETURN_ADDR();
            long long rel = (long long)((intptr_t)nret - (intptr_t)&hh_wrap_FUN_800058dc);
            fprintf(sf, "=== SET b280 #%d obj=%08X sp=%08X ra=%08X a2=%08X a3=%08X host_rel=%lld ===\n",
                    sn, (unsigned)ctx->r4, (unsigned)ctx->r29, (unsigned)ctx->r31,
                    (unsigned)ctx->r6, (unsigned)ctx->r7, rel);
            uint32_t ring[16];
            int rn = hh_get_callring(ctx, ring, 16);
            if (rn > 0) {
                fprintf(sf, "  callring:");
                for (int k = 0; k < rn; k++) fprintf(sf, " %08X", ring[k]);
                fprintf(sf, "\n");
            }
            fflush(sf);
        }
    }
    hh_dump_venom("800058dc", rdram, ctx);
    // HH: workaround OPT-IN HH_NO_B280=1 ("forzar la ruta del emulador"). Ignora la PUBLICACION del
    // handler 0x8021B280 en el setter: M10_FUN_8021b240 corre (y su gate), pero b280 nunca queda
    // instalado ni se ejecuta. El emulador jamás ejecuta b280/M55_FUN_80379410 con el mismo input
    // (contralado 0 veces), asi que este hook reproduce su conducta un paso aguas arriba del sentinel
    // (HH_NO_DISABLE solo ignoraba la escritura final y no bastaba). Ver notes/2026-09-19-*.
    if (getenv("HH_NO_B280") != nullptr && (uint32_t)ctx->r5 == 0x8021B280u) {
        static int nb = 0;
        if (nb < 20) {
            nb++;
            fprintf(stderr, "[NO_B280] obj=%08X handler=%08X (publicacion ignorada)\n",
                    (unsigned)ctx->r4, (unsigned)ctx->r5);
        }
        return;
    }
    // HH: workaround OPT-IN HH_NO_DISABLE=1. El sentinel 0xFFFF84CD/0xFF7F84CD es el "disable" que el
    // emulador NUNCA aplica (mantiene el callback sano, p. ej. 801CB71C). Ignorar esa escritura evita
    // que el CaC quede envenenado y congele. NO es el fix de raiz (la divergencia es de timing); sirve
    // para validar en vivo si el disable es el unico bloqueo. Ver notes/2026-09-19-*.
    if (getenv("HH_NO_DISABLE") != nullptr &&
        ((uint32_t)ctx->r5 == 0xFFFF84CDu || (uint32_t)ctx->r5 == 0xFF7F84CDu)) {
        static int nd = 0;
        if (nd < 20) {
            nd++;
            fprintf(stderr, "[NO_DISABLE] obj=%08X cb=%08X (escritura ignorada)\n",
                    (unsigned)ctx->r4, (unsigned)ctx->r5);
        }
        return;
    }
    hh_real_FUN_800058dc(rdram, ctx);
}
// HH: FUN_800058f4 es el cuerpo general del setter (continuacion por fallthrough de 0x800058DC).
// Se engancha tambien por si algun modulo lo invoca por LOOKUP_FUNC (las llamadas directas del C
// generado, p. ej. la continuacion en funcs_1.c, no pasan por get_function).
static recomp_func_t* hh_real_FUN_800058f4 = nullptr;
static void hh_wrap_FUN_800058f4(uint8_t* rdram, recomp_context* ctx) {
    hh_dump_venom("800058f4", rdram, ctx);
    hh_real_FUN_800058f4(rdram, ctx);
}
// HH: traza de la puerta M7_FUN_80126CC0 (la que consulta M12_FUN_80242db4 para armar el contador
// [obj+0xA8] del callback M12_FUN_80242e90; el softlock tiene ese contador a 0). Gate HH_M7GATE=1,
// filtra a1==0x80127014 (el argumento que pasa M12_FUN_80242db4) y registra el retorno + las flags
// que mira la puerta (globales 0x188/0x181 y campos del objeto). Ver notes/2026-09-19-bat-stall-check.md.
static recomp_func_t* hh_real_M7_FUN_80126cc0 = nullptr;
static void hh_wrap_M7_FUN_80126cc0(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t a0 = (uint32_t)ctx->r4, a1 = (uint32_t)ctx->r5;
    hh_real_M7_FUN_80126cc0(rdram, ctx);
    static const bool on = getenv("HH_M7GATE") != nullptr;
    if (!on || a1 != 0x80127014u) return;
    auto r32 = [&](uint32_t a) -> uint32_t {
        return *(uint32_t*)(rdram + ((a - 0x80000000u) & 0x7FFFFF));
    };
    auto r16 = [&](uint32_t a) -> uint32_t {
        return *(uint16_t*)(rdram + (((a) ^ 2u) & 0x7FFFFF));
    };
    static int n = 0;
    if (n++ < 400) {
        uint32_t p38 = r32(a0 + 0x38);
        fprintf(stderr,
                "[M7GATE] #%d a0=%08X ret=%u 188=%08X 181=%02X p38=%08X p38+1E=%04X a0+36=%04X a0+2C=%08X\n",
                n, a0, (unsigned)ctx->r2, r32(0x801BBD78), r32(0x801BBD71) & 0xFFu,
                p38, r16(p38 + 0x1E), r16(a0 + 0x36), r32(a0 + 0x2C));
    }
}
// HH: traza de la PUERTA DEL DISABLE M7_FUN_80126A0C (la que consulta M10_FUN_8021b240 con
// (obj,0x39,1) para instalar b280). Gate HH_GATE_A=1; filtra a1==0x39. Registra retorno + las
// variables que mira la puerta. Comparar port<->emu para ver por que el port abre la rama M10/M12.
static recomp_func_t* hh_real_M7_FUN_80126a0c = nullptr;
static void hh_wrap_M7_FUN_80126a0c(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t a0 = (uint32_t)ctx->r4, a1 = (uint32_t)ctx->r5, a2 = (uint32_t)ctx->r6;
    hh_real_M7_FUN_80126a0c(rdram, ctx);
    static const bool on = getenv("HH_GATE_A") != nullptr;
    if (!on) return;
    static int total = 0, hits = 0;
    total++;
    if (a1 != 0x39u) return;
    auto r32 = [&](uint32_t a) -> uint32_t {
        return *(uint32_t*)(rdram + ((a - 0x80000000u) & 0x7FFFFF));
    };
    if (hits++ < 200) {
        fprintf(stderr,
                "[GATE_A] #%d/%d a0=%08X a1=%08X a2=%08X ret=%u 42D0=%08X 181=%02X 188=%08X mode7DD92=%08X\n",
                hits, total, a0, a1, a2, (unsigned)ctx->r2, r32(0x8008D580),
                r32(0x801BBD71) & 0xFFu, r32(0x801BBD78), r32(0x8017DD92));
    }
}
// HH: traza de la CADENA DE INSTALACION DE CALLBACKS del disable del CaC (gate HH_CHAINTRACE=1).
// Secuencia: M10_FUN_8021b1A8 -> (setter FUN_800058dc) instala M10_FUN_8021b200 -> instala
// M10_FUN_8021b240 -> (puerta 0x39) instala M10_FUN_8021b280 (disable). Cada paso lo dispara el
// scheduler FUN_80004bb0. Ver notes/2026-09-19-causa-raiz-cadencia-frames.md seccion 10.
static recomp_func_t* hh_real_M10_FUN_8021b1A8 = nullptr;
static recomp_func_t* hh_real_M10_FUN_8021b200 = nullptr;
static recomp_func_t* hh_real_M10_FUN_8021b240 = nullptr;
static recomp_func_t* hh_real_FUN_80004bb0 = nullptr;
static void hh_chain_log(const char* what, uint32_t a0, uint32_t a1, uint32_t extra) {
    static FILE* f = nullptr;
    if (f == nullptr) f = fopen("hh_chain.log", "w");
    if (f == nullptr) return;
    fprintf(f, "[CHAIN] vi=%llu sample=%llu %-10s a0=%08X a1=%08X extra=%08X\n",
            (unsigned long long)hh_get_vi_count(), (unsigned long long)hh_replay_get_sample(),
            what, a0, a1, extra);
    fflush(f);
}
static void hh_wrap_M10_FUN_8021b1A8(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t a0 = (uint32_t)ctx->r4, a1 = (uint32_t)ctx->r5;
    hh_chain_log("b1A8-in", a0, a1, 0);
    hh_real_M10_FUN_8021b1A8(rdram, ctx);
}
static void hh_wrap_M10_FUN_8021b200(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t a0 = (uint32_t)ctx->r4, a1 = (uint32_t)ctx->r5;
    hh_chain_log("b200-in", a0, a1, 0);
    hh_real_M10_FUN_8021b200(rdram, ctx);
}
static void hh_wrap_M10_FUN_8021b240(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t a0 = (uint32_t)ctx->r4, a1 = (uint32_t)ctx->r5;
    hh_chain_log("b240-in", a0, a1, 0);
    hh_real_M10_FUN_8021b240(rdram, ctx);
    hh_chain_log("b240-out", a0, a1, (uint32_t)ctx->r2);
}
// HH: instalador del disable (M10_FUN_8021b280). Mide la REINVOCACION: profundidad por hilo
// (thread_local) y contador por objeto. Si la cadena se reinvoca en bucle, aqui se ve (depth crece
// o el mismo obj se cuenta N veces). Salida: hh_b280call.log. Gate: HH_CHAINTRACE.
static recomp_func_t* hh_real_M10_FUN_8021b280 = nullptr;
static thread_local int hh_b280_depth = 0;
struct HhB280Count { uint32_t obj; uint32_t n; };
static HhB280Count hh_b280_cnt[16];
static int hh_b280_cnt_n = 0;
static void hh_b280_count(uint32_t obj) {
    for (int i = 0; i < hh_b280_cnt_n; i++) {
        if (hh_b280_cnt[i].obj == obj) { hh_b280_cnt[i].n++; return; }
    }
    if (hh_b280_cnt_n < 16) {
        hh_b280_cnt[hh_b280_cnt_n].obj = obj;
        hh_b280_cnt[hh_b280_cnt_n].n = 1;
        hh_b280_cnt_n++;
    }
}
static void hh_wrap_M10_FUN_8021b280(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t obj = (uint32_t)ctx->r4, a1 = (uint32_t)ctx->r5;
    hh_b280_count(obj);
    const int d = ++hh_b280_depth;
    static FILE* f = nullptr;
    static long n = 0;
    if (f == nullptr) f = fopen("hh_b280call.log", "w");
    if (f != nullptr && n < 500000) {
        n++;
        uint32_t cnt = 0;
        for (int i = 0; i < hh_b280_cnt_n; i++)
            if (hh_b280_cnt[i].obj == obj) { cnt = hh_b280_cnt[i].n; break; }
        fprintf(f, "[B280CALL] n=%ld vi=%llu tid=%d depth=%d obj=%08X a1=%08X sp=%08X ra=%08X objcount=%u\n",
                n, (unsigned long long)hh_get_vi_count(), hh_tid(rdram), d, obj, a1,
                (uint32_t)ctx->r29, (uint32_t)ctx->r31, cnt);
        fflush(f);
    }
    hh_real_M10_FUN_8021b280(rdram, ctx);
    hh_b280_depth = d - 1;
}
// HH: detector de FUGA DE PILA (desbalance de sp) por funcion. Se wrapean las funciones del driver/
// cadena del disable (las del anillo del veneno). Si una funcion retorna con sp != entry, lo loguea
// a hh_spchk.log con el delta. La deriva observada es +0x58 por frame: aqui se localiza quien la
// causa. Gate: HH_CHAINTRACE.
static FILE* hh_spchk_fp = nullptr;
static void hh_spchk_log(const char* name, uint32_t sp0, uint32_t sp1, uint32_t ra) {
    if (sp0 == sp1 || sp0 == 0) return;
    if (hh_spchk_fp == nullptr) hh_spchk_fp = fopen("hh_spchk.log", "w");
    if (hh_spchk_fp == nullptr) return;
    fprintf(hh_spchk_fp, "[SPCHK] %-24s entry=%08X exit=%08X delta=%+d ra=%08X\n",
            name, sp0, sp1, (int)(sp1 - sp0), ra);
    fflush(hh_spchk_fp);
}
#define HH_SPCHK(NAME) \
    static recomp_func_t* hh_real_##NAME = nullptr; \
    static void hh_wrap_##NAME(uint8_t* rdram, recomp_context* ctx) { \
        uint32_t s0 = (uint32_t)ctx->r29; \
        hh_real_##NAME(rdram, ctx); \
        hh_spchk_log(#NAME, s0, (uint32_t)ctx->r29, (uint32_t)ctx->r31); \
    }
HH_SPCHK(FUN_80006214)
HH_SPCHK(FUN_8001f718)
HH_SPCHK(M7_FUN_801277b0)
HH_SPCHK(M7_FUN_8012c9c0)
HH_SPCHK(M7_FUN_8012ce10)
HH_SPCHK(M7_FUN_8012b1e4)
HH_SPCHK(M7_FUN_8012bfa0)
HH_SPCHK(M10_FUN_8022c7a4)
HH_SPCHK(M10_FUN_8022c5ac)
HH_SPCHK(M10_FUN_8022c314)
HH_SPCHK(M10_FUN_8022c478)
// HH: `FUN_80001454` (frame) y `FUN_80001BB0` (no-op) ya tienen wrapper propio (más abajo).
HH_SPCHK(FUN_8000290C)
// HH: llamantes directos de FUN_80001454 (frame) — para localizar la fuga 0x58/frame del hilo 5.
HH_SPCHK(FUN_80000ec8)
HH_SPCHK(FUN_80001060)
HH_SPCHK(FUN_80001b30)
HH_SPCHK(FUN_80001bc0)
HH_SPCHK(FUN_80001d5c)
HH_SPCHK(FUN_800021b4)
HH_SPCHK(FUN_8000433c)
HH_SPCHK(FUN_80006790)
HH_SPCHK(FUN_80006af0)
HH_SPCHK(FUN_8001e978)
HH_SPCHK(FUN_8001eaa4)
HH_SPCHK(FUN_80026e58)
HH_SPCHK(FUN_80026f58)
HH_SPCHK(FUN_80029fa0)
HH_SPCHK(FUN_80032890)
HH_SPCHK(FUN_80034ab8)
HH_SPCHK(FUN_80034c24)
HH_SPCHK(M7_FUN_80126744)
HH_SPCHK(M7_FUN_80126944)
HH_SPCHK(FUN_8012fe50)
HH_SPCHK(FUN_80133aa0)
HH_SPCHK(FUN_8001f76c)
// HH: FIX de la fuga de pila del cluster M55 (0x80379410/424/444/464). En ROM son mid-entries (sin
// prologo -0x58) del contenedor M55_FUN_80379244, con epilogo COMPARTIDO 0x8037948C (sp += 0x58).
// El fallthrough interno (0x80379244 -> directo) NO pasa por get_function/wrappers; las entradas
// EXTERNAS (jal desde M10, o callback via FUN_80005270 `jalr [obj+0x18]`) si, y retornan con sp
// +0x58 (fuga/frame -> pisa FUN_800011b0 / nodo 0x8005BF14). Con HH_M55SPFIX=1 se restaura sp.
// NO se envuelve el epilogo 0x8037948C (rompería el camino interno).
// Ver notes/2026-09-20-nodo-8005bf14-origen-y-captura.md §10.
#define HH_M55SPFIX_WRAP(NAME) \
    static recomp_func_t* hh_real_##NAME = nullptr; \
    static void hh_wrap_##NAME(uint8_t* rdram, recomp_context* ctx) { \
        uint32_t s0 = (uint32_t)ctx->r29; \
        hh_real_##NAME(rdram, ctx); \
        hh_spchk_log(#NAME, s0, (uint32_t)ctx->r29, (uint32_t)ctx->r31); \
        if (getenv("HH_M55SPFIX") != nullptr) { \
            static FILE* f = nullptr; \
            static long n = 0; \
            if (f == nullptr) f = fopen("hh_m55spfix.log", "w"); \
            if (f != nullptr && n < 500000) { \
                n++; \
                fprintf(f, "[M55SPFIX] %s n=%ld vi=%llu sp=%08X->%08X (restaurado)\n", \
                        #NAME, n, (unsigned long long)hh_get_vi_count(), s0, (uint32_t)ctx->r29); \
                fflush(f); \
            } \
            ctx->r29 = s0; \
        } \
    }
HH_M55SPFIX_WRAP(M55_FUN_80379410)
HH_M55SPFIX_WRAP(M55_FUN_80379424)
HH_M55SPFIX_WRAP(M55_FUN_80379444)
HH_M55SPFIX_WRAP(M55_FUN_80379464)
// HH: scheduler de eventos temporizados (FUN_80004bb0). Loguea a0 (id), a1 y los tiempos que
// gobiernan el disparo (0x42CC/0x42BC/0x42D0). Un unico call-site real: M7_FUN_80125808.
static void hh_wrap_FUN_80004bb0(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t a0 = (uint32_t)ctx->r4, a1 = (uint32_t)ctx->r5;
    auto r32 = [&](uint32_t a) -> uint32_t {
        return *(uint32_t*)(rdram + ((a - 0x80000000u) & 0x7FFFFF));
    };
    static FILE* f = nullptr;
    if (f == nullptr) f = fopen("hh_sched.log", "w");
    if (f != nullptr) {
        fprintf(f, "[SCHED] vi=%llu sample=%llu a0=%08X a1=%08X 42CC=%08X 42BC=%08X 42D0=%08X 42C8=%02X\n",
                (unsigned long long)hh_get_vi_count(), (unsigned long long)hh_replay_get_sample(),
                a0, a1, r32(0x8008D57C), r32(0x8008D56C), r32(0x8008D580), r32(0x8008D578) & 0xFF);
        fflush(f);
    }
    hh_real_FUN_80004bb0(rdram, ctx);
}
// HH: dispatcher de una entrada temporizada (FUN_80004d20(handler=a0, target=a1)) y "add" del
// scheduler (FUN_80004adc). Captura que handler se dispara (p.ej. la cadena 0x8021Bxxx del disable).
static recomp_func_t* hh_real_FUN_80004d20 = nullptr;
static recomp_func_t* hh_real_FUN_80004adc = nullptr;
static void hh_scheddisp_log(const char* what, uint32_t handler, uint32_t target) {
    static FILE* f = nullptr;
    if (f == nullptr) f = fopen("hh_scheddisp.log", "w");
    if (f == nullptr) return;
    fprintf(f, "[DISP] vi=%llu sample=%llu %-8s handler=%08X target=%08X\n",
            (unsigned long long)hh_get_vi_count(), (unsigned long long)hh_replay_get_sample(),
            what, handler, target);
    fflush(f);
}
static void hh_wrap_FUN_80004d20(uint8_t* rdram, recomp_context* ctx) {
    hh_scheddisp_log("run", (uint32_t)ctx->r4, (uint32_t)ctx->r5);
    hh_real_FUN_80004d20(rdram, ctx);
}
static void hh_wrap_FUN_80004adc(uint8_t* rdram, recomp_context* ctx) {
    hh_scheddisp_log("add", (uint32_t)ctx->r4, (uint32_t)ctx->r5);
    hh_real_FUN_80004adc(rdram, ctx);
}
static recomp_func_t* hh_real_FUN_80005270 = nullptr;
static void hh_wrap_FUN_80005270(uint8_t* rdram, recomp_context* ctx) {
    auto r32 = [&](uint32_t a){ return *(uint32_t*)&rdram[a & 0x7FFFFF]; };
    uint32_t head = r32(0x80089378);
    if (head != 0) {
        fprintf(stderr, "[DISP] head=%08X n=%08X f18=%08X f1C=%08X f20=%08X\n",
                head, r32(head), r32(head + 0x18), r32(head + 0x1C), r32(head + 0x20));
    } else {
        fprintf(stderr, "[DISP] head=0\n");
    }
    hh_real_FUN_80005270(rdram, ctx);
}
// HH: balance de sp del dispatcher (hereda la fuga si el callback es un mid-entry M55 sin arreglar).
// Loguea los callbacks de objeto que despacha ([listhead+0x18/0x1C/0x20]) para identificar el que fuga.
static void hh_wrap_FUN_80005270_spchk(uint8_t* rdram, recomp_context* ctx) {
    uint32_t s0 = (uint32_t)ctx->r29;
    auto r32 = [&](uint32_t a){ return *(uint32_t*)&rdram[a & 0x7FFFFF]; };
    if (getenv("HH_M55SPFIX") != nullptr) {
        static FILE* f = nullptr;
        static long n = 0;
        if (f == nullptr) f = fopen("hh_disp.log", "w");
        if (f != nullptr && n < 4000) {
            n++;
            uint32_t head = r32(0x80089378);
            fprintf(f, "[DISP] n=%ld vi=%llu a0=%08X head=%08X", n,
                    (unsigned long long)hh_get_vi_count(), (uint32_t)ctx->r4, head);
            uint32_t it = head;
            for (int i = 0; i < 4 && it != 0; i++, it = r32(it)) {
                fprintf(f, " [%08X 18=%08X 1C=%08X 20=%08X]", it,
                        r32(it + 0x18), r32(it + 0x1C), r32(it + 0x20));
            }
            fprintf(f, "\n");
            fflush(f);
        }
    }
    hh_real_FUN_80005270(rdram, ctx);
    hh_spchk_log("FUN_80005270", s0, (uint32_t)ctx->r29, (uint32_t)ctx->r31);
    // HH: el dispatcher debe tener neto sp=0 (prologo -0x40 / epilogo +0x40). La fuga +0x58 viene del
    // callback (`jalr [obj+0x18/0x1C/0x20]`, mid-entry sin prologo). Restauramos sp para neutralizarla.
    if (getenv("HH_M55SPFIX") != nullptr && (uint32_t)ctx->r29 != s0) {
        static FILE* f = nullptr;
        static long n = 0;
        if (f == nullptr) f = fopen("hh_m55spfix.log", "a");
        if (f != nullptr && n < 500000) {
            n++;
            fprintf(f, "[DISP-SPFIX] n=%ld vi=%llu sp=%08X->%08X (restaurado)\n",
                    n, (unsigned long long)hh_get_vi_count(), s0, (uint32_t)ctx->r29);
            fflush(f);
        }
        ctx->r29 = s0;
    }
}
static recomp_func_t* hh_real_FUN_801CBE88 = nullptr;
static void hh_wrap_FUN_801CBE88(uint8_t* rdram, recomp_context* ctx) {
    fprintf(stderr, "[M23] FUN_801CBE88 a0=%08X a1=%08X\n", (unsigned)ctx->r4, (unsigned)ctx->r5);
    hh_real_FUN_801CBE88(rdram, ctx);
}
static recomp_func_t* hh_real_FUN_801CBDC0 = nullptr;
static void hh_wrap_FUN_801CBDC0(uint8_t* rdram, recomp_context* ctx) {
    fprintf(stderr, "[M23] FUN_801CBDC0 a0=%08X a1=%08X\n", (unsigned)ctx->r4, (unsigned)ctx->r5);
    hh_real_FUN_801CBDC0(rdram, ctx);
}
static recomp_func_t* hh_real_FUN_801BF1CC = nullptr;
static void hh_wrap_FUN_801BF1CC(uint8_t* rdram, recomp_context* ctx) {
    fprintf(stderr, "[M23] FUN_801BF1CC a0=%08X a1=%08X\n", (unsigned)ctx->r4, (unsigned)ctx->r5);
    hh_real_FUN_801BF1CC(rdram, ctx);
}
// HH: contador de frames de juego (0x80001454) para comparar la cadencia del front-end port<->emu
// (el port consume 1 muestra de replay por get_input; el emu ~1 muestra/VI en el front-end). Ver
// notes/2026-09-19-inventario-y-nueva-evidencia-fase-previa.md.
static std::atomic<uint64_t> hh_game_frame_count{0};
extern "C" uint64_t hh_get_game_frame_count() { return hh_game_frame_count.load(); }
// HH: callback del objeto de transicion 0x801D0474 (front-end). Mide su cadencia (VI) y el
// registro de flancos de input [0x80089478] que lee via 801C1334. Gated por HH_LSTTRACE.
static recomp_func_t* hh_real_FUN_801C1508 = nullptr;
static void hh_wrap_FUN_801C1508(uint8_t* rdram, recomp_context* ctx) {
    if (getenv("HH_LSTTRACE") != nullptr) {
        auto rh = [&](uint32_t a){ return (uint32_t)*(uint16_t*)&rdram[(a ^ 2) & 0x7FFFFF]; };
        fprintf(stderr, "[CB1508] vi=%llu gframe=%llu f89478=%04X\n",
            (unsigned long long)hh_get_vi_count(),
            (unsigned long long)hh_get_game_frame_count(), rh(0x80089478));
    }
    hh_real_FUN_801C1508(rdram, ctx);
}
extern "C" unsigned long long hh_get_input_polls(void);
// HH: wrapper directo del frame del bucle de juego (0x80001454) para medir su cadencia real
// (frames/s y VI/frame) sin el posible doble conteo de get_function. Gated por HH_FRAMERATE.
static recomp_func_t* hh_real_FUN_80001454 = nullptr;
static void hh_wrap_FUN_80001454(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t s0 = (uint32_t)ctx->r29;
    const uint64_t n = hh_game_frame_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (getenv("HH_FRAMERATE") != nullptr) {
        // HH: los primeros 40 frames con VI, para ver dVI/frame (¿2 VI/frame como el original?).
        if (n <= 40) {
            fprintf(stderr, "[FRM] n=%llu vi=%llu in=%llu\n", (unsigned long long)n,
                    (unsigned long long)hh_get_vi_count(), (unsigned long long)hh_get_input_polls());
        }
        static const auto t0 = std::chrono::steady_clock::now();
        static auto last_log = t0;
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - last_log).count() >= 1.0) {
            last_log = now;
            fprintf(stderr, "[FRM] t=%.2f frame=%llu vi=%llu in=%llu\n",
                    std::chrono::duration<double>(now - t0).count(), (unsigned long long)n,
                    (unsigned long long)hh_get_vi_count(), (unsigned long long)hh_get_input_polls());
        }
    }
    hh_real_FUN_80001454(rdram, ctx);
    hh_spchk_log("FUN_80001454", s0, (uint32_t)ctx->r29, (uint32_t)ctx->r31);
}
// HH: rama no-op del bucle principal (0x80001BB0), para medir el reparto frame/noop por iteracion.
static recomp_func_t* hh_real_FUN_80001BB0 = nullptr;
static void hh_wrap_FUN_80001BB0(uint8_t* rdram, recomp_context* ctx) {
    if (getenv("HH_FRAMERATE") != nullptr) {
        static const auto t0 = std::chrono::steady_clock::now();
        static auto last_log = t0;
        static uint64_t n = 0;
        ++n;
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - last_log).count() >= 1.0) {
            last_log = now;
            fprintf(stderr, "[NOOP] t=%.2f n=%llu vi=%llu\n",
                    std::chrono::duration<double>(now - t0).count(), (unsigned long long)n,
                    (unsigned long long)hh_get_vi_count());
        }
    }
    hh_real_FUN_80001BB0(rdram, ctx);
}
static recomp_func_t* hh_real_FUN_801C2050 = nullptr; // callback instalado en 0x801D0474 (transicion)
static void hh_wrap_FUN_801C2050(uint8_t* rdram, recomp_context* ctx) {
    static uint64_t n = 0;
    if (n < 10 || (n % 100) == 0) {
        fprintf(stderr, "[M23] FUN_801C2050 n=%llu a0=%08X a1=%08X a2=%08X a3=%08X ra=%08X\n",
            (unsigned long long)n, (unsigned)ctx->r4, (unsigned)ctx->r5, (unsigned)ctx->r6,
            (unsigned)ctx->r7, (unsigned)ctx->r31);
    }
    n++;
    hh_real_FUN_801C2050(rdram, ctx);
}
static recomp_func_t* hh_real_FUN_80124C54 = nullptr; // callback del nodo 0x801CFE20
static void hh_wrap_FUN_80124C54(uint8_t* rdram, recomp_context* ctx) {
    static uint64_t n = 0;
    if (n < 8 || (n % 500) == 0) {
        // 0x801BBD56 = byte que decide M7_FUN_8012FD1C (lbu 0x166 de 0x801BBBF0) -> 1 = seguir
        // esperando la escena, 0 = fin de espera (transicion). Comparar port/emu en el mismo VI.
        fprintf(stderr, "[M23] FUN_80124C54 n=%llu vi=%llu a0=%08X a1=%08X bd6d=%02X bd6e=%02X bc1c=%04X bd56=%02X\n",
            (unsigned long long)n, (unsigned long long)hh_get_vi_count(),
            (unsigned)ctx->r4, (unsigned)ctx->r5,
            (unsigned)rdram[(0x801BBD6D ^ 3) & 0x7FFFFF], (unsigned)rdram[(0x801BBD6E ^ 3) & 0x7FFFFF],
            (unsigned)*(uint16_t*)&rdram[(0x801BBC1C ^ 2) & 0x7FFFFF],
            (unsigned)rdram[(0x801BBD56 ^ 3) & 0x7FFFFF]);
    }
    n++;
    hh_real_FUN_80124C54(rdram, ctx);
}
static recomp_func_t* hh_real_FUN_8012FD1C = nullptr; // condicion que instala 80124CEC
static void hh_wrap_FUN_8012FD1C(uint8_t* rdram, recomp_context* ctx) {
    uint32_t a0 = (uint32_t)ctx->r4;
    // 0x801BBD56 = t6 leido por M7_FUN_8012fd28 (lbu 0x166 de 0x801BBBF0): decide la rama.
    uint32_t t6 = (uint32_t)rdram[(0x801BBD56 ^ 3) & 0x7FFFFF];
    hh_real_FUN_8012FD1C(rdram, ctx);
    static int n = 0;
    if (n < 30 || ((n % 500) == 0 && (uint32_t)ctx->r2 != 0)) {
        fprintf(stderr, "[M23] FUN_8012FD1C n=%d vi=%llu a0=%08X t6=%u -> %08X\n", n,
            (unsigned long long)hh_get_vi_count(), a0, t6, (unsigned)ctx->r2);
    }
    n++;
}
static recomp_func_t* hh_real_FUN_8012FE50 = nullptr; // escribe bd6d (gate: bd6d==0 && [0x8008D550]==0)
static void hh_wrap_FUN_8012FE50(uint8_t* rdram, recomp_context* ctx) {
    auto rb = [&](uint32_t a){ return (uint32_t)rdram[(a ^ 3) & 0x7FFFFF]; };
    uint32_t pre = rb(0x801BBD6D);
    uint32_t d550 = rb(0x8008D550);
    uint32_t a0 = (uint32_t)ctx->r4, a1 = (uint32_t)ctx->r5, a2 = (uint32_t)ctx->r6, a3 = (uint32_t)ctx->r7;
    hh_real_FUN_8012FE50(rdram, ctx);
    static int n = 0;
    if (n < 40) {
        fprintf(stderr, "[M23] FUN_8012FE50 n=%d a0=%02X a1=%04X a2=%02X a3=%02X bd6d_pre=%02X d550=%02X bd6d_post=%02X\n",
            n, a0 & 0xFF, a1 & 0xFFFF, a2 & 0xFF, a3 & 0xFF, pre, d550, (unsigned)rb(0x801BBD6D));
    }
    n++;
}
static recomp_func_t* hh_real_FUN_801C5A0C = nullptr; // continuacion (fallthrough) de FUN_801C5A00
static void hh_wrap_FUN_801C5A0C(uint8_t* rdram, recomp_context* ctx) {
    static int n = 0;
    if (n < 20) {
        auto rh = [&](uint32_t a){ return (uint32_t)*(uint16_t*)&rdram[(a ^ 2) & 0x7FFFFF]; };
        fprintf(stderr, "[M23] FUN_801C5A0C n=%d idx=%u\n", n, rh(0x801BBD68));
    }
    n++;
    hh_real_FUN_801C5A0C(rdram, ctx);
}
static int hh_ms_now() {
    static const auto t0 = std::chrono::steady_clock::now();
    return (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}
// HH: listas de carga de recursos (CaC). FUN_80004310 registra un puntero en 0x800894F4[cnt] (cnt
// en byte 0x8008946D); FUN_8000433C las procesa (hasta 8 entradas de 8 bytes por llamada) y carga
// cada id con FUN_800045E8/80004664/80004484. Evidencia del diferencial de la carga #22.
static bool hh_is_burst_id(uint32_t id) {
    static const uint16_t ids[] = {330,348,180,189,331,333,334,335,336,181,184,127,130,133,152};
    for (uint16_t v : ids) if (id == v) return true;
    return false;
}
static recomp_func_t* hh_real_FUN_80004310 = nullptr;
static void hh_wrap_FUN_80004310(uint8_t* rdram, recomp_context* ctx) {
    uint32_t ptr = (uint32_t)ctx->r4;
    if (getenv("HH_LSTTRACE") != nullptr) {
        auto r32 = [&](uint32_t a){ return *(uint32_t*)&rdram[a & 0x7FFFFF]; };
        uint32_t cnt = (uint32_t)rdram[(0x8008946D ^ 3) & 0x7FFFFF];
        fprintf(stderr, "[LST] t=%d REG ptr=%08X cnt=%u ra=%08X\n", hh_ms_now(), ptr, cnt, (unsigned)ctx->r31);
        for (int i = 0; i < 16; ++i) {
            uint32_t e0 = r32(ptr + i * 8), e1 = r32(ptr + i * 8 + 4);
            fprintf(stderr, "[LST]   e[%d] %08X %08X (id=%04X arg=%08X)%s\n", i, e0, e1,
                    e0 & 0xFFFF, e1, (e0 & 0x40000000) ? " END" : "");
            if (e0 & 0x40000000) break;
        }
    }
    hh_real_FUN_80004310(rdram, ctx);
}
// HH: cargador de recursos de la seccion 7 (M24_FUN_801c242c): a0=indice (tabla 0x80171CF0),
// a1=salida. Carga 2 ids del descriptor [tabla[a0-1]]. Es el llamante de la rafaga #22 del CaC.
static void hh_log_m24(uint8_t* rdram, recomp_context* ctx, const char* tag) {
    auto r32 = [&](uint32_t a){ return *(uint32_t*)&rdram[a & 0x7FFFFF]; };
    auto r16 = [&](uint32_t a){ return (uint32_t)*(uint16_t*)&rdram[(a ^ 2) & 0x7FFFFF]; };
    uint32_t idx = (uint32_t)ctx->r4, arg = (uint32_t)ctx->r5;
    uint32_t desc = r32(0x80171CF0 + idx * 4 - 4);
    uint32_t p = desc ? r32(desc) : 0;
    uint32_t id1 = p ? (r16(p) & 0xFFFF) : 0xFFFF, id2 = p ? (r16(p + 2) & 0xFFFF) : 0xFFFF;
    fprintf(stderr, "[LST] t=%d vi=%llu %s idx=%u arg=%08X p=%08X id1=%u id2=%u\n",
            hh_ms_now(), (unsigned long long)hh_get_vi_count(), tag, idx, arg, p, id1, id2);
}
static recomp_func_t* hh_real_M24_FUN_801c2420 = nullptr;
static void hh_wrap_M24_FUN_801c2420(uint8_t* rdram, recomp_context* ctx) {
    if (getenv("HH_LSTTRACE") != nullptr) hh_log_m24(rdram, ctx, "M24C@2420");
    hh_real_M24_FUN_801c2420(rdram, ctx);
}
static recomp_func_t* hh_real_M24_FUN_801c242c = nullptr;
static void hh_wrap_M24_FUN_801c242c(uint8_t* rdram, recomp_context* ctx) {
    if (getenv("HH_LSTTRACE") != nullptr) hh_log_m24(rdram, ctx, "M24C@242c");
    hh_real_M24_FUN_801c242c(rdram, ctx);
}
// HH: driver de la linea temporal de escena (M24_FUN_801bfaa0) y sus ayudantes. g2=[0x801D8CE8]
// selecciona el "periodo"; el incrementador M24_FUN_801bffac avanza de periodo (aqui murio el CaC).
static recomp_func_t* hh_real_M24_FUN_801bfaa0 = nullptr;
static void hh_wrap_M24_FUN_801bfaa0(uint8_t* rdram, recomp_context* ctx) {
    if (getenv("HH_LSTTRACE") != nullptr) {
        auto r32 = [&](uint32_t a){ return *(uint32_t*)&rdram[a & 0x7FFFFF]; };
        uint32_t tl = r32(0x801D8CE4), g2 = r32(0x801D8CE8), cnt = r32(0x801D8CF0);
        uint32_t tlbase = r32(0x801D8C00 + tl * 4);
        uint32_t per = tlbase ? r32(tlbase + g2 * 4) : 0;
        uint32_t ent = per ? r32(per + cnt * 0x18) : 0, val = per ? r32(per + cnt * 0x18 + 4) : 0;
        static unsigned long long n = 0;
        if (n < 40 || (n % 200) == 0) {
            fprintf(stderr, "[TL] t=%d vi=%llu n=%llu tl=%u g2=%u cnt=%u per=%08X ent=%08X val=%08X\n",
                hh_ms_now(), (unsigned long long)hh_get_vi_count(), n, tl, g2, cnt, per, ent, val);
        }
        n++;
    }
    hh_real_M24_FUN_801bfaa0(rdram, ctx);
}
static recomp_func_t* hh_real_M24_FUN_801bffac = nullptr;
static void hh_wrap_M24_FUN_801bffac(uint8_t* rdram, recomp_context* ctx) {
    if (getenv("HH_LSTTRACE") != nullptr) {
        auto r32 = [&](uint32_t a){ return *(uint32_t*)&rdram[a & 0x7FFFFF]; };
        fprintf(stderr, "[TL] t=%d vi=%llu ADVANCE g2: %u -> %u (cnt=%u)\n", hh_ms_now(),
            (unsigned long long)hh_get_vi_count(), r32(0x801D8CE8), r32(0x801D8CE8) + 1, r32(0x801D8CF0));
    }
    hh_real_M24_FUN_801bffac(rdram, ctx);
}
// HH: M24_FUN_801c0a30 fija la epoch de la linea temporal: 0x801D8D80/84 = retorno de
// M24_FUN_801C0C08 (osGetTime escalado). Traza HH_EPOCHTRACE para ver cuando/que valory por que el
// port la fija ~16 s antes que el emulador (nota 2026-09-19 Syntesis).
static recomp_func_t* hh_real_M24_FUN_801c0a30 = nullptr;
static void hh_wrap_M24_FUN_801c0a30(uint8_t* rdram, recomp_context* ctx) {
    auto r32 = [&](uint32_t a) -> uint32_t {
        return *(uint32_t*)(rdram + ((a - 0x80000000u) & 0x7FFFFF));
    };
    static const bool on = getenv("HH_EPOCHTRACE") != nullptr;
    if (on) {
        fprintf(stderr, "[EPOCH] ENTER vi=%llu epoch_prev=%08X%08X\n",
                (unsigned long long)hh_get_vi_count(), r32(0x801D8D80), r32(0x801D8D84));
    }
    hh_real_M24_FUN_801c0a30(rdram, ctx);
    if (on) {
        fprintf(stderr, "[EPOCH] SET vi=%llu epoch=%08X%08X (helper_ret=%08X:%08X)\n",
                (unsigned long long)hh_get_vi_count(), r32(0x801D8D80), r32(0x801D8D84),
                (unsigned)ctx->r2, (unsigned)ctx->r3);
    }
}
static recomp_func_t* hh_real_FUN_801C0B8C = nullptr;
static void hh_wrap_FUN_801C0B8C(uint8_t* rdram, recomp_context* ctx) {
    uint32_t a0 = (uint32_t)ctx->r4, a1 = (uint32_t)ctx->r5;
    hh_real_FUN_801C0B8C(rdram, ctx);
    if (getenv("HH_LSTTRACE") != nullptr) {
        static unsigned long long n = 0;
        if (n < 60 || (n % 300) == 0) {
            fprintf(stderr, "[TL] t=%d vi=%llu EVQCHECK n=%llu a0=%08X a1=%08X -> %08X\n",
                hh_ms_now(), (unsigned long long)hh_get_vi_count(), n, a0, a1, (unsigned)ctx->r2);
        }
        n++;
    }
}
static recomp_func_t* hh_real_M24_FUN_801bff20 = nullptr;
static void hh_wrap_M24_FUN_801bff20(uint8_t* rdram, recomp_context* ctx) {
    if (getenv("HH_LSTTRACE") != nullptr) fprintf(stderr, "[TL] t=%d vi=%llu PFF20\n",
        hh_ms_now(), (unsigned long long)hh_get_vi_count());
    hh_real_M24_FUN_801bff20(rdram, ctx);
}
// HH: cadena de decision del cambio de escena (M24_FUN_801bf398) y sus checks. En el port el flag
// [0x801D8CFC] se pone a 1 en VI~790 (emulador: nunca durante la transicion) -> adelanta la carga de
// modulos sobre el modulo 24. Aqui se registra que check pasa y cuando.
static recomp_func_t* hh_real_M24_FUN_801bf398 = nullptr;
static void hh_wrap_M24_FUN_801bf398(uint8_t* rdram, recomp_context* ctx) {
    auto r32 = [&](uint32_t a){ return *(uint32_t*)&rdram[a & 0x7FFFFF]; };
    uint32_t f478_before = r32(0x80089478);
    uint32_t before = r32(0x801D8CFC);
    hh_real_M24_FUN_801bf398(rdram, ctx);
    uint32_t f478_after = r32(0x80089478);
    if (getenv("HH_LSTTRACE") != nullptr && f478_after != f478_before) {
        fprintf(stderr, "[TL] t=%d vi=%llu F89478 %08X -> %08X (lhu=%04X b1000=%u cnt30=%u)\n",
            hh_ms_now(), (unsigned long long)hh_get_vi_count(), f478_before, f478_after,
            (f478_after >> 16) & 0xFFFFu, ((f478_after >> 16) & 0x1000u) != 0 ? 1u : 0u, r32(0x801D8DA8));
    }
    uint32_t after = r32(0x801D8CFC);
    if (getenv("HH_LSTTRACE") != nullptr && after != before) {
        fprintf(stderr, "[TL] t=%d vi=%llu FLAG 801D8CFC %u->%u tl=%08X f28=%08X cnt=%u ev=%08X\n",
            hh_ms_now(), (unsigned long long)hh_get_vi_count(), before, after,
            r32(0x801D8CE4), r32(0x801DE828), r32(0x801D8DA8), r32(0x801DE830));
    }
}
static void hh_chain_log(int slot, const char* name, uint32_t v, uint8_t* rdram) {
    static uint32_t last[8] = {0xEEEEEEEEu,0xEEEEEEEEu,0xEEEEEEEEu,0xEEEEEEEEu,0xEEEEEEEEu,0xEEEEEEEEu,0xEEEEEEEEu,0xEEEEEEEEu};
    uint64_t vi = hh_get_vi_count();
    bool window = (vi >= 780 && vi <= 835);
    if (slot < 0 || slot > 7 || (!window && last[slot] == v)) return;
    last[slot] = v;
    auto r32 = [&](uint32_t a){ return *(uint32_t*)&rdram[a & 0x7FFFFF]; };
    fprintf(stderr, "[TL] t=%d vi=%llu CHAIN %s -> %08X (tl=%08X f28=%08X cnt=%u ev=%08X flag=%u f00=%08X)\n",
        hh_ms_now(), (unsigned long long)vi, name, v,
        r32(0x801D8CE4), r32(0x801DE828), r32(0x801D8DA8), r32(0x801DE830), r32(0x801D8CFC) & 0xFF, r32(0x801D8D00));
}
static recomp_func_t* hh_real_M24_FUN_801c02fc = nullptr;
static void hh_wrap_M24_FUN_801c02fc(uint8_t* rdram, recomp_context* ctx) {
    hh_real_M24_FUN_801c02fc(rdram, ctx);
    if (getenv("HH_LSTTRACE") != nullptr) hh_chain_log(0, "CHK_02FC", (uint32_t)ctx->r2, rdram);
}
static recomp_func_t* hh_real_M24_FUN_801c0c68 = nullptr;
static void hh_wrap_M24_FUN_801c0c68(uint8_t* rdram, recomp_context* ctx) {
    hh_real_M24_FUN_801c0c68(rdram, ctx);
    if (getenv("HH_LSTTRACE") != nullptr) hh_chain_log(1, "CHK_0C68", (uint32_t)ctx->r2, rdram);
}
static recomp_func_t* hh_real_M24_FUN_801c012c = nullptr;
static void hh_wrap_M24_FUN_801c012c(uint8_t* rdram, recomp_context* ctx) {
    hh_real_M24_FUN_801c012c(rdram, ctx);
    if (getenv("HH_LSTTRACE") != nullptr) hh_chain_log(2, "CHK_012C", (uint32_t)ctx->r2, rdram);
}
static recomp_func_t* hh_real_M24_FUN_801c0190 = nullptr;
static void hh_wrap_M24_FUN_801c0190(uint8_t* rdram, recomp_context* ctx) {
    hh_real_M24_FUN_801c0190(rdram, ctx);
    if (getenv("HH_LSTTRACE") != nullptr) hh_chain_log(3, "CHK_0190", (uint32_t)ctx->r2, rdram);
}
static recomp_func_t* hh_real_M24_FUN_801bf610 = nullptr;
static void hh_wrap_M24_FUN_801bf610(uint8_t* rdram, recomp_context* ctx) {
    if (getenv("HH_LSTTRACE") != nullptr) fprintf(stderr, "[TL] t=%d vi=%llu SET_F28 a0=%08X\n",
        hh_ms_now(), (unsigned long long)hh_get_vi_count(), (unsigned)ctx->r4);
    hh_real_M24_FUN_801bf610(rdram, ctx);
}
static recomp_func_t* hh_real_M24_FUN_801bf61c = nullptr;
static void hh_wrap_M24_FUN_801bf61c(uint8_t* rdram, recomp_context* ctx) {
    if (getenv("HH_LSTTRACE") != nullptr) fprintf(stderr, "[TL] t=%d vi=%llu SET_TL a0=%08X\n",
        hh_ms_now(), (unsigned long long)hh_get_vi_count(), (unsigned)ctx->r4);
    hh_real_M24_FUN_801bf61c(rdram, ctx);
}
static recomp_func_t* hh_real_FUN_80031190 = nullptr; // osGetTime (reimpl)
extern "C" uint8_t* hh_get_rdram_base(void);
static void hh_wrap_FUN_80031190(uint8_t* rdram, recomp_context* ctx) {
    hh_real_FUN_80031190(rdram, ctx);
    // HH: diagnostico del target del frame limiter (M7_FUN_80133AA0 lee [0x8017AA90]). Una vez, ya
    // arrancado el juego. Gated HH_CLOCKDIAG.
    if (getenv("HH_CLOCKDIAG") != nullptr) {
        static bool done = false;
        uint64_t vi = hh_get_vi_count();
        if (!done && vi > 400 && vi < 1000) {
            done = true;
            uint8_t* rb = hh_get_rdram_base();
            uint32_t target = rb ? *(uint32_t*)(rb + (0x8017AA90u - 0x80000000u)) : 0;
            uint64_t t = ((uint64_t)(uint32_t)ctx->r2 << 32) | (uint32_t)ctx->r3;
            fprintf(stderr, "[CLOCK] target[0x8017AA90]=%u (0x%X) VI=%llu osGetTime=%llu (%llu ms)\n",
                    target, target, (unsigned long long)vi,
                    (unsigned long long)t, (unsigned long long)(t / 46875));
        }
    }
    if (getenv("HH_LSTTRACE") != nullptr) {
        static unsigned long long n = 0;
        uint64_t vi = hh_get_vi_count();
        if (vi < 1000 && (n % 500000) == 0) {
            uint64_t t = ((uint64_t)(uint32_t)ctx->r2 << 32) | (uint32_t)ctx->r3;
            fprintf(stderr, "[TL] t=%d vi=%llu OSGETTIME n=%llu v=%016llX (%llu ms)\n",
                hh_ms_now(), (unsigned long long)vi, n,
                (unsigned long long)t, (unsigned long long)(t / 46875));
        }
        n++;
    }
}
// HH: camino de cambio de escena (modulo 99) invocado por M24_FUN_801bf398 cuando el predicado
// FUN_801BFA58() != 0. En el emulador la carga de modulos ocurre en VI~3660; en el port en VI~840.
static void hh_scenefn(const char* name, uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    if (getenv("HH_LSTTRACE") != nullptr) {
        fprintf(stderr, "[TL] t=%d vi=%llu SCENEFN %s a0=%08X a1=%08X a2=%08X a3=%08X\n",
            hh_ms_now(), (unsigned long long)hh_get_vi_count(), name,
            (unsigned)ctx->r4, (unsigned)ctx->r5, (unsigned)ctx->r6, (unsigned)ctx->r7);
    }
}
static recomp_func_t* hh_real_FUN_8038BCE0 = nullptr;
static void hh_wrap_FUN_8038BCE0(uint8_t* rdram, recomp_context* ctx) {
    auto r32 = [&](uint32_t a){ return *(uint32_t*)&rdram[a & 0x7FFFFF]; };
    hh_scenefn("8038BCE0", rdram, ctx);
    static uint32_t last478 = 0xEEEEEEEEu;
    uint32_t a = r32(0x80089478);
    if (getenv("HH_LSTTRACE") != nullptr && a != last478) {
        fprintf(stderr, "[TL] t=%d vi=%llu P89478 %08X -> %08X (lhu=%04X b1000=%u cnt30=%u tl=%08X d00=%08X)\n",
            hh_ms_now(), (unsigned long long)hh_get_vi_count(), last478, a,
            (a >> 16) & 0xFFFFu, ((a >> 16) & 0x1000u) ? 1u : 0u, r32(0x801D8DA8),
            r32(0x801D8CE4), r32(0x801D8D00));
        last478 = a;
    }
    hh_real_FUN_8038BCE0(rdram, ctx);
}
static recomp_func_t* hh_real_FUN_8038C914 = nullptr;
static void hh_wrap_FUN_8038C914(uint8_t* rdram, recomp_context* ctx) { hh_scenefn("8038C914", rdram, ctx); hh_real_FUN_8038C914(rdram, ctx); }
static recomp_func_t* hh_real_FUN_8038CA8C = nullptr;
static void hh_wrap_FUN_8038CA8C(uint8_t* rdram, recomp_context* ctx) { hh_scenefn("8038CA8C", rdram, ctx); hh_real_FUN_8038CA8C(rdram, ctx); }
static recomp_func_t* hh_real_FUN_8038D224 = nullptr;
static void hh_wrap_FUN_8038D224(uint8_t* rdram, recomp_context* ctx) { hh_scenefn("8038D224", rdram, ctx); hh_real_FUN_8038D224(rdram, ctx); }
static recomp_func_t* hh_real_M24_FUN_801bfa58 = nullptr;
static void hh_wrap_M24_FUN_801bfa58(uint8_t* rdram, recomp_context* ctx) {
    hh_real_M24_FUN_801bfa58(rdram, ctx);
    if (getenv("HH_LSTTRACE") != nullptr) {
        static uint32_t last = 0xEEEEEEEEu;
        uint32_t v = (uint32_t)ctx->r2;
        if (v != last) {
            auto r32 = [&](uint32_t a){ return *(uint32_t*)&rdram[a & 0x7FFFFF]; };
            fprintf(stderr, "[TL] t=%d vi=%llu BFA58 -> %08X (f28=%08X cnt30=%u tl=%08X g2=%u ctr=%u flag=%u)\n",
                hh_ms_now(), (unsigned long long)hh_get_vi_count(), v, r32(0x801DE828),
                r32(0x801D8DA8), r32(0x801D8CE4), r32(0x801D8CE8), r32(0x801D8CF0), r32(0x801D8CFC) & 0xFF);
            last = v;
        }
    }
}
// HH: dispatcher de comandos de guion (M24_FUN_801bf850): a0=obj, a1=indice, a2=cmd.
static recomp_func_t* hh_real_M24_FUN_801bf850 = nullptr;
static void hh_wrap_M24_FUN_801bf850(uint8_t* rdram, recomp_context* ctx) {
    uint32_t a0 = (uint32_t)ctx->r4, a1 = (uint32_t)ctx->r5, a2 = (uint32_t)ctx->r6;
    hh_real_M24_FUN_801bf850(rdram, ctx);
    if (getenv("HH_LSTTRACE") != nullptr) {
        auto r32 = [&](uint32_t a){ return *(uint32_t*)&rdram[a & 0x7FFFFF]; };
        uint32_t cmd = r32(a2), word1 = r32(a2 + 4);
        fprintf(stderr, "[TL] t=%d vi=%llu CMD a0=%08X idx=%u cmd=%08X w1=%08X -> %08X\n",
            hh_ms_now(), (unsigned long long)hh_get_vi_count(), a0, a1, cmd, word1, (unsigned)ctx->r2);
    }
}
static recomp_func_t* hh_real_M24_FUN_801c0464 = nullptr;
static void hh_wrap_M24_FUN_801c0464(uint8_t* rdram, recomp_context* ctx) {
    if (getenv("HH_LSTTRACE") != nullptr) fprintf(stderr, "[LST] t=%d vi=%llu SCRIPT a0=%08X a1=%08X\n",
        hh_ms_now(), (unsigned long long)hh_get_vi_count(), (unsigned)ctx->r4, (unsigned)ctx->r5);
    hh_real_M24_FUN_801c0464(rdram, ctx);
}
static recomp_func_t* hh_real_M25_FUN_801e389c = nullptr;
static void hh_wrap_M25_FUN_801e389c(uint8_t* rdram, recomp_context* ctx) {
    if (getenv("HH_LSTTRACE") != nullptr) fprintf(stderr, "[LST] t=%d vi=%llu M25C@389c a0=%08X a1=%08X\n",
        hh_ms_now(), (unsigned long long)hh_get_vi_count(), (unsigned)ctx->r4, (unsigned)ctx->r5);
    hh_real_M25_FUN_801e389c(rdram, ctx);
}
static recomp_func_t* hh_real_M25_FUN_801e39f8 = nullptr;
static void hh_wrap_M25_FUN_801e39f8(uint8_t* rdram, recomp_context* ctx) {
    if (getenv("HH_LSTTRACE") != nullptr) fprintf(stderr, "[LST] t=%d vi=%llu M25C@39f8 a0=%08X a1=%08X\n",
        hh_ms_now(), (unsigned long long)hh_get_vi_count(), (unsigned)ctx->r4, (unsigned)ctx->r5);
    hh_real_M25_FUN_801e39f8(rdram, ctx);
}
static recomp_func_t* hh_real_FUN_8000433C = nullptr;
static void hh_wrap_FUN_8000433C(uint8_t* rdram, recomp_context* ctx) {
    if (getenv("HH_LSTTRACE") != nullptr) {
        auto r32 = [&](uint32_t a){ return *(uint32_t*)&rdram[a & 0x7FFFFF]; };
        uint32_t s[8];
        bool any = false;
        for (int i = 0; i < 8; ++i) { s[i] = r32(0x800894F4 + i * 4); if (s[i] != 0) any = true; }
        if (any) {
            fprintf(stderr, "[LST] t=%d PROC slots=", hh_ms_now());
            for (int i = 0; i < 8; ++i) fprintf(stderr, "%08X%s", s[i], i == 7 ? "\n" : ",");
        }
    }
    hh_real_FUN_8000433C(rdram, ctx);
}
static void hh_log_chain(const char* name, uint8_t* rdram, recomp_context* ctx) {
    fprintf(stderr, "[CHN] t=%d %s a0=%08X a1=%08X bd6d=%02X d550=%02X\n", hh_ms_now(), name,
        (unsigned)ctx->r4, (unsigned)ctx->r5,
        (unsigned)rdram[(0x801BBD6D ^ 3) & 0x7FFFFF], (unsigned)rdram[(0x8008D550 ^ 3) & 0x7FFFFF]);
}
static recomp_func_t* hh_real_FUN_80125968 = nullptr;
static void hh_wrap_FUN_80125968(uint8_t* rdram, recomp_context* ctx) { hh_log_chain("80125968", rdram, ctx); hh_real_FUN_80125968(rdram, ctx); }
static recomp_func_t* hh_real_FUN_801257DC = nullptr;
static void hh_wrap_FUN_801257DC(uint8_t* rdram, recomp_context* ctx) { hh_log_chain("801257DC", rdram, ctx); hh_real_FUN_801257DC(rdram, ctx); }
static recomp_func_t* hh_real_FUN_80126A18 = nullptr;
static void hh_wrap_FUN_80126A18(uint8_t* rdram, recomp_context* ctx) { hh_log_chain("80126A18", rdram, ctx); hh_real_FUN_80126A18(rdram, ctx); }
static recomp_func_t* hh_real_FUN_801267C0 = nullptr;
static void hh_wrap_FUN_801267C0(uint8_t* rdram, recomp_context* ctx) { hh_log_chain("801267C0", rdram, ctx); hh_real_FUN_801267C0(rdram, ctx); }
static recomp_func_t* hh_real_FUN_80022044 = nullptr; // progreso de modulo: contador de carga
static void hh_wrap_FUN_80022044(uint8_t* rdram, recomp_context* ctx) {
    auto r8 = [&](uint32_t a){ return (uint32_t)rdram[(a ^ 3) & 0x7FFFFF]; };
    auto r16 = [&](uint32_t a){ return (uint32_t)*(uint16_t*)&rdram[(a ^ 2) & 0x7FFFFF]; };
    uint32_t cnt = r8(0x800CBB4E);
    uint32_t v1 = r16(0x800CBB4C);
    uint32_t flag = r8(0x8008DC18);
    uint32_t idx = (v1 - 0x10) & 0xFFFF;
    uint32_t tbl = r8(0x80153072 + idx * 4);
    static int n = 0;
    if (n < 3000) { fprintf(stderr, "[MDL] n=%d cnt=%u v1=%04X flag=%u idx=%X tbl=%u\n", n, cnt, v1, flag, idx, tbl); n++; }
    hh_real_FUN_80022044(rdram, ctx);
}
static recomp_func_t* hh_real_FUN_8001fd14 = nullptr; // driver de audio (mixer + submit SP)
static void hh_wrap_FUN_8001fd14(uint8_t* rdram, recomp_context* ctx) {
    static uint64_t n = 0;
    uint32_t a1 = (uint32_t)ctx->r5;
    bool bad = (a1 != 0) && (a1 < 0x80000000u || a1 >= 0x80800000u);
    if (n < 10 || (n % 200) == 0 || bad) {
        fprintf(stderr, "[TKA] n=%llu a0=%08X a1=%08X%s\n", (unsigned long long)n, (unsigned)ctx->r4, a1, bad ? " BAD" : "");
    }
    n++;
    hh_real_FUN_8001fd14(rdram, ctx);
}
static recomp_func_t* hh_real_FUN_8001fba8 = nullptr; // thread del driver de audio
static void hh_wrap_FUN_8001fba8(uint8_t* rdram, recomp_context* ctx) {
    static uint64_t n = 0;
    fprintf(stderr, "[TKB] entry n=%llu\n", (unsigned long long)n);
    n++;
    hh_real_FUN_8001fba8(rdram, ctx);
}
static recomp_func_t* hh_real_FUN_80021eb8 = nullptr; // peticion de modulo (escribe 0x800CBB4C)
static void hh_wrap_FUN_80021eb8(uint8_t* rdram, recomp_context* ctx) {
    auto r16 = [&](uint32_t a){ return (uint32_t)*(uint16_t*)&rdram[(a ^ 2) & 0x7FFFFF]; };
    auto r32 = [&](uint32_t a){ return (uint32_t)*(uint32_t*)&rdram[a & 0x7FFFFF]; };
    static int n = 0;
    if (n < 40) {
        fprintf(stderr, "[MDLE] n=%d a0=%08X a1=%08X a2=%08X a3=%08X cb48=%08X cbb4c=%04X\n",
            n, (unsigned)ctx->r4, (unsigned)ctx->r5, (unsigned)ctx->r6, (unsigned)ctx->r7,
            r32(0x800CBB48), r16(0x800CBB4C));
    }
    n++;
    hh_real_FUN_80021eb8(rdram, ctx);
    if (n <= 40) fprintf(stderr, "[MDLE] -> cbb4c=%04X\n", r16(0x800CBB4C));
}
static recomp_func_t* hh_real_FUN_8002c4d0 = nullptr; // iterador del scheduler de procesos
static void hh_wrap_FUN_8002c4d0(uint8_t* rdram, recomp_context* ctx) {
    static uint64_t n = 0;
    if (n < 20 || (n % 500) == 0) {
        fprintf(stderr, "[SCD] n=%llu a0=%08X a1=%08X a2=%08X a3=%08X\n",
                (unsigned long long)n, (unsigned)ctx->r4, (unsigned)ctx->r5, (unsigned)ctx->r6, (unsigned)ctx->r7);
    }
    n++;
    hh_real_FUN_8002c4d0(rdram, ctx);
}
static recomp_func_t* hh_real_FUN_8002c7f4 = nullptr; // selector de voz del mixer
static void hh_wrap_FUN_8002c7f4(uint8_t* rdram, recomp_context* ctx) {
    auto r32 = [&](uint32_t a){ return (uint32_t)*(uint32_t*)&rdram[a & 0x7FFFFF]; };
    uint32_t outp = (uint32_t)ctx->r5;
    hh_real_FUN_8002c7f4(rdram, ctx);
    static int n = 0;
    if (n < 24) {
        uint32_t voice = outp ? r32(outp) : 0;
        uint32_t cb = voice ? r32(voice + 8) : 0;
        fprintf(stderr, "[VOICE] n=%d ret=%08X voice=%08X cb=%08X\n", n, (unsigned)ctx->r2, voice, cb);
    }
    n++;
}
static recomp_func_t* hh_real_FUN_80020460 = nullptr; // procesa peticion de modulo (llama a 80022044)
static void hh_wrap_FUN_80020460(uint8_t* rdram, recomp_context* ctx) {
    auto r8 = [&](uint32_t a){ return (uint32_t)rdram[(a ^ 3) & 0x7FFFFF]; };
    auto r16 = [&](uint32_t a){ return (uint32_t)*(uint16_t*)&rdram[(a ^ 2) & 0x7FFFFF]; };
    static int n = 0;
    uint32_t ab0 = r8(0x800CBAB0);
    uint32_t req = r16(0x800CBB4C);
    uint32_t st = (uint32_t)rdram[(0x800CBAF0 ^ 3) & 0x7FFFFF];
    if (n < 20 || (n % 100) == 0 || req != 0) {
        fprintf(stderr, "[MDLB] n=%d ab0=%u req=%04X st=%02X\n", n, ab0, req, st);
    }
    n++;
    hh_real_FUN_80020460(rdram, ctx);
}
static recomp_func_t* hh_real_FUN_800207d0 = nullptr; // productor de eventos de modulo (cola 0x800CBB28)
static void hh_wrap_FUN_800207d0(uint8_t* rdram, recomp_context* ctx) {
    auto r8 = [&](uint32_t a){ return (uint32_t)rdram[(a ^ 3) & 0x7FFFFF]; };
    static int n = 0;
    if (n < 200) {
        fprintf(stderr, "[EVQ] n=%d id=%04X ra=%08X a1=%08X a3=%08X cnt=%u\n",
            n, (unsigned)ctx->r4 & 0xFFFF, (unsigned)ctx->r31, (unsigned)ctx->r5,
            (unsigned)ctx->r7, r8(0x800CBB22));
    }
    n++;
    hh_real_FUN_800207d0(rdram, ctx);
}
static recomp_func_t* hh_real_FUN_80020f60 = nullptr; // consumidor de la cola de eventos de modulo
static void hh_wrap_FUN_80020f60(uint8_t* rdram, recomp_context* ctx) {
    auto r8 = [&](uint32_t a){ return (uint32_t)rdram[(a ^ 3) & 0x7FFFFF]; };
    auto r32 = [&](uint32_t a){ return (uint32_t)*(uint32_t*)&rdram[a & 0x7FFFFF]; };
    uint32_t cnt0 = r8(0x800CBB22);
    uint32_t pend = cnt0 ? r32(0x800CBB28) : 0xFFFFFFFFu;
    hh_real_FUN_80020f60(rdram, ctx);
    uint32_t cnt1 = r8(0x800CBB22);
    static int n = 0;
    if (n < 100 || cnt0 != 0 || cnt0 != cnt1) {
        fprintf(stderr, "[EVQC] n=%d cnt0=%u cnt1=%u id=%04X\n", n, cnt0, cnt1, (unsigned)pend & 0xFFFF);
    }
    n++;
}
static recomp_func_t* hh_real_FUN_8002059c = nullptr; // rama estado==8 de FUN_80020460
static void hh_wrap_FUN_8002059c(uint8_t* rdram, recomp_context* ctx) {
    static int n = 0;
    if (n < 200) fprintf(stderr, "[EVQS] n=%d (estado==8)\n", n);
    n++;
    hh_real_FUN_8002059c(rdram, ctx);
}
static recomp_func_t* hh_real_FUN_80022b6c = nullptr; // handler del evento id 0x08 (tabla 0x80047E0C)
static void hh_wrap_FUN_80022b6c(uint8_t* rdram, recomp_context* ctx) {
    auto r16 = [&](uint32_t a){ return (uint32_t)*(uint16_t*)&rdram[(a ^ 2) & 0x7FFFFF]; };
    auto r32 = [&](uint32_t a){ return (uint32_t)*(uint32_t*)&rdram[a & 0x7FFFFF]; };
    static int n = 0;
    if (n < 200) {
        fprintf(stderr, "[EVQH] n=%d a0=%08X a1=%08X ab4=%04X ab8=%04X ab0=%08X cbb48=%08X\n", n,
            (unsigned)ctx->r4, (unsigned)ctx->r5, r16(0x800CBAAC), r16(0x800CBAAE),
            r32(0x800CBAB0), r32(0x800CBB48));
    }
    n++;
    hh_real_FUN_80022b6c(rdram, ctx);
}
static recomp_func_t* hh_real_FUN_80004560 = nullptr; // registra recurso: FUN_8001752C(id, ptr)
static void hh_wrap_FUN_80004560(uint8_t* rdram, recomp_context* ctx) {
    auto rb = [&](uint32_t a){ return (uint32_t)rdram[(a ^ 3) & 0x7FFFFF]; };
    uint32_t a0 = (uint32_t)ctx->r4 & 0xFFFF;
    uint32_t d558 = (uint32_t)rdram[(0x8008D558 ^ 3) & 0x7FFFFF];
    hh_real_FUN_80004560(rdram, ctx);
    static int n = 0;
    if (n < 60 || a0 == 0x74) {
        fprintf(stderr, "[REG] n=%d FUN_80004560 id=%04X d558=%02X -> %08X c0=%02X ff=%02X\n", n, a0, d558,
            (unsigned)ctx->r2, rb(0x8008D570), rb(0x8008D5AF));
    }
    n++;
}
extern "C" double hh_time_now(void);
static recomp_func_t* hh_real_FUN_80000ed0 = nullptr; // submit de task con contador +0x89C
static void hh_wrap_FUN_80000ed0(uint8_t* rdram, recomp_context* ctx) {
    auto r32 = [&](uint32_t a){ return *(uint32_t*)&rdram[a & 0x7FFFFF]; };
    uint32_t mq = (uint32_t)ctx->r5, msg = (uint32_t)ctx->r6;
    uint32_t before = r32(0x8005CD4C);
    fprintf(stderr, "[SUBM] t=%.3f tid=%d msg=%08X mq=%08X fl8=%08X cnt=%u\n", hh_time_now(), hh_tid(rdram), msg, mq, r32((msg & 0x7FFFFF) + 8), before);
    hh_real_FUN_80000ed0(rdram, ctx);
    fprintf(stderr, "[SUBM] -> t=%.3f cnt=%u\n", hh_time_now(), r32(0x8005CD4C));
}

// HH: declaraciones para el registro de llamadas (diagnostico de pila).
extern "C" int hh_watch_active;
extern "C" recomp_context* hh_get_current_ctx(void);
extern "C" uint8_t* hh_get_rdram_base(void);
extern "C" void hh_ring_record(uint32_t target, uint32_t sp);
extern "C" void hh_ring2_record(uint32_t target, uint32_t sp);
extern "C" void hh_callring_record(uint32_t addr);
extern "C" void hh_trace_fn(uint32_t addr);
extern "C" recomp_context* hh_get_current_ctx(void);
extern "C" uint8_t* hh_get_rdram_base(void);

static thread_local uint32_t hh_last_r16 = 0xFFFFFFFFu;
static thread_local uint32_t hh_last_tgt = 0;
static thread_local bool hh_main_seen = false;

// HH: FIX del cuelgue por dano. El bucle principal (0x800011B0) guarda en s0 (r16) el puntero de
// estado 0x80037748 y lo usa tras sus llamadas (frame/no-op). Algun camino del frame (dispatcher
// FUN_80026fe8 y compania, simbolo sobredimensionado) deja s0 con un valor que ya no es un puntero
// valido de RDRAM -> el check lee memoria invalida -> el frame no se vuelve a llamar (sin input
// polls, imagen congelada; el resto del motor sigue). Reparamos SOLO el puntero invalido, en la
// entrada del osRecvMesg del bucle principal, restaurando el invariante s0=0x80037748. Un valor
// legitimo (0x800xxxxx) nunca se toca. Opt-in con HH_S0FIX=1.
static std::atomic<unsigned long long> hh_s0fix_counter{0};
extern "C" unsigned long long hh_s0fix_hits(void) {
    return hh_s0fix_counter.load();
}
static thread_local uint32_t hh_last_valid_r16 = 0;
// HH: FIX del cuelgue por dano. El bucle principal (FUN_800011B0) guarda en s0 (r16) el puntero
// de estado 0x80037748 y, tras llamar al "work" (0x8000290C), decide con el si ejecuta el frame
// (FUN_80001454, poll de input) o un no-op. La cadena del frame (dispatcher FUN_80026fe8 y
// compania) puede dejar s0 con otro valor (incluso un puntero "valido" a un modulo), y entonces el
// frame no se vuelve a llamar (imagen congelada, motor vivo). 0x8000290C tiene UN SOLO call-site
// (el bucle principal), asi que en su entrada forzamos el invariante s0=0x80037748 si no lo es.
static void hh_s0fix_check(uint32_t tgt, recomp_context* ctx) {
    if (ctx == nullptr) return;
    // Auto-test: HH_TEST_S0BUG=1 corrompe s0 una vez (a un puntero 'valido') para validar el fix.
    static const bool s0bug = getenv("HH_TEST_S0BUG") != nullptr;
    static thread_local int hh_s0bug_n = 0;
    if (s0bug && tgt == 0x8000290Cu && (uint32_t)ctx->r16 == 0x80037748u) {
        if (++hh_s0bug_n == 60) { ctx->r16 = 0x80249B80u; fprintf(stderr, "[S0TEST] corrompiendo r16 a 80249B80 (puntero 'valido')\n"); }
    }
    if (tgt != 0x8000290Cu) return;
    uint32_t r16 = (uint32_t)ctx->r16;
    if (r16 == 0x80037748u) return;
    ctx->r16 = 0x80037748u;
    hh_s0fix_counter++;
    static FILE* ff = nullptr;
    if (ff == nullptr) ff = fopen("hh_s0fix.log", "w");
    if (ff != nullptr) { fprintf(ff, "[S0FIX] r16 %08X -> 80037748 (work del bucle principal)\n", r16); fflush(ff); }
    static int hh_s0fix_console = 0;
    if (hh_s0fix_console < 20) { hh_s0fix_console++; fprintf(stderr, "[S0FIX] r16 %08X -> 80037748 (work del bucle principal)\n", r16); }
}

extern "C" recomp_func_t * get_function(int32_t addr) {
    // HH: flags de instrumentacion CACHEADOS. getenv() es caro y get_function se ejecuta en CADA
    // llamada recompilada (millones por tick en bucles de decodificacion como 0x80015A64/0x80016xxx)
    // -> cachearlos evita segundos de overhead. Se evaluan una sola vez.
    static const bool hh_mqlog_on = getenv("HH_MQLOG_ALL") != nullptr;
    static const bool hh_tbltrace_on = getenv("HH_TBLTRACE") != nullptr || getenv("HH_LSTTRACE") != nullptr;
    static const bool hh_modtrace_on = getenv("HH_MODTRACE") != nullptr;
    hh_calltrace((uint32_t)addr);
    hh_callring_record((uint32_t)addr);
    hh_trace_fn((uint32_t)addr);
    // HH: vigila cambios de r16 (s0) entre llamadas guest. El cuelgue por dano: s0 se machaca a
    // 0x1E82 y el dispatch frame/no-op del bucle principal se rompe. Loguea SOLO los cambios con
    // la funcion anterior (la culpable) y la actual. Gated por HH_MQLOG_ALL -> hh_s0.log.
    if (hh_mqlog_on) {
        recomp_context* hc0 = hh_get_current_ctx();
        if (hc0 != nullptr) {
            uint32_t r16 = (uint32_t)hc0->r16;
            uint32_t tgt = (uint32_t)addr;
            if (r16 == 0x80037748u || hh_last_r16 == 0x80037748u) hh_main_seen = true;
            bool interes = hh_main_seen;
            if (interes && r16 != hh_last_r16) {
                static FILE* sf = nullptr;
                static long total = 0;
                if (sf == nullptr) sf = fopen("hh_s0.log", "w");
                if (sf != nullptr && total < (32L * 1024 * 1024)) {
                    total += fprintf(sf, "[S0] prev=%08X new=%08X r16: %08X -> %08X\n",
                                     hh_last_tgt, tgt, hh_last_r16, r16);
                    fflush(sf);
                }
                hh_last_r16 = r16;
            }
            hh_last_tgt = tgt;
        }
    }
    // HH: vigilancia del dispatch del bucle principal (livelock del dano): registra, en cada
    // llamada al frame (0x80001454) o al no-op (0x80001BB0), el registro s0 (r16) y el valor de
    // memoria que consulta el branch (lhu 0x0(s0)). Gated por HH_MQLOG_ALL.
    {
        recomp_context* hc_fix = hh_get_current_ctx();
        hh_s0fix_check((uint32_t)addr, hc_fix);
    }
    if (((uint32_t)addr == 0x80001454u || (uint32_t)addr == 0x80001BB0u ||
         (uint32_t)addr == 0x8000290Cu || (uint32_t)addr == 0x800266B0u) && hh_mqlog_on) {
        recomp_context* hc = hh_get_current_ctx();
        uint32_t s0 = hc != nullptr ? (uint32_t)hc->r16 : 0;
        uint32_t flag = 0;
        uint8_t* rdram_base = hh_get_rdram_base();
        if (rdram_base != nullptr && s0 >= 0x80000000u && s0 + 2 <= 0x80800000u) {
            flag = *(uint16_t*)(rdram_base + (s0 - 0x80000000u));
        }
        static FILE* df = nullptr;
        if (df == nullptr) df = fopen("hh_disp.log", "w");
        if (df != nullptr) {
            fprintf(df, "[DISP] tgt=%08X s0=%08X flag=%04X\n", (unsigned)addr, s0, flag);
            fflush(df);
        }
    }
    // HH: registrar (target, sp) de cada llamada para diagnosticar hundimientos de pila. El anillo
    // grande (hh_ring2) se activa con HH_WATCH_ADDR o HH_CHAINTRACE (historial largo del veneno).
    static const bool hh_ring2_on = (getenv("HH_WATCH_ADDR") != nullptr) ||
                                    (getenv("HH_CHAINTRACE") != nullptr);
    if (hh_watch_active || hh_ring2_on) {
        recomp_context* hh_c = hh_get_current_ctx();
        uint32_t hh_sp = hh_c != nullptr ? (uint32_t)hh_c->r29 : 0;
        hh_ring_record((uint32_t)addr, hh_sp);
        hh_ring2_record((uint32_t)addr, hh_sp);
    }
    const char* hh_mod_label = nullptr;
    if (hh_modtrace_on && hh_modtrace_match((uint32_t)addr, &hh_mod_label)) {
        recomp_context* hc_m = hh_get_current_ctx();
        auto itm = func_map.find(addr);
        uint16_t hh_mode = 0;
        uint8_t* hh_rb = hh_get_rdram_base();
        if (hh_rb != nullptr) hh_mode = *(uint16_t*)&hh_rb[(0x801BBC1C ^ 2) & 0x7FFFFF];
        fprintf(stderr, "[MODT] %-10s vi=%llu mode=%04X addr=%08X ra=%08X -> %p\n", hh_mod_label,
                (unsigned long long)hh_get_vi_count(), (unsigned)hh_mode, (unsigned)addr,
                hc_m != nullptr ? (unsigned)hc_m->r31 : 0,
                itm != func_map.end() ? (const void*)itm->second : nullptr);
    }
    auto func_find = func_map.find(addr);
    if (hh_owner_watched(addr)) {
        recomp_context* hc_o = hh_get_current_ctx();
        fprintf(stderr, "[OWNER] get addr=%08X ra=%08X ret=exe+0x%llX -> %p\n", addr,
                hc_o != nullptr ? (unsigned)hc_o->r31 : 0,
                (unsigned long long)HH_RETURN_RVA(),
                func_find != func_map.end() ? (const void*)func_find->second : nullptr);
    }
    // HH: diagnostico del disparador del disable (M10_FUN_8021b280). Al resolverlo, volcar el
    // contexto del llamante y localizar el/los slot(s) de RDRAM que guardan su puntero (para cazar
    // quien lo publica). Gated por HH_B280TRACE. Ver notes/2026-09-19-*.
    if ((uint32_t)addr == 0x8021B280u && getenv("HH_B280TRACE") != nullptr) {
        recomp_context* hc_b = hh_get_current_ctx();
        uint8_t* rb_b = hh_get_rdram_base();
        static FILE* bf = nullptr;
        static int bn = 0;
        if (bf == nullptr) bf = fopen("hh_b280.log", "w");
        if (bf != nullptr && bn < 32) {
            bn++;
            fprintf(bf, "=== B280 #%d vi=%llu sample=%llu a0=%08X a1=%08X a2=%08X a3=%08X sp=%08X ra=%08X ===\n",
                    bn, (unsigned long long)hh_get_vi_count(), (unsigned long long)hh_replay_get_sample(),
                    hc_b ? (unsigned)hc_b->r4 : 0, hc_b ? (unsigned)hc_b->r5 : 0,
                    hc_b ? (unsigned)hc_b->r6 : 0, hc_b ? (unsigned)hc_b->r7 : 0,
                    hc_b ? (unsigned)hc_b->r29 : 0, hc_b ? (unsigned)hc_b->r31 : 0);
            if (rb_b != nullptr) {
                int found = 0;
                for (uint32_t o = 0; o + 16u <= 0x400000u; o += 4) {
                    if (*(uint32_t*)(rb_b + o) == 0x8021B280u) {
                        uint32_t base = o & ~0xFu;
                        fprintf(bf, "  slot=%08X ctx:", o | 0x80000000u);
                        for (uint32_t k = 0; k < 4; k++) fprintf(bf, " %08X", *(uint32_t*)(rb_b + base + k * 4));
                        fprintf(bf, "\n");
                        if (++found >= 16) break;
                    }
                }
                fprintf(bf, "  slots=%d\n", found);
                // HH: variables de la PUERTA que decide instalar b280 (M7_FUN_80126A18):
                // 0x801BBD78 (0x188), 0x801BBD71 (0x181), timer 0x8008D580 (0x42D0), 0x8008D5AF (0x42FF).
                auto rw = [&](uint32_t a) { return *(uint32_t*)(rb_b + (a - 0x80000000u)); };
                auto rb8 = [&](uint32_t a) { return (unsigned)rb_b[a - 0x80000000u]; };
                fprintf(bf, "  gate: 188=%08X 181=%02X timer42D0=%08X 42FF=%02X mode7DD92=%02X\n",
                        rw(0x801BBD78u), rb8(0x801BBD71u), rw(0x8008D580u), rb8(0x8008D5AFu),
                        rb8(0x8017DD92u));
            }
            uint32_t ring[16];
            int rn = hh_get_callring(hc_b, ring, 16);
            if (rn > 0) {
                fprintf(bf, "  callring:");
                for (int k = 0; k < rn; k++) fprintf(bf, " %08X", ring[k]);
                fprintf(bf, "\n");
            }
            fflush(bf);
        }
    }
    if (func_find == func_map.end()) {
        // HH: diagnostico del LLAMANTE. Un target invalido (p. ej. 0xFF7F84CD) suele ser un puntero
        // de funcion corrupto, no un simbolo sin registrar. `ra` identifica al que intenta llamarlo.
        recomp_context* hc_miss = hh_get_current_ctx();
        char caller[320];
        if (hc_miss != nullptr) {
            // `host_ret` = direccion de retorno NATIVA dentro de la funcion recompilada llamante;
            // se simboliza con el .map (exe+0x...) y da la funcion guest exacta (p. ej. M12_FUN_...).
            snprintf(caller, sizeof(caller),
                     "  caller ra=%08X sp=%08X r4=%08X r5=%08X r6=%08X r7=%08X host_ret=exe+0x%llX",
                     (unsigned)hc_miss->r31, (unsigned)hc_miss->r29,
                     (unsigned)hc_miss->r4, (unsigned)hc_miss->r5,
                     (unsigned)hc_miss->r6, (unsigned)hc_miss->r7,
                     (unsigned long long)HH_RETURN_RVA());
        } else {
            snprintf(caller, sizeof(caller), "  caller (sin contexto) host_ret=exe+0x%llX",
                     (unsigned long long)HH_RETURN_RVA());
        }
        fprintf(stderr, "Failed to find function at 0x%08X\n%s\n", addr, caller);
        // HH: además de stderr, dejarlo en hh_missing.log (CWD) para builds de Windows sin consola.
        if (FILE* mf = fopen("hh_missing.log", "a")) {
            fprintf(mf, "Failed to find function at 0x%08X\n%s\n", addr, caller);
            fclose(mf);
        }
        // HH: volcado del objeto del llamante (s0/r16) y busqueda del valor corrupto, para localizar
        // de que estructura sale el puntero. Solo diagnostico (una vez, al fallar).
        if (FILE* df = fopen("hh_badlookup.log", "a")) {
            uint8_t* rb = hh_get_rdram_base();
            uint32_t s0 = hc_miss != nullptr ? (uint32_t)hc_miss->r16 : 0;
            fprintf(df, "=== bad lookup target=%08X caller ra=%08X sp=%08X s0/r16=%08X r4=%08X r5=%08X host_ret=exe+0x%llX ===\n",
                    (unsigned)addr, hc_miss ? (unsigned)hc_miss->r31 : 0, hc_miss ? (unsigned)hc_miss->r29 : 0,
                    (unsigned)s0, hc_miss ? (unsigned)hc_miss->r4 : 0, hc_miss ? (unsigned)hc_miss->r5 : 0,
                    (unsigned long long)HH_RETURN_RVA());
            if (rb != nullptr && s0 >= 0x80000000u && s0 + 0x40 <= 0x80800000u) {
                for (int row = 0; row < 0x40; row += 4) {
                    uint32_t v = *(uint32_t*)&rb[(s0 + row) & 0x1FFFFFFFu];
                    fprintf(df, "  s0+%02X = %08X\n", row, (unsigned)v);
                }
            }
            if (rb != nullptr) {
                int n = 0;
                uint32_t bad = (uint32_t)addr;
                for (uint32_t off = 0; off + 4 <= 0x800000u && n < 32; off += 4) {
                    if (*(uint32_t*)&rb[off] == bad) {
                        fprintf(df, "  valor %08X encontrado en guest %08X\n", bad, (unsigned)(off | 0x80000000u));
                        ++n;
                    }
                }
            }
            fclose(df);
        }
        if (getenv("HH_SOFT_LOOKUP") != nullptr) {
            return soft_missing_func;
        }
        // HH: un target FUERA del rango de codigo (KSEG0 0x80000000..0x807FFFFF) no puede ser una
        // funcion recompilada: es un puntero corrupto/centinela (p. ej. 0xFF7F84CD = centinela del
        // juego al que se le perdio el bit 0x00800000). El juego lo usaba como "callback no
        // habilitado"; devolver no-op equivale a lo que pretendia y evita el crash. Un target dentro
        // del rango pero no registrado sigue abortando (para registrar mid-entries).
        uint32_t uaddr = (uint32_t)addr;
        if (uaddr < 0x80000000u || uaddr >= 0x80800000u) {
            static int hh_invalid_targets = 0;
            if (hh_invalid_targets < 20) {
                ++hh_invalid_targets;
                fprintf(stderr, "[LOOKUP] target fuera de rango %08X -> no-op (posible centinela)\n", uaddr);
            }
            return soft_missing_func;
        }
        assert(false);
        std::exit(EXIT_FAILURE);
    }
    // HH: el loader de módulos debe registrar la sección recompilada en su base real SIEMPRE
    // (no solo en modo traza): soporta la reutilización de bases entre módulos.
    if ((uint32_t)addr == 0x80001454) {
        if (hh_real_FUN_80001454 == nullptr) {
            hh_real_FUN_80001454 = func_find->second;
        }
        return hh_wrap_FUN_80001454;
    }
    if ((uint32_t)addr == 0x80001BB0) {
        if (hh_real_FUN_80001BB0 == nullptr) {
            hh_real_FUN_80001BB0 = func_find->second;
        }
        return hh_wrap_FUN_80001BB0;
    }
    if ((uint32_t)addr == 0x80003824) {
        if (hh_real_FUN_80003824 == nullptr) {
            hh_real_FUN_80003824 = func_find->second;
        }
        return hh_wrap_FUN_80003824;
    }
    // HH: loader streamed (fichero 57, combate). SIEMPRE: registra el modulo al completarse.
    if ((uint32_t)addr == 0x80004838) {
        if (hh_real_FUN_80004838 == nullptr) {
            hh_real_FUN_80004838 = func_find->second;
        }
        return hh_wrap_FUN_80004838;
    }
    // HH: el setter de callback FUN_800058dc se engancha SIEMPRE (barato: ~1400 llamadas). Si recibe
    // el veneno 0xFFFF84CD, vuelca la pila guest (quien dispara el disable) a hh_venom.log.
    if ((uint32_t)addr == 0x800058DC) {
        if (hh_real_FUN_800058dc == nullptr) {
            hh_real_FUN_800058dc = func_find->second;
        }
        return hh_wrap_FUN_800058dc;
    }
    if ((uint32_t)addr == 0x800058F4) {
        if (hh_real_FUN_800058f4 == nullptr) {
            hh_real_FUN_800058f4 = func_find->second;
        }
        return hh_wrap_FUN_800058f4;
    }
    // HH: puerta M7 que arma el contador del callback de la transicion (traza HH_M7GATE).
    if ((uint32_t)addr == 0x80126CC0) {
        if (hh_real_M7_FUN_80126cc0 == nullptr) {
            hh_real_M7_FUN_80126cc0 = func_find->second;
        }
        return hh_wrap_M7_FUN_80126cc0;
    }
    // HH: puerta del disable (traza HH_GATE_A).
    if ((uint32_t)addr == 0x80126A0C) {
        if (hh_real_M7_FUN_80126a0c == nullptr) {
            hh_real_M7_FUN_80126a0c = func_find->second;
        }
        return hh_wrap_M7_FUN_80126a0c;
    }
    // HH: fijacion de la epoch de la linea temporal M24 (traza HH_EPOCHTRACE).
    if ((uint32_t)addr == 0x801C0A30) {
        if (hh_real_M24_FUN_801c0a30 == nullptr) {
            hh_real_M24_FUN_801c0a30 = func_find->second;
        }
        return hh_wrap_M24_FUN_801c0a30;
    }
    // HH: traza de la cadena de callbacks del disable (gate HH_CHAINTRACE).
    if (getenv("HH_CHAINTRACE") != nullptr) {
        switch ((uint32_t)addr) {
            case 0x8021B1A8: if (hh_real_M10_FUN_8021b1A8 == nullptr) hh_real_M10_FUN_8021b1A8 = func_find->second; return hh_wrap_M10_FUN_8021b1A8;
            case 0x8021B200: if (hh_real_M10_FUN_8021b200 == nullptr) hh_real_M10_FUN_8021b200 = func_find->second; return hh_wrap_M10_FUN_8021b200;
            case 0x8021B240: if (hh_real_M10_FUN_8021b240 == nullptr) hh_real_M10_FUN_8021b240 = func_find->second; return hh_wrap_M10_FUN_8021b240;
            case 0x8021B280: if (hh_real_M10_FUN_8021b280 == nullptr) hh_real_M10_FUN_8021b280 = func_find->second; return hh_wrap_M10_FUN_8021b280;
            case 0x80006214: if (hh_real_FUN_80006214 == nullptr) hh_real_FUN_80006214 = func_find->second; return hh_wrap_FUN_80006214;
            case 0x8001F718: if (hh_real_FUN_8001f718 == nullptr) hh_real_FUN_8001f718 = func_find->second; return hh_wrap_FUN_8001f718;
            case 0x8001F76C: if (hh_real_FUN_8001f76c == nullptr) hh_real_FUN_8001f76c = func_find->second; return hh_wrap_FUN_8001f76c;
            case 0x801277B0: if (hh_real_M7_FUN_801277b0 == nullptr) hh_real_M7_FUN_801277b0 = func_find->second; return hh_wrap_M7_FUN_801277b0;
            case 0x8012C9C0: if (hh_real_M7_FUN_8012c9c0 == nullptr) hh_real_M7_FUN_8012c9c0 = func_find->second; return hh_wrap_M7_FUN_8012c9c0;
            case 0x8012CE10: if (hh_real_M7_FUN_8012ce10 == nullptr) hh_real_M7_FUN_8012ce10 = func_find->second; return hh_wrap_M7_FUN_8012ce10;
            case 0x8012B1E4: if (hh_real_M7_FUN_8012b1e4 == nullptr) hh_real_M7_FUN_8012b1e4 = func_find->second; return hh_wrap_M7_FUN_8012b1e4;
            case 0x8012BFA0: if (hh_real_M7_FUN_8012bfa0 == nullptr) hh_real_M7_FUN_8012bfa0 = func_find->second; return hh_wrap_M7_FUN_8012bfa0;
            case 0x80379410: if (hh_real_M55_FUN_80379410 == nullptr) hh_real_M55_FUN_80379410 = func_find->second; return hh_wrap_M55_FUN_80379410;
            case 0x80379424: if (hh_real_M55_FUN_80379424 == nullptr) hh_real_M55_FUN_80379424 = func_find->second; return hh_wrap_M55_FUN_80379424;
            case 0x80379444: if (hh_real_M55_FUN_80379444 == nullptr) hh_real_M55_FUN_80379444 = func_find->second; return hh_wrap_M55_FUN_80379444;
            case 0x80379464: if (hh_real_M55_FUN_80379464 == nullptr) hh_real_M55_FUN_80379464 = func_find->second; return hh_wrap_M55_FUN_80379464;
            case 0x8022C7A4: if (hh_real_M10_FUN_8022c7a4 == nullptr) hh_real_M10_FUN_8022c7a4 = func_find->second; return hh_wrap_M10_FUN_8022c7a4;
            case 0x8022C5AC: if (hh_real_M10_FUN_8022c5ac == nullptr) hh_real_M10_FUN_8022c5ac = func_find->second; return hh_wrap_M10_FUN_8022c5ac;
            case 0x8022C314: if (hh_real_M10_FUN_8022c314 == nullptr) hh_real_M10_FUN_8022c314 = func_find->second; return hh_wrap_M10_FUN_8022c314;
            case 0x8022C478: if (hh_real_M10_FUN_8022c478 == nullptr) hh_real_M10_FUN_8022c478 = func_find->second; return hh_wrap_M10_FUN_8022c478;
            case 0x80001454: if (hh_real_FUN_80001454 == nullptr) hh_real_FUN_80001454 = func_find->second; return hh_wrap_FUN_80001454;
            case 0x8000290C: if (hh_real_FUN_8000290C == nullptr) hh_real_FUN_8000290C = func_find->second; return hh_wrap_FUN_8000290C;
            case 0x80001BB0: if (hh_real_FUN_80001BB0 == nullptr) hh_real_FUN_80001BB0 = func_find->second; return hh_wrap_FUN_80001BB0;
            case 0x80000EC8: if (hh_real_FUN_80000ec8 == nullptr) hh_real_FUN_80000ec8 = func_find->second; return hh_wrap_FUN_80000ec8;
            case 0x80005270: if (hh_real_FUN_80005270 == nullptr) hh_real_FUN_80005270 = func_find->second; return hh_wrap_FUN_80005270_spchk;
            case 0x80001060: if (hh_real_FUN_80001060 == nullptr) hh_real_FUN_80001060 = func_find->second; return hh_wrap_FUN_80001060;
            case 0x80001B30: if (hh_real_FUN_80001b30 == nullptr) hh_real_FUN_80001b30 = func_find->second; return hh_wrap_FUN_80001b30;
            case 0x80001BC0: if (hh_real_FUN_80001bc0 == nullptr) hh_real_FUN_80001bc0 = func_find->second; return hh_wrap_FUN_80001bc0;
            case 0x80001D5C: if (hh_real_FUN_80001d5c == nullptr) hh_real_FUN_80001d5c = func_find->second; return hh_wrap_FUN_80001d5c;
            case 0x800021B4: if (hh_real_FUN_800021b4 == nullptr) hh_real_FUN_800021b4 = func_find->second; return hh_wrap_FUN_800021b4;
            case 0x8000433C: if (hh_real_FUN_8000433c == nullptr) hh_real_FUN_8000433c = func_find->second; return hh_wrap_FUN_8000433c;
            case 0x80006790: if (hh_real_FUN_80006790 == nullptr) hh_real_FUN_80006790 = func_find->second; return hh_wrap_FUN_80006790;
            case 0x80006AF0: if (hh_real_FUN_80006af0 == nullptr) hh_real_FUN_80006af0 = func_find->second; return hh_wrap_FUN_80006af0;
            case 0x8001E978: if (hh_real_FUN_8001e978 == nullptr) hh_real_FUN_8001e978 = func_find->second; return hh_wrap_FUN_8001e978;
            case 0x8001EAA4: if (hh_real_FUN_8001eaa4 == nullptr) hh_real_FUN_8001eaa4 = func_find->second; return hh_wrap_FUN_8001eaa4;
            case 0x80026E58: if (hh_real_FUN_80026e58 == nullptr) hh_real_FUN_80026e58 = func_find->second; return hh_wrap_FUN_80026e58;
            case 0x80026F58: if (hh_real_FUN_80026f58 == nullptr) hh_real_FUN_80026f58 = func_find->second; return hh_wrap_FUN_80026f58;
            case 0x80029FA0: if (hh_real_FUN_80029fa0 == nullptr) hh_real_FUN_80029fa0 = func_find->second; return hh_wrap_FUN_80029fa0;
            case 0x80032890: if (hh_real_FUN_80032890 == nullptr) hh_real_FUN_80032890 = func_find->second; return hh_wrap_FUN_80032890;
            case 0x80034AB8: if (hh_real_FUN_80034ab8 == nullptr) hh_real_FUN_80034ab8 = func_find->second; return hh_wrap_FUN_80034ab8;
            case 0x80034C24: if (hh_real_FUN_80034c24 == nullptr) hh_real_FUN_80034c24 = func_find->second; return hh_wrap_FUN_80034c24;
            case 0x80126744: if (hh_real_M7_FUN_80126744 == nullptr) hh_real_M7_FUN_80126744 = func_find->second; return hh_wrap_M7_FUN_80126744;
            case 0x80126944: if (hh_real_M7_FUN_80126944 == nullptr) hh_real_M7_FUN_80126944 = func_find->second; return hh_wrap_M7_FUN_80126944;
            case 0x8012FE50: if (hh_real_FUN_8012fe50 == nullptr) hh_real_FUN_8012fe50 = func_find->second; return hh_wrap_FUN_8012fe50;
            case 0x80133AA0: if (hh_real_FUN_80133aa0 == nullptr) hh_real_FUN_80133aa0 = func_find->second; return hh_wrap_FUN_80133aa0;
            case 0x80004BB0: if (hh_real_FUN_80004bb0 == nullptr) hh_real_FUN_80004bb0 = func_find->second; return hh_wrap_FUN_80004bb0;
            case 0x80004D20: if (hh_real_FUN_80004d20 == nullptr) hh_real_FUN_80004d20 = func_find->second; return hh_wrap_FUN_80004d20;
            case 0x80004ADC: if (hh_real_FUN_80004adc == nullptr) hh_real_FUN_80004adc = func_find->second; return hh_wrap_FUN_80004adc;
            case 0x80000A0C: if (hh_real_FUN_80000a0c == nullptr) hh_real_FUN_80000a0c = func_find->second; return hh_wrap_FUN_80000a0c;
            case 0x80000934: if (hh_real_FUN_80000934 == nullptr) hh_real_FUN_80000934 = func_find->second; return hh_wrap_FUN_80000934;
            case 0x80000984: if (hh_real_FUN_80000984 == nullptr) hh_real_FUN_80000984 = func_find->second; return hh_wrap_FUN_80000984;
            default: break;
        }
    }
    if (hh_tbltrace_on) {
        switch ((uint32_t)addr) {
            case 0x80017064: if (hh_real_FUN_80017064 == nullptr) hh_real_FUN_80017064 = func_find->second; return hh_wrap_FUN_80017064;
            case 0x80017014: if (hh_real_FUN_80017014 == nullptr) hh_real_FUN_80017014 = func_find->second; return hh_wrap_FUN_80017014;
            case 0x80004560: if (hh_real_FUN_80004560 == nullptr) hh_real_FUN_80004560 = func_find->second; return hh_wrap_FUN_80004560;
            case 0x800045E8: if (hh_real_FUN_800045e8 == nullptr) hh_real_FUN_800045e8 = func_find->second; return hh_wrap_FUN_800045e8;
            case 0x80017384: if (hh_real_FUN_80017384 == nullptr) hh_real_FUN_80017384 = func_find->second; return hh_wrap_FUN_80017384;
            case 0x800173B8: if (hh_real_FUN_800173b8 == nullptr) hh_real_FUN_800173b8 = func_find->second; return hh_wrap_FUN_800173b8;
            case 0x80125814: if (hh_real_FUN_80125814 == nullptr) hh_real_FUN_80125814 = func_find->second; return hh_wrap_FUN_80125814;
            case 0x80125808: if (hh_real_FUN_80125808 == nullptr) hh_real_FUN_80125808 = func_find->second; return hh_wrap_FUN_80125808;
            case 0x80124ED8: if (hh_real_FUN_80124ed8 == nullptr) hh_real_FUN_80124ed8 = func_find->second; return hh_wrap_FUN_80124ed8;
            case 0x80000A0C: if (hh_real_FUN_80000a0c == nullptr) hh_real_FUN_80000a0c = func_find->second; return hh_wrap_FUN_80000a0c;
            case 0x80000934: if (hh_real_FUN_80000934 == nullptr) hh_real_FUN_80000934 = func_find->second; return hh_wrap_FUN_80000934;
            case 0x80000984: if (hh_real_FUN_80000984 == nullptr) hh_real_FUN_80000984 = func_find->second; return hh_wrap_FUN_80000984;
            case 0x80022044: if (hh_real_FUN_80022044 == nullptr) hh_real_FUN_80022044 = func_find->second; return hh_wrap_FUN_80022044;
            case 0x80020460: if (hh_real_FUN_80020460 == nullptr) hh_real_FUN_80020460 = func_find->second; return hh_wrap_FUN_80020460;
            case 0x8001FD14: if (hh_real_FUN_8001fd14 == nullptr) hh_real_FUN_8001fd14 = func_find->second; return hh_wrap_FUN_8001fd14;
            case 0x8001FBA8: if (hh_real_FUN_8001fba8 == nullptr) hh_real_FUN_8001fba8 = func_find->second; return hh_wrap_FUN_8001fba8;
            case 0x80021EB8: if (hh_real_FUN_80021eb8 == nullptr) hh_real_FUN_80021eb8 = func_find->second; return hh_wrap_FUN_80021eb8;
            case 0x8002C4D0: if (hh_real_FUN_8002c4d0 == nullptr) hh_real_FUN_8002c4d0 = func_find->second; return hh_wrap_FUN_8002c4d0;
            case 0x8002C7F4: if (hh_real_FUN_8002c7f4 == nullptr) hh_real_FUN_8002c7f4 = func_find->second; return hh_wrap_FUN_8002c7f4;
            case 0x80000ED0: if (hh_real_FUN_80000ed0 == nullptr) hh_real_FUN_80000ed0 = func_find->second; return hh_wrap_FUN_80000ed0;
            case 0x800207D0: if (hh_real_FUN_800207d0 == nullptr) hh_real_FUN_800207d0 = func_find->second; return hh_wrap_FUN_800207d0;
            case 0x80020F60: if (hh_real_FUN_80020f60 == nullptr) hh_real_FUN_80020f60 = func_find->second; return hh_wrap_FUN_80020f60;
            case 0x8002059C: if (hh_real_FUN_8002059c == nullptr) hh_real_FUN_8002059c = func_find->second; return hh_wrap_FUN_8002059c;
            case 0x80022B6C: if (hh_real_FUN_80022b6c == nullptr) hh_real_FUN_80022b6c = func_find->second; return hh_wrap_FUN_80022b6c;
            case 0x80035050: if (hh_real_FUN_80035050 == nullptr) hh_real_FUN_80035050 = func_find->second; return hh_wrap_FUN_80035050;
            case 0x800058DC: if (hh_real_FUN_800058dc == nullptr) hh_real_FUN_800058dc = func_find->second; return hh_wrap_FUN_800058dc;
            case 0x80003824: if (hh_real_FUN_80003824 == nullptr) hh_real_FUN_80003824 = func_find->second; return hh_wrap_FUN_80003824;
            case 0x8000469C: if (hh_real_FUN_8000469c == nullptr) hh_real_FUN_8000469c = func_find->second; return hh_wrap_FUN_8000469c;
            case 0x80004310: if (hh_real_FUN_80004310 == nullptr) hh_real_FUN_80004310 = func_find->second; return hh_wrap_FUN_80004310;
            case 0x8000433C: if (hh_real_FUN_8000433C == nullptr) hh_real_FUN_8000433C = func_find->second; return hh_wrap_FUN_8000433C;
            case 0x801BFAA0: if (hh_real_M24_FUN_801bfaa0 == nullptr) hh_real_M24_FUN_801bfaa0 = func_find->second; return hh_wrap_M24_FUN_801bfaa0;
            case 0x801BFFAC: if (hh_real_M24_FUN_801bffac == nullptr) hh_real_M24_FUN_801bffac = func_find->second; return hh_wrap_M24_FUN_801bffac;
            case 0x801C0B8C: if (hh_real_FUN_801C0B8C == nullptr) hh_real_FUN_801C0B8C = func_find->second; return hh_wrap_FUN_801C0B8C;
            case 0x801BFF20: if (hh_real_M24_FUN_801bff20 == nullptr) hh_real_M24_FUN_801bff20 = func_find->second; return hh_wrap_M24_FUN_801bff20;
            case 0x801BF398: if (hh_real_M24_FUN_801bf398 == nullptr) hh_real_M24_FUN_801bf398 = func_find->second; return hh_wrap_M24_FUN_801bf398;
            case 0x801C02FC: if (hh_real_M24_FUN_801c02fc == nullptr) hh_real_M24_FUN_801c02fc = func_find->second; return hh_wrap_M24_FUN_801c02fc;
            case 0x801C0C68: if (hh_real_M24_FUN_801c0c68 == nullptr) hh_real_M24_FUN_801c0c68 = func_find->second; return hh_wrap_M24_FUN_801c0c68;
            case 0x801C012C: if (hh_real_M24_FUN_801c012c == nullptr) hh_real_M24_FUN_801c012c = func_find->second; return hh_wrap_M24_FUN_801c012c;
            case 0x801C0190: if (hh_real_M24_FUN_801c0190 == nullptr) hh_real_M24_FUN_801c0190 = func_find->second; return hh_wrap_M24_FUN_801c0190;
            case 0x801BF610: if (hh_real_M24_FUN_801bf610 == nullptr) hh_real_M24_FUN_801bf610 = func_find->second; return hh_wrap_M24_FUN_801bf610;
            case 0x801BF61C: if (hh_real_M24_FUN_801bf61c == nullptr) hh_real_M24_FUN_801bf61c = func_find->second; return hh_wrap_M24_FUN_801bf61c;
            case 0x80031190: if (hh_real_FUN_80031190 == nullptr) hh_real_FUN_80031190 = func_find->second; return hh_wrap_FUN_80031190;
            case 0x801C0464: if (hh_real_M24_FUN_801c0464 == nullptr) hh_real_M24_FUN_801c0464 = func_find->second; return hh_wrap_M24_FUN_801c0464;
            case 0x801BF850: if (hh_real_M24_FUN_801bf850 == nullptr) hh_real_M24_FUN_801bf850 = func_find->second; return hh_wrap_M24_FUN_801bf850;
            case 0x801BFA58: if (hh_real_M24_FUN_801bfa58 == nullptr) hh_real_M24_FUN_801bfa58 = func_find->second; return hh_wrap_M24_FUN_801bfa58;
            case 0x8038BCE0: if (hh_real_FUN_8038BCE0 == nullptr) hh_real_FUN_8038BCE0 = func_find->second; return hh_wrap_FUN_8038BCE0;
            case 0x8038C914: if (hh_real_FUN_8038C914 == nullptr) hh_real_FUN_8038C914 = func_find->second; return hh_wrap_FUN_8038C914;
            case 0x8038CA8C: if (hh_real_FUN_8038CA8C == nullptr) hh_real_FUN_8038CA8C = func_find->second; return hh_wrap_FUN_8038CA8C;
            case 0x8038D224: if (hh_real_FUN_8038D224 == nullptr) hh_real_FUN_8038D224 = func_find->second; return hh_wrap_FUN_8038D224;
            case 0x801C2420: if (hh_real_M24_FUN_801c2420 == nullptr) hh_real_M24_FUN_801c2420 = func_find->second; return hh_wrap_M24_FUN_801c2420;
            case 0x801C242C: if (hh_real_M24_FUN_801c242c == nullptr) hh_real_M24_FUN_801c242c = func_find->second; return hh_wrap_M24_FUN_801c242c;
            case 0x801E389C: if (hh_real_M25_FUN_801e389c == nullptr) hh_real_M25_FUN_801e389c = func_find->second; return hh_wrap_M25_FUN_801e389c;
            case 0x801E39F8: if (hh_real_M25_FUN_801e39f8 == nullptr) hh_real_M25_FUN_801e39f8 = func_find->second; return hh_wrap_M25_FUN_801e39f8;
            case 0x80005270: if (hh_real_FUN_80005270 == nullptr) hh_real_FUN_80005270 = func_find->second; return hh_wrap_FUN_80005270;
            case 0x801CBE88: if (hh_real_FUN_801CBE88 == nullptr) hh_real_FUN_801CBE88 = func_find->second; return hh_wrap_FUN_801CBE88;
            case 0x801CBDC0: if (hh_real_FUN_801CBDC0 == nullptr) hh_real_FUN_801CBDC0 = func_find->second; return hh_wrap_FUN_801CBDC0;
            case 0x801BF1CC: if (hh_real_FUN_801BF1CC == nullptr) hh_real_FUN_801BF1CC = func_find->second; return hh_wrap_FUN_801BF1CC;
            case 0x801C1508: if (hh_real_FUN_801C1508 == nullptr) hh_real_FUN_801C1508 = func_find->second; return hh_wrap_FUN_801C1508;
            case 0x801C2050: if (hh_real_FUN_801C2050 == nullptr) hh_real_FUN_801C2050 = func_find->second; return hh_wrap_FUN_801C2050;
            case 0x80124C54: if (hh_real_FUN_80124C54 == nullptr) hh_real_FUN_80124C54 = func_find->second; return hh_wrap_FUN_80124C54;
            case 0x8012FD1C: if (hh_real_FUN_8012FD1C == nullptr) hh_real_FUN_8012FD1C = func_find->second; return hh_wrap_FUN_8012FD1C;
            case 0x8012FE50: if (hh_real_FUN_8012FE50 == nullptr) hh_real_FUN_8012FE50 = func_find->second; return hh_wrap_FUN_8012FE50;
            case 0x801C5A0C: if (hh_real_FUN_801C5A0C == nullptr) hh_real_FUN_801C5A0C = func_find->second; return hh_wrap_FUN_801C5A0C;
            case 0x80125968: if (hh_real_FUN_80125968 == nullptr) hh_real_FUN_80125968 = func_find->second; return hh_wrap_FUN_80125968;
            case 0x801257DC: if (hh_real_FUN_801257DC == nullptr) hh_real_FUN_801257DC = func_find->second; return hh_wrap_FUN_801257DC;
            case 0x80126A18: if (hh_real_FUN_80126A18 == nullptr) hh_real_FUN_80126A18 = func_find->second; return hh_wrap_FUN_80126A18;
            case 0x801267C0: if (hh_real_FUN_801267C0 == nullptr) hh_real_FUN_801267C0 = func_find->second; return hh_wrap_FUN_801267C0;
            default: break;
        }
    }
    return func_find->second;
}

std::unordered_map<recomp_func_t*, recomp::overlays::BasePatchedFunction> recomp::overlays::get_base_patched_funcs() {
    std::unordered_map<recomp_func_t*, BasePatchedFunction> ret{};

    // Collect the set of all functions in the patches.
    std::unordered_map<recomp_func_t*, BasePatchedFunction> all_patch_funcs{};
    for (size_t patch_section_index = 0; patch_section_index < num_patch_code_sections; patch_section_index++) {
        const auto& patch_section = patch_code_sections[patch_section_index];
        for (size_t func_index = 0; func_index < patch_section.num_funcs; func_index++) {
            all_patch_funcs.emplace(patch_section.funcs[func_index].func, BasePatchedFunction{ .patch_section = patch_section_index, .function_index = func_index });
        }
    }

    // Check every vanilla function against the full patch function set.
    // Any functions in both are patched.
    for (size_t code_section_index = 0; code_section_index < sections_info.num_code_sections; code_section_index++) {
        const auto& code_section = sections_info.code_sections[code_section_index];
        for (size_t func_index = 0; func_index < code_section.num_funcs; func_index++) {
            recomp_func_t* cur_func = code_section.funcs[func_index].func;
            // If this function also exists in the patches function set then it's a vanilla function that was patched.
            auto find_it = all_patch_funcs.find(cur_func);
            if (find_it != all_patch_funcs.end()) {
                ret.emplace(cur_func, find_it->second);
            }
        }
    }

    return ret;
}

const std::unordered_map<uint32_t, uint16_t>& recomp::overlays::get_patch_vrom_to_section_map() {
    return patch_code_sections_by_rom;
}

uint32_t recomp::overlays::get_patch_section_ram_addr(uint16_t patch_code_section_index) {
    if (patch_code_section_index < num_patch_code_sections) {
        return patch_code_sections[patch_code_section_index].ram_addr;
    }
    assert(false);
    return -1;
}

uint32_t recomp::overlays::get_patch_section_rom_addr(uint16_t patch_code_section_index) {
    if (patch_code_section_index < num_patch_code_sections) {
        return patch_code_sections[patch_code_section_index].rom_addr;
    }
    assert(false);
    return -1;
}

const FuncEntry* recomp::overlays::get_patch_function_entry(uint16_t patch_code_section_index, size_t function_index) {
    if (patch_code_section_index < num_patch_code_sections) {
        const auto& section = patch_code_sections[patch_code_section_index];
        if (function_index < section.num_funcs) {
            return &section.funcs[function_index];
        }
    }
    assert(false);
    return nullptr;
}

// Finds a base patched function given a patch section's index and the function's offset into the section.
bool recomp::overlays::get_patch_func_entry_by_section_index_function_offset(uint16_t patch_code_section_index, uint32_t function_offset, FuncEntry& func_out) {
    if (patch_code_section_index >= num_patch_code_sections) {
        return false;
    }

    SectionTableEntry* section = &patch_code_sections[patch_code_section_index];
    if (function_offset >= section->size) {
        return false;
    }
    
    // TODO avoid a linear lookup here.
    for (size_t func_index = 0; func_index < section->num_funcs; func_index++) {
        if (section->funcs[func_index].offset == function_offset) {
            func_out = section->funcs[func_index];
            return true;
        }
    }

    return false;
}

std::span<const RelocEntry> recomp::overlays::get_patch_section_relocs(uint16_t patch_code_section_index) {
    if (patch_code_section_index < num_patch_code_sections) {
        const auto& section = patch_code_sections[patch_code_section_index];
        return std::span{ section.relocs, section.num_relocs };
    }
    assert(false);
    return {};
}

std::span<const uint8_t> recomp::overlays::get_patch_binary() {
    return std::span{ reinterpret_cast<const uint8_t*>(patch_data.data()), patch_data.size() };
}
