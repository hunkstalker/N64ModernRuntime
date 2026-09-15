#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <ultramodern/ultra64.h>
#include <ultramodern/ultramodern.hpp>

#include "recomp.h"
#include "helpers.hpp"

// HH: Controller Pak (mempak) mínimo en RAM, persistido a disco.
// El host no emula el dispositivo SI real: implementamos aquí la semántica de
// alto nivel de PFS (init/formato, asignación, búsqueda, lectura/escritura,
// borrado y estado) sobre un pak de 32 KB con un sistema de ficheros simple.
// Ver TODO #15.

enum {
    PFS_ERR_NOPACK      = 1,
    PFS_ERR_NEW_PACK    = 2,
    PFS_ERR_INCONSISTENT = 3,
    PFS_ERR_CONTRFAIL   = 4,
    PFS_ERR_INVALID     = 5,
    PFS_ERR_BAD_DATA    = 6,
    PFS_ERR_ID_FATAL    = 7,
    PFS_ERR_DEVICE      = 8,
    PFS_ERR_NO_FILE     = 10,
};

#define OS_READ_FLAG  0
#define OS_WRITE_FLAG 1

#define PAK_SIZE       0x8000
#define PAK_RESERVED   0x200
#define PAK_MAX_FILES  16

struct PakFile {
    bool used = false;
    uint16_t company = 0;
    uint32_t game = 0;
    uint8_t game_name[4] = {0, 0, 0, 0};
    uint8_t ext_name[4] = {0, 0, 0, 0};
    uint32_t size = 0;
    std::vector<uint8_t> data;
};

struct PakState {
    bool loaded = false;
    std::vector<PakFile> files;
};

static PakState g_pak;

static std::filesystem::path pak_path() {
    std::filesystem::path p = ultramodern::get_save_file_path();
    if (p.empty()) {
        return std::filesystem::path("saves") / "mempak.bin";
    }
    return p.string() + ".pak";
}

static void pak_save() {
    std::filesystem::path path = pak_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        fprintf(stderr, "[PAK] no se pudo escribir %s\n", path.string().c_str());
        return;
    }
    const char magic[4] = {'H', 'H', 'P', 'K'};
    uint32_t count = static_cast<uint32_t>(g_pak.files.size());
    out.write(magic, 4);
    out.write(reinterpret_cast<const char*>(&count), 4);
    for (const PakFile& f : g_pak.files) {
        uint8_t used = f.used ? 1 : 0;
        out.write(reinterpret_cast<const char*>(&used), 1);
        out.write(reinterpret_cast<const char*>(&f.company), 2);
        out.write(reinterpret_cast<const char*>(&f.game), 4);
        out.write(reinterpret_cast<const char*>(f.game_name), 4);
        out.write(reinterpret_cast<const char*>(f.ext_name), 4);
        out.write(reinterpret_cast<const char*>(&f.size), 4);
        if (f.used && !f.data.empty()) {
            out.write(reinterpret_cast<const char*>(f.data.data()), f.data.size());
        }
    }
}

static void pak_load() {
    g_pak.loaded = true;
    g_pak.files.clear();
    std::ifstream in(pak_path(), std::ios::binary);
    if (!in) {
        return;
    }
    char magic[4] = {};
    uint32_t count = 0;
    in.read(magic, 4);
    if (magic[0] != 'H' || magic[1] != 'H' || magic[2] != 'P' || magic[3] != 'K') {
        return;
    }
    in.read(reinterpret_cast<char*>(&count), 4);
    for (uint32_t i = 0; i < count && i < PAK_MAX_FILES; i++) {
        PakFile f;
        uint8_t used = 0;
        in.read(reinterpret_cast<char*>(&used), 1);
        in.read(reinterpret_cast<char*>(&f.company), 2);
        in.read(reinterpret_cast<char*>(&f.game), 4);
        in.read(reinterpret_cast<char*>(f.game_name), 4);
        in.read(reinterpret_cast<char*>(f.ext_name), 4);
        in.read(reinterpret_cast<char*>(&f.size), 4);
        f.used = used != 0;
        if (f.size > PAK_SIZE) {
            f.size = PAK_SIZE;
        }
        f.data.assign(f.size, 0);
        if (f.used && f.size > 0 && !f.data.empty()) {
            in.read(reinterpret_cast<char*>(f.data.data()), f.data.size());
            if (!in) {
                std::fill(f.data.begin(), f.data.end(), 0);
            }
        }
        g_pak.files.push_back(std::move(f));
    }
}

static void pak_ensure_loaded() {
    if (!g_pak.loaded) {
        pak_load();
    }
}

static uint32_t pak_used_bytes() {
    uint32_t used = PAK_RESERVED;
    for (const PakFile& f : g_pak.files) {
        if (f.used) {
            used += (f.size + 0xFF) & ~0xFFu;
        }
    }
    return used < PAK_SIZE ? used : PAK_SIZE;
}

