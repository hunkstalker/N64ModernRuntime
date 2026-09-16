#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <ultramodern/hh_paklog.hpp>
#include <ultramodern/ultra64.h>
#include <ultramodern/ultramodern.hpp>

#include "recomp.h"
#include "helpers.hpp"

// HH: Controller Pak (mempak) mínimo en RAM, persistido a disco.
// El host no emula el dispositivo SI real: implementamos aquí la semántica de
// alto nivel de PFS (init/formato, asignación, búsqueda, lectura/escritura,
// borrado y estado) sobre un pak de 32 KB con un sistema de ficheros simple.
// Ver TODO #15.
//
// VOLCADO DE DIAGNÓSTICO (fase guardado, 2026-09-16): activo por defecto -> `hh_pak.log` junto al
// exe (ver ultramodern/hh_paklog.hpp). Por cada llamada se registra el `ra` del llamante (qué
// función del juego la pidió), los argumentos, el retorno y el estado del pak (nuestro + los OSPfs
// del juego en 0x8005CE70+ch*0x68). HH_PAKLOG=0 lo desactiva.
// Notas: notes/2026-09-16-guardado-capsula-pak-y-crash-cac-8021d8d0.md

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

// Direcciones guest del subsistema de guardado del juego (visto en el C generado):
// FUN_80002BE0/FUN_800024D0 usan OSPfs en 0x8005CE70 + canal*0x68 y la cola en 0x8005CE20.
#define HH_GAME_PFS_BASE   0x8005CE70u
#define HH_GAME_PFS_STRIDE 0x68u

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
static bool g_in_paktest = false;
static bool g_paktest_ran = false;
static void hh_pak_selftest();

static std::filesystem::path pak_path() {
    if (g_in_paktest) {
        return std::filesystem::path("hh_paktest.tmp"); // el autotest no toca saves/
    }
    std::filesystem::path p = ultramodern::get_save_file_path();
    if (p.empty()) {
        return std::filesystem::path("saves") / "mempak.bin";
    }
    return p.string() + ".pak";
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

static void hh_name_hex(char out[9], const u8* n) {
    if (n == nullptr) {
        std::strcpy(out, "--------");
        return;
    }
    std::snprintf(out, 9, "%02X%02X%02X%02X", n[0], n[1], n[2], n[3]);
}

// Estado tras la llamada: nuestro pak + los OSPfs que ve el juego (canales 0..3).
static void hh_dump_state(uint8_t* rdram) {
    int usados = 0;
    for (const PakFile& f : g_pak.files) {
        if (f.used) {
            usados++;
        }
    }
    hh_paklog("  estado: files=%zu usados=%d usados_bytes=%u libres=%u", g_pak.files.size(), usados,
              (unsigned)pak_used_bytes(), (unsigned)(PAK_SIZE - pak_used_bytes()));
    for (int ch = 0; ch < 4; ch++) {
        const OSPfs* p = TO_PTR(OSPfs, HH_GAME_PFS_BASE + (uint32_t)ch * HH_GAME_PFS_STRIDE);
        hh_paklog("  pfs[%d] status=%d banks=%u activebank=%u dir_size=%d id=%02X%02X%02X%02X", ch,
                  p->status, (unsigned)p->banks, (unsigned)p->activebank, p->dir_size, p->id[0],
                  p->id[1], p->id[2], p->id[3]);
    }
}

// Registra el retorno (con el `ra` de la llamada) y devuelve, como _return.
#define HH_PAK_RET(nombre, expr)                                                                   \
    do {                                                                                           \
        s32 hh_ret = (s32)(expr);                                                                  \
        hh_paklog("  -> %s = %d", (nombre), (int)hh_ret);                                          \
        _return<s32>(ctx, hh_ret);                                                                 \
        return;                                                                                    \
    } while (0)

static void pak_save() {
    std::filesystem::path path = pak_path();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    hh_paklog("pak_save path=%s files=%zu", path.string().c_str(), g_pak.files.size());

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
        hh_paklog("pak_load path=%s -> no existe (pak vacio)", pak_path().string().c_str());
        return;
    }
    char magic[4] = {};
    uint32_t count = 0;
    in.read(magic, 4);
    if (magic[0] != 'H' || magic[1] != 'H' || magic[2] != 'P' || magic[3] != 'K') {
        hh_paklog("pak_load path=%s -> magic invalido (pak vacio)", pak_path().string().c_str());
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
    hh_paklog("pak_load path=%s -> %zu ficheros", pak_path().string().c_str(), g_pak.files.size());
}

static void pak_ensure_loaded() {
    if (!g_pak.loaded) {
        pak_load();
    }
    if (!g_paktest_ran && !g_in_paktest && hh_paktest_enabled()) {
        g_paktest_ran = true;
        hh_pak_selftest();
    }
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

    hh_paklog("osPfsInitPak ra=%08X ch=%d pfs=%08X", (unsigned)ctx->r31, channel,
              (unsigned)ctx->r5);

    if (channel != 0 || pfs == nullptr) {
        HH_PAK_RET("osPfsInitPak", PFS_ERR_INVALID);
    }
    pak_init_fields(pfs, channel);
    hh_dump_state(rdram);
    HH_PAK_RET("osPfsInitPak", 0);
}

// Formato: deja el pak vacío (el juego lo usa para formatear un pak nuevo).
extern "C" void osPfsInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    OSPfs* pfs = _arg<1, OSPfs*>(rdram, ctx);
    int channel = _arg<2, int>(rdram, ctx);
    pak_ensure_loaded();

    hh_paklog("osPfsInit (formato) ra=%08X ch=%d pfs=%08X", (unsigned)ctx->r31, channel,
              (unsigned)ctx->r5);

    if (channel != 0 || pfs == nullptr) {
        HH_PAK_RET("osPfsInit", PFS_ERR_INVALID);
    }
    g_pak.files.clear();
    pak_save();
    pak_init_fields(pfs, channel);
    hh_dump_state(rdram);
    HH_PAK_RET("osPfsInit", 0);
}

