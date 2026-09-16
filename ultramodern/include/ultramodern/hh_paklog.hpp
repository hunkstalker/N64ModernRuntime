// HH: volcado de diagnóstico del flujo de guardado (Controller Pak / accesorios).
// Ver notes/2026-09-16-guardado-capsula-pak-y-crash-cac-8021d8d0.md
//
// Comportamiento:
//   - Activo POR DEFECTO durante la fase de diagnóstico: escribe en `hh_pak.log` del directorio de
//     trabajo (que run_windows.bat fija a build_win\bin\Release => al lado del exe).
//   - HH_PAKLOG=0        -> desactivado.
//   - HH_PAKLOG=<ruta>   -> fichero explícito (p. ej. una ruta de la carpeta compartida).
//   - HH_PAKTEST=1       -> ejecuta el autotest de la API PFS al primer uso (opt-in).
// Se escribe una línea por evento y se hace flush, para que el final quede aunque el juego crashee.
#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

inline const char* hh_paklog_path() {
    const char* env = std::getenv("HH_PAKLOG");
    if (env == nullptr) {
        return "hh_pak.log";
    }
    if (env[0] == '0' && env[1] == '\0') {
        return nullptr; // desactivado
    }
    if (env[0] == '1' && env[1] == '\0') {
        return "hh_pak.log";
    }
    return env; // ruta explícita
}

inline FILE* hh_paklog_file() {
    static FILE* file = []() -> FILE* {
        const char* path = hh_paklog_path();
        if (path == nullptr) {
            return nullptr;
        }
        // OJO: nada de setvbuf(_IOLBF, tamaño 0): en el CRT de Windows dispara el invalid
        // parameter handler y el proceso muere con 0xC0000409 en la PRIMERA linea (el fichero se
        // creaba vacio). Como hacemos fflush por linea, no hace falta.
        return std::fopen(path, "w");
    }();
    return file;
}

inline void hh_paklog(const char* fmt, ...) {
    FILE* f = hh_paklog_file();
    if (f == nullptr) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fflush(f);
}

// Opt-in: solo con HH_PAKTEST=1 (evita riesgos de arranque; el autotest ya se validó en Linux).
inline bool hh_paktest_enabled() {
    const char* env = std::getenv("HH_PAKTEST");
    return env != nullptr && env[0] == '1' && env[1] == '\0';
}