static int pak_find(uint16_t company, uint32_t game, const uint8_t game_name[4], const uint8_t ext_name[4]) {
    for (size_t i = 0; i < g_pak.files.size(); i++) {
        const PakFile& f = g_pak.files[i];
        if (f.used && f.company == company && f.game == game &&
            memcmp(f.game_name, game_name, 4) == 0 && memcmp(f.ext_name, ext_name, 4) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

static void pak_init_fields(OSPfs* pfs, int channel) {
    memset(pfs, 0, sizeof(OSPfs));
    pfs->channel = channel;
    pfs->status = 0;
    pfs->banks = 1;
    pfs->activebank = 0;
    static const u8 id[8] = {'N', '6', '4', ' ', 'P', 'F', 'S', 0};
    memcpy(pfs->id, id, sizeof(id));
}

extern "C" void osPfsInitPak_recomp(uint8_t* rdram, recomp_context* ctx) {
    OSPfs* pfs = _arg<1, OSPfs*>(rdram, ctx);
    int channel = _arg<2, int>(rdram, ctx);
    pak_ensure_loaded();

    if (channel != 0 || pfs == nullptr) {
        _return<s32>(ctx, PFS_ERR_INVALID);
        return;
    }
    pak_init_fields(pfs, channel);
    _return<s32>(ctx, 0);
}

// Formato: deja el pak vacío (el juego lo usa para formatear un pak nuevo).
extern "C" void osPfsInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    OSPfs* pfs = _arg<1, OSPfs*>(rdram, ctx);
    int channel = _arg<2, int>(rdram, ctx);
    pak_ensure_loaded();

    if (channel != 0 || pfs == nullptr) {
        _return<s32>(ctx, PFS_ERR_INVALID);
        return;
    }
    g_pak.files.clear();
    pak_save();
    pak_init_fields(pfs, channel);
    _return<s32>(ctx, 0);
}

extern "C" void osPfsFreeBlocks_recomp(uint8_t* rdram, recomp_context* ctx) {
    s32* bytes = _arg<1, s32*>(rdram, ctx);
    pak_ensure_loaded();
    if (bytes != nullptr) {
        *bytes = static_cast<s32>(PAK_SIZE - pak_used_bytes());
    }
    _return<s32>(ctx, 0);
}

extern "C" void osPfsAllocateFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint16_t company = _arg<1, u16>(rdram, ctx);
    uint32_t game = _arg<2, u32>(rdram, ctx);
    u8* game_name = _arg<3, u8*>(rdram, ctx);
    u8* ext_name = _arg<4, u8*>(rdram, ctx);
    s32 size = _arg<5, s32>(rdram, ctx);
    s32* file_no = _arg<6, s32*>(rdram, ctx);
    pak_ensure_loaded();

    if (size < 0 || file_no == nullptr) {
        _return<s32>(ctx, PFS_ERR_INVALID);
        return;
    }
    if (pak_used_bytes() + ((static_cast<uint32_t>(size) + 0xFF) & ~0xFFu) > PAK_SIZE) {
        _return<s32>(ctx, PFS_ERR_INCONSISTENT);
        return;
    }
    int slot = -1;
    for (size_t i = 0; i < g_pak.files.size(); i++) {
        if (!g_pak.files[i].used) {
            slot = static_cast<int>(i);
            break;
        }
    }
    if (slot < 0) {
        if (g_pak.files.size() >= PAK_MAX_FILES) {
            _return<s32>(ctx, PFS_ERR_INCONSISTENT);
            return;
        }
        g_pak.files.emplace_back();
        slot = static_cast<int>(g_pak.files.size()) - 1;
    }

    PakFile& f = g_pak.files[slot];
    f.used = true;
    f.company = company;
    f.game = game;
    if (game_name != nullptr) {
        memcpy(f.game_name, game_name, 4);
    }
    if (ext_name != nullptr) {
        memcpy(f.ext_name, ext_name, 4);
    }
    f.size = static_cast<uint32_t>(size);
    f.data.assign(size, 0);
    *file_no = slot;
    pak_save();
    _return<s32>(ctx, 0);
}

extern "C" void osPfsFindFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint16_t company = _arg<1, u16>(rdram, ctx);
    uint32_t game = _arg<2, u32>(rdram, ctx);
    u8* game_name = _arg<3, u8*>(rdram, ctx);
    u8* ext_name = _arg<4, u8*>(rdram, ctx);
    s32* file_no = _arg<5, s32*>(rdram, ctx);
    pak_ensure_loaded();

    if (game_name == nullptr || ext_name == nullptr || file_no == nullptr) {
        _return<s32>(ctx, PFS_ERR_INVALID);
        return;
    }
    int idx = pak_find(company, game, game_name, ext_name);
    if (idx < 0) {
        _return<s32>(ctx, PFS_ERR_NO_FILE);
        return;
    }
    *file_no = idx;
    _return<s32>(ctx, 0);
}