extern "C" void osPfsFreeBlocks_recomp(uint8_t* rdram, recomp_context* ctx) {
    s32* bytes = _arg<1, s32*>(rdram, ctx);
    pak_ensure_loaded();
    if (bytes != nullptr) {
        *bytes = static_cast<s32>(PAK_SIZE - pak_used_bytes());
    }
    hh_paklog("osPfsFreeBlocks ra=%08X -> libres=%d", (unsigned)ctx->r31,
              (int)(PAK_SIZE - pak_used_bytes()));
    HH_PAK_RET("osPfsFreeBlocks", 0);
}

extern "C" void osPfsAllocateFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint16_t company = _arg<1, u16>(rdram, ctx);
    uint32_t game = _arg<2, u32>(rdram, ctx);
    u8* game_name = _arg<3, u8*>(rdram, ctx);
    u8* ext_name = _arg<4, u8*>(rdram, ctx);
    s32 size = _arg<5, s32>(rdram, ctx);
    s32* file_no = _arg<6, s32*>(rdram, ctx);
    pak_ensure_loaded();

    char na[9];
    char nb[9];
    hh_name_hex(na, game_name);
    hh_name_hex(nb, ext_name);
    hh_paklog("osPfsAllocateFile ra=%08X co=%04X game=%08X name=%s ext=%s size=%d libres=%d",
              (unsigned)ctx->r31, (unsigned)company, (unsigned)game, na, nb, (int)size,
              (int)(PAK_SIZE - pak_used_bytes()));

    if (size < 0 || file_no == nullptr) {
        HH_PAK_RET("osPfsAllocateFile", PFS_ERR_INVALID);
    }
    if (pak_used_bytes() + ((static_cast<uint32_t>(size) + 0xFF) & ~0xFFu) > PAK_SIZE) {
        HH_PAK_RET("osPfsAllocateFile", PFS_ERR_INCONSISTENT);
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
            HH_PAK_RET("osPfsAllocateFile", PFS_ERR_INCONSISTENT);
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
    hh_paklog("osPfsAllocateFile file_no=%d", slot);
    pak_save();
    hh_dump_state(rdram);
    HH_PAK_RET("osPfsAllocateFile", 0);
}

