#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ultramodern/ultramodern.hpp"

#include "recomp.h"
#include "recompiler/context.h"
#include "overlays.hpp"
#include "sections.h"

static recomp::overlays::overlay_section_table_data_t sections_info {};
static recomp::overlays::overlays_by_index_t overlays_info {};

// HH: registro con marca de tiempo de carga de overlays/modulos (hh_ovl.log, acotado) para
// correlacionar escenas y transiciones con hh_state.log y hh_pi.log.
static void hh_ovl_log(const char* fmt, ...) {
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
            func_map[section.ram_addr + func.offset] = func.func;
        }
    }
}

void load_overlay(size_t section_table_index, int32_t ram) {
    const SectionTableEntry& section = sections_info.code_sections[section_table_index];
    hh_ovl_log("load section=%zu idx=%u rom=%08X ram=%08X funcs=%zu\n", section_table_index,
               (unsigned)section.index, (unsigned)section.rom_addr, (unsigned)ram,
               (size_t)section.num_funcs);

    for (size_t function_index = 0; function_index < section.num_funcs; function_index++) {
        const FuncEntry& func = section.funcs[function_index];
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
    if (hh_calltrace_fp == nullptr) {
        const char* e = getenv("HH_CALLTRACE");
        if (e == nullptr || *e == 0) return;
        hh_calltrace_fp = fopen(e, "wb");
        if (hh_calltrace_fp == nullptr) return;
    }
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
static void hh_wrap_FUN_80017064(uint8_t* rdram, recomp_context* ctx) {
    uint32_t id = (uint32_t)ctx->r4 & 0xFFFF;
    uint32_t sp_before = (uint32_t)ctx->r29;
    uint32_t ra_before = (uint32_t)ctx->r31;
    hh_real_FUN_80017064(rdram, ctx);
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
static int hh_tid(uint8_t* rdram) {
    PTR(OSThread) t = ultramodern::this_thread();
    return t == NULLPTR ? -1 : (int)TO_PTR(OSThread, t)->id;
}
static void hh_wrap_FUN_80000a0c(uint8_t* rdram, recomp_context* ctx) {
    auto r32 = [&](uint32_t a){ return *(uint32_t*)&rdram[a & 0x1FFFFFFF]; };
    static uint64_t hh_n = 0;
    uint32_t obj = (uint32_t)ctx->r4, msg = (uint32_t)ctx->r5;
    uint32_t head = r32(obj + 0x888);
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
static recomp_func_t* hh_real_FUN_80003824 = nullptr;
static void hh_wrap_FUN_80003824(uint8_t* rdram, recomp_context* ctx) {
    uint32_t src = (uint32_t)ctx->r4;
    uint32_t dst = (uint32_t)ctx->r5;
    if (getenv("HH_TBLTRACE") != nullptr) {
        fprintf(stderr, "[LD384] a0=%08X a1=%08X a2=%08X a3=%08X\n",
                src, dst, (unsigned)ctx->r6, (unsigned)ctx->r7);
    }
    hh_real_FUN_80003824(rdram, ctx);
    recomp::overlays::load_module_by_source(src, (int32_t)dst);
}
// HH: cadena del nodo de boot: setter de callback (FUN_800058DC), dispatcher y pasos del
// módulo 23 que deben avanzar 0x801CFE00/02.
static recomp_func_t* hh_real_FUN_800058dc = nullptr;
static void hh_wrap_FUN_800058dc(uint8_t* rdram, recomp_context* ctx) {
    fprintf(stderr, "[SETCB] obj=%08X cb=%08X\n", (unsigned)ctx->r4, (unsigned)ctx->r5);
    hh_real_FUN_800058dc(rdram, ctx);
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
        fprintf(stderr, "[M23] FUN_80124C54 n=%llu a0=%08X a1=%08X bd6d=%02X bd6e=%02X bc1c=%04X\n",
            (unsigned long long)n, (unsigned)ctx->r4, (unsigned)ctx->r5,
            (unsigned)rdram[(0x801BBD6D ^ 3) & 0x7FFFFF], (unsigned)rdram[(0x801BBD6E ^ 3) & 0x7FFFFF],
            (unsigned)*(uint16_t*)&rdram[(0x801BBC1C ^ 2) & 0x7FFFFF]);
    }
    n++;
    hh_real_FUN_80124C54(rdram, ctx);
}
static recomp_func_t* hh_real_FUN_8012FD1C = nullptr; // condicion que instala 80124CEC
static void hh_wrap_FUN_8012FD1C(uint8_t* rdram, recomp_context* ctx) {
    uint32_t a0 = (uint32_t)ctx->r4;
    hh_real_FUN_8012FD1C(rdram, ctx);
    static int n = 0;
    if (n < 30 || ((n % 500) == 0 && (uint32_t)ctx->r2 != 0)) {
        fprintf(stderr, "[M23] FUN_8012FD1C n=%d a0=%08X -> %08X\n", n, a0, (unsigned)ctx->r2);
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
static recomp_func_t* hh_real_FUN_80000ed0 = nullptr; // submit de task con contador +0x89C
static void hh_wrap_FUN_80000ed0(uint8_t* rdram, recomp_context* ctx) {
    auto r32 = [&](uint32_t a){ return *(uint32_t*)&rdram[a & 0x7FFFFF]; };
    uint32_t mq = (uint32_t)ctx->r5, msg = (uint32_t)ctx->r6;
    uint32_t before = r32(0x8005CD4C);
    fprintf(stderr, "[SUBM] tid=%d msg=%08X mq=%08X fl8=%08X cnt=%u\n", hh_tid(rdram), msg, mq, r32((msg & 0x7FFFFF) + 8), before);
    hh_real_FUN_80000ed0(rdram, ctx);
    fprintf(stderr, "[SUBM] -> cnt=%u\n", r32(0x8005CD4C));
}

// HH: declaraciones para el registro de llamadas (diagnostico de pila).
extern "C" int hh_watch_active;
extern "C" recomp_context* hh_get_current_ctx(void);
extern "C" uint8_t* hh_get_rdram_base(void);
extern "C" void hh_ring_record(uint32_t target, uint32_t sp);
extern "C" void hh_ring2_record(uint32_t target, uint32_t sp);
extern "C" void hh_callring_record(uint32_t addr);
extern "C" recomp_context* hh_get_current_ctx(void);
extern "C" uint8_t* hh_get_rdram_base(void);

extern "C" recomp_func_t * get_function(int32_t addr) {
    hh_calltrace((uint32_t)addr);
    hh_callring_record((uint32_t)addr);
    // HH: vigilancia del dispatch del bucle principal (livelock del dano): registra, en cada
    // llamada al frame (0x80001454) o al no-op (0x80001BB0), el registro s0 (r16) y el valor de
    // memoria que consulta el branch (lhu 0x0(s0)). Gated por HH_MQLOG_ALL.
    if (((uint32_t)addr == 0x80001454u || (uint32_t)addr == 0x80001BB0u ||
         (uint32_t)addr == 0x8000290Cu || (uint32_t)addr == 0x800266B0u) && getenv("HH_MQLOG_ALL") != nullptr) {
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
    // HH: registrar (target, sp) de cada llamada para diagnosticar hundimientos de pila.
    if (hh_watch_active) {
        recomp_context* hh_c = hh_get_current_ctx();
        uint32_t hh_sp = hh_c != nullptr ? (uint32_t)hh_c->r29 : 0;
        hh_ring_record((uint32_t)addr, hh_sp);
        hh_ring2_record((uint32_t)addr, hh_sp);
    }
    auto func_find = func_map.find(addr);
    if (func_find == func_map.end()) {
        fprintf(stderr, "Failed to find function at 0x%08X\n", addr);
        // HH: además de stderr, dejarlo en hh_missing.log (CWD) para builds de Windows sin consola.
        if (FILE* mf = fopen("hh_missing.log", "a")) {
            fprintf(mf, "Failed to find function at 0x%08X\n", addr);
            fclose(mf);
        }
        if (getenv("HH_SOFT_LOOKUP") != nullptr) {
            return soft_missing_func;
        }
        assert(false);
        std::exit(EXIT_FAILURE);
    }
    // HH: el loader de módulos debe registrar la sección recompilada en su base real SIEMPRE
    // (no solo en modo traza): soporta la reutilización de bases entre módulos.
    if ((uint32_t)addr == 0x80003824) {
        if (hh_real_FUN_80003824 == nullptr) {
            hh_real_FUN_80003824 = func_find->second;
        }
        return hh_wrap_FUN_80003824;
    }
    if (getenv("HH_TBLTRACE") != nullptr) {
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
            case 0x80005270: if (hh_real_FUN_80005270 == nullptr) hh_real_FUN_80005270 = func_find->second; return hh_wrap_FUN_80005270;
            case 0x801CBE88: if (hh_real_FUN_801CBE88 == nullptr) hh_real_FUN_801CBE88 = func_find->second; return hh_wrap_FUN_801CBE88;
            case 0x801CBDC0: if (hh_real_FUN_801CBDC0 == nullptr) hh_real_FUN_801CBDC0 = func_find->second; return hh_wrap_FUN_801CBDC0;
            case 0x801BF1CC: if (hh_real_FUN_801BF1CC == nullptr) hh_real_FUN_801BF1CC = func_find->second; return hh_wrap_FUN_801BF1CC;
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