extern "C" void osPfsDeleteFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint16_t company = _arg<1, u16>(rdram, ctx);
    uint32_t game = _arg<2, u32>(rdram, ctx);
    u8* game_name = _arg<3, u8*>(rdram, ctx);
    u8* ext_name = _arg<4, u8*>(rdram, ctx);
    pak_ensure_loaded();

    if (game_name == nullptr || ext_name == nullptr) {
        _return<s32>(ctx, PFS_ERR_INVALID);
        return;
    }
    int idx = pak_find(company, game, game_name, ext_name);
    if (idx < 0) {
        _return<s32>(ctx, PFS_ERR_NO_FILE);
        return;
    }
    PakFile& f = g_pak.files[idx];
    f.used = false;
    f.size = 0;
    f.data.clear();
    pak_save();
    _return<s32>(ctx, 0);
}

extern "C" void osPfsReadWriteFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    s32 file_no = _arg<1, s32>(rdram, ctx);
    u8 flag = _arg<2, u8>(rdram, ctx);
    s32 offset = _arg<3, s32>(rdram, ctx);
    s32 size = _arg<4, s32>(rdram, ctx);
    u8* buffer = _arg<5, u8*>(rdram, ctx);
    pak_ensure_loaded();

    if (file_no < 0 || static_cast<size_t>(file_no) >= g_pak.files.size() ||
        offset < 0 || size < 0 || buffer == nullptr) {
        _return<s32>(ctx, PFS_ERR_INVALID);
        return;
    }
    PakFile& f = g_pak.files[file_no];
    if (!f.used) {
        _return<s32>(ctx, PFS_ERR_INVALID);
        return;
    }
    if (static_cast<uint32_t>(offset) > f.size) {
        _return<s32>(ctx, PFS_ERR_INVALID);
        return;
    }
    uint32_t count = static_cast<uint32_t>(size);
    if (static_cast<uint32_t>(offset) + count > f.size) {
        count = f.size - static_cast<uint32_t>(offset);
    }

    if (flag == OS_WRITE_FLAG) {
        if (count > 0) {
            memcpy(f.data.data() + offset, buffer, count);
        }
        pak_save();
    } else {
        if (count > 0) {
            memcpy(buffer, f.data.data() + offset, count);
        }
    }
    _return<s32>(ctx, 0);
}

extern "C" void osPfsFileState_recomp(uint8_t* rdram, recomp_context* ctx) {
    s32 file_no = _arg<1, s32>(rdram, ctx);
    s32 state_ptr = _arg<2, s32>(rdram, ctx);
    pak_ensure_loaded();

    if (file_no < 0 || static_cast<size_t>(file_no) >= g_pak.files.size() ||
        state_ptr == 0) {
        _return<s32>(ctx, PFS_ERR_INVALID);
        return;
    }
    const PakFile& f = g_pak.files[file_no];
    if (!f.used) {
        _return<s32>(ctx, PFS_ERR_INVALID);
        return;
    }

    MEM_W(state_ptr, 0x0) = f.size;
    MEM_W(state_ptr, 0x4) = f.game;
    MEM_H(state_ptr, 0x8) = f.company;
    for (int i = 0; i < 4; i++) {
        MEM_B(state_ptr, 0xA + i) = f.ext_name[i];
    }
    _return<s32>(ctx, 0);
}

extern "C" void osPfsNumFiles_recomp(uint8_t* rdram, recomp_context* ctx) {
    s32* max_files = _arg<1, s32*>(rdram, ctx);
    s32* files_used = _arg<2, s32*>(rdram, ctx);
    pak_ensure_loaded();

    int used = 0;
    for (const PakFile& f : g_pak.files) {
        if (f.used) {
            used++;
        }
    }
    if (max_files != nullptr) {
        *max_files = PAK_MAX_FILES;
    }
    if (files_used != nullptr) {
        *files_used = used;
    }
    _return<s32>(ctx, 0);
}

extern "C" void osPfsIsPlug_recomp(uint8_t* rdram, recomp_context* ctx) {
    u8* pattern = _arg<1, u8*>(rdram, ctx);
    pak_ensure_loaded();
    if (pattern != nullptr) {
        *pattern = 0x01; // pak presente en el canal 0
    }
    _return<s32>(ctx, 0);
}

extern "C" void osPfsChecker_recomp(uint8_t* rdram, recomp_context* ctx) {
    s32* errors = _arg<2, s32*>(rdram, ctx);
    if (errors != nullptr) {
        *errors = 0;
    }
    _return<s32>(ctx, 0);
}

extern "C" void osPfsRepairId_recomp(uint8_t* rdram, recomp_context* ctx) {
    _return<s32>(ctx, 0);
}

// Game Boy Pak: libultra `osGbpakInit` devuelve GB_PAK_ERR_NOPAK (0xB) cuando no hay accesorio.
#define GB_PAK_ERR_NOPAK 0xB
extern "C" void osGbpakInit_recomp(uint8_t * rdram, recomp_context* ctx) {
    OSPfs* pfs = _arg<1, OSPfs*>(rdram, ctx);
    int channel = _arg<2, int>(rdram, ctx);
    if (pfs != nullptr) {
        pfs->status = GB_PAK_ERR_NOPAK;
        pfs->channel = channel;
    }
    _return<s32>(ctx, GB_PAK_ERR_NOPAK);
}