extern "C" void osPfsFindFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint16_t company = _arg<1, u16>(rdram, ctx);
    uint32_t game = _arg<2, u32>(rdram, ctx);
    u8* game_name = _arg<3, u8*>(rdram, ctx);
    u8* ext_name = _arg<4, u8*>(rdram, ctx);
    s32* file_no = _arg<5, s32*>(rdram, ctx);
    pak_ensure_loaded();

    char na[9];
    char nb[9];
    hh_name_hex(na, game_name);
    hh_name_hex(nb, ext_name);
    hh_paklog("osPfsFindFile ra=%08X co=%04X game=%08X name=%s ext=%s", (unsigned)ctx->r31,
              (unsigned)company, (unsigned)game, na, nb);

    if (game_name == nullptr || ext_name == nullptr || file_no == nullptr) {
        HH_PAK_RET("osPfsFindFile", PFS_ERR_INVALID);
    }
    int idx = pak_find(company, game, game_name, ext_name);
    if (idx < 0) {
        HH_PAK_RET("osPfsFindFile", PFS_ERR_NO_FILE);
    }
    *file_no = idx;
    HH_PAK_RET("osPfsFindFile", 0);
}

extern "C" void osPfsDeleteFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint16_t company = _arg<1, u16>(rdram, ctx);
    uint32_t game = _arg<2, u32>(rdram, ctx);
    u8* game_name = _arg<3, u8*>(rdram, ctx);
    u8* ext_name = _arg<4, u8*>(rdram, ctx);
    pak_ensure_loaded();

    char na[9];
    char nb[9];
    hh_name_hex(na, game_name);
    hh_name_hex(nb, ext_name);
    hh_paklog("osPfsDeleteFile ra=%08X co=%04X game=%08X name=%s ext=%s", (unsigned)ctx->r31,
              (unsigned)company, (unsigned)game, na, nb);

    if (game_name == nullptr || ext_name == nullptr) {
        HH_PAK_RET("osPfsDeleteFile", PFS_ERR_INVALID);
    }
    int idx = pak_find(company, game, game_name, ext_name);
    if (idx < 0) {
        HH_PAK_RET("osPfsDeleteFile", PFS_ERR_NO_FILE);
    }
    PakFile& f = g_pak.files[idx];
    f.used = false;
    f.size = 0;
    f.data.clear();
    pak_save();
    hh_dump_state(rdram);
    HH_PAK_RET("osPfsDeleteFile", 0);
}

extern "C" void osPfsReadWriteFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    s32 file_no = _arg<1, s32>(rdram, ctx);
    u8 flag = _arg<2, u8>(rdram, ctx);
    s32 offset = _arg<3, s32>(rdram, ctx);
    s32 size = _arg<4, s32>(rdram, ctx);
    u8* buffer = _arg<5, u8*>(rdram, ctx);
    pak_ensure_loaded();

    hh_paklog("osPfsReadWriteFile ra=%08X file_no=%d %s off=%d size=%d buf=%08X", (unsigned)ctx->r31,
              (int)file_no, flag == OS_WRITE_FLAG ? "WRITE" : "READ", (int)offset, (int)size,
              (unsigned)MEM_W(0x14, ctx->r29));

    if (file_no < 0 || static_cast<size_t>(file_no) >= g_pak.files.size() ||
        offset < 0 || size < 0 || buffer == nullptr) {
        HH_PAK_RET("osPfsReadWriteFile", PFS_ERR_INVALID);
    }
    PakFile& f = g_pak.files[file_no];
    if (!f.used) {
        HH_PAK_RET("osPfsReadWriteFile", PFS_ERR_INVALID);
    }
    if (static_cast<uint32_t>(offset) > f.size) {
        HH_PAK_RET("osPfsReadWriteFile", PFS_ERR_INVALID);
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
    HH_PAK_RET("osPfsReadWriteFile", 0);
}

extern "C" void osPfsFileState_recomp(uint8_t* rdram, recomp_context* ctx) {
    s32 file_no = _arg<1, s32>(rdram, ctx);
    s32 state_ptr = _arg<2, s32>(rdram, ctx);
    pak_ensure_loaded();

    hh_paklog("osPfsFileState ra=%08X file_no=%d state=%08X", (unsigned)ctx->r31, (int)file_no,
              (unsigned)state_ptr);

    if (file_no < 0 || static_cast<size_t>(file_no) >= g_pak.files.size() ||
        state_ptr == 0) {
        HH_PAK_RET("osPfsFileState", PFS_ERR_INVALID);
    }
    const PakFile& f = g_pak.files[file_no];
    if (!f.used) {
        HH_PAK_RET("osPfsFileState", PFS_ERR_INVALID);
    }

    MEM_W(state_ptr, 0x0) = f.size;
    MEM_W(state_ptr, 0x4) = f.game;
    MEM_H(state_ptr, 0x8) = f.company;
    for (int i = 0; i < 4; i++) {
        MEM_B(state_ptr, 0xA + i) = f.ext_name[i];
    }
    hh_paklog("  osPfsFileState size=%u game=%08X co=%04X", (unsigned)f.size, (unsigned)f.game,
              (unsigned)f.company);
    HH_PAK_RET("osPfsFileState", 0);
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
    hh_paklog("osPfsNumFiles ra=%08X -> max=%d usados=%d", (unsigned)ctx->r31, PAK_MAX_FILES, used);
    HH_PAK_RET("osPfsNumFiles", 0);
}

extern "C" void osPfsIsPlug_recomp(uint8_t* rdram, recomp_context* ctx) {
    u8* pattern = _arg<1, u8*>(rdram, ctx);
    pak_ensure_loaded();
    if (pattern != nullptr) {
        *pattern = 0x01; // pak presente en el canal 0
    }
    hh_paklog("osPfsIsPlug ra=%08X -> pattern=01", (unsigned)ctx->r31);
    HH_PAK_RET("osPfsIsPlug", 0);
}

extern "C" void osPfsChecker_recomp(uint8_t* rdram, recomp_context* ctx) {
    s32* errors = _arg<2, s32*>(rdram, ctx);
    if (errors != nullptr) {
        *errors = 0;
    }
    hh_paklog("osPfsChecker ra=%08X -> 0", (unsigned)ctx->r31);
    HH_PAK_RET("osPfsChecker", 0);
}

extern "C" void osPfsRepairId_recomp(uint8_t* rdram, recomp_context* ctx) {
    hh_paklog("osPfsRepairId ra=%08X -> 0", (unsigned)ctx->r31);
    HH_PAK_RET("osPfsRepairId", 0);
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
    hh_paklog("osGbpakInit ra=%08X ch=%d -> NOPAK", (unsigned)ctx->r31, channel);
    HH_PAK_RET("osGbpakInit", GB_PAK_ERR_NOPAK);
}

// -------------------------------------------------------------------------------------------
// Autotest de la API PFS (HH_PAKTEST=0 lo desactiva). Ejercita las MISMAS entradas que usa el
// juego sobre un pak temporal (`hh_paktest.tmp`) y un rdram de mentira, y restaura el estado real:
// no toca `saves/` ni los OSPfs del juego. Sirve para descartar que el problema sea nuestra API.
static void hh_pak_selftest() {
    g_in_paktest = true;
    PakState saved = g_pak;
    g_pak = PakState{};
    g_pak.loaded = true;

    std::vector<uint8_t> fake(0x100000, 0);
    uint8_t* rdram = fake.data();
    recomp_context ctx{};

    const uint32_t SP = 0x80008000u;
    const uint32_t NAME_A = 0x80001000u;
    const uint32_t NAME_B = 0x80001010u;
    const uint32_t STATE = 0x80001020u;
    const uint32_t FILE_NO = 0x80001030u;
    const uint32_t BUF = 0x80002000u;
    const uint32_t TAM = 0x800u;

    std::memcpy(rdram + (NAME_A - 0x80000000u), "HHSV", 4);
    std::memcpy(rdram + (NAME_B - 0x80000000u), "SAVE", 4);
    for (int i = 0; i < 0x100; i++) {
        rdram[(BUF - 0x80000000u) + i] = (uint8_t)i;
    }

    int fallos = 0;
    hh_paklog("[selftest] inicio (pak temporal, no toca saves/)");

    ctx = {};
    ctx.r5 = 0x80003000u; // pfs de mentira
    ctx.r6 = 0;
    osPfsInitPak_recomp(rdram, &ctx);
    fallos += (ctx.r2 != 0);

    ctx = {};
    ctx.r29 = SP;
    ctx.r5 = 0x0A0Au;
    ctx.r6 = 0x48535631u;
    ctx.r7 = NAME_A;
    MEM_W(0x10, SP) = NAME_B;
    MEM_W(0x14, SP) = TAM;
    MEM_W(0x18, SP) = FILE_NO;
    osPfsAllocateFile_recomp(rdram, &ctx);
    fallos += (ctx.r2 != 0);
    uint32_t file_no = MEM_W(FILE_NO, 0);

    ctx = {};
    ctx.r29 = SP;
    ctx.r5 = 0x0A0Au;
    ctx.r6 = 0x48535631u;
    ctx.r7 = NAME_A;
    MEM_W(0x10, SP) = NAME_B;
    MEM_W(0x14, SP) = FILE_NO;
    osPfsFindFile_recomp(rdram, &ctx);
    fallos += (ctx.r2 != 0);
    fallos += (file_no != MEM_W(FILE_NO, 0));

    ctx = {};
    ctx.r29 = SP;
    ctx.r5 = (uint32_t)file_no;
    ctx.r6 = OS_WRITE_FLAG;
    ctx.r7 = 0;
    MEM_W(0x10, SP) = 0x100;
    MEM_W(0x14, SP) = BUF;
    osPfsReadWriteFile_recomp(rdram, &ctx);
    fallos += (ctx.r2 != 0);

    ctx = {};
    ctx.r29 = SP;
    ctx.r5 = (uint32_t)file_no;
    ctx.r6 = OS_READ_FLAG;
    ctx.r7 = 0;
    MEM_W(0x10, SP) = 0x100;
    MEM_W(0x14, SP) = 0x80004000u; // destino de lectura
    osPfsReadWriteFile_recomp(rdram, &ctx);
    fallos += (ctx.r2 != 0);
    fallos += (rdram[(0x80004000u - 0x80000000u) + 7] != 7);

    ctx = {};
    ctx.r5 = (uint32_t)file_no;
    ctx.r6 = STATE;
    osPfsFileState_recomp(rdram, &ctx);
    fallos += (ctx.r2 != 0);
    fallos += (MEM_W(STATE, 0) != TAM);

    ctx = {};
    ctx.r5 = 0x80005000u;
    ctx.r6 = 0x80005004u;
    osPfsNumFiles_recomp(rdram, &ctx);
    fallos += (ctx.r2 != 0);
    fallos += (MEM_W(0x80005004u, 0) != 1);

    ctx = {};
    ctx.r5 = 0x80005008u;
    osPfsFreeBlocks_recomp(rdram, &ctx);
    fallos += (ctx.r2 != 0);

    ctx = {};
    ctx.r29 = SP;
    ctx.r5 = 0x0A0Au;
    ctx.r6 = 0x48535631u;
    ctx.r7 = NAME_A;
    MEM_W(0x10, SP) = NAME_B;
    osPfsDeleteFile_recomp(rdram, &ctx);
    fallos += (ctx.r2 != 0);

    g_pak = std::move(saved);
    g_in_paktest = false;
    std::error_code ec;
    std::filesystem::remove("hh_paktest.tmp", ec);

    hh_paklog("[selftest] fin: %s (%d fallos)", fallos == 0 ? "OK" : "FALLO", fallos);
}
