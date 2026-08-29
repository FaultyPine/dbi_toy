#define NOB_REBUILD_URSELF(binary_path, source_path) "tools\\clang\\bin\\clang.exe", "-o", binary_path, source_path
#define NOB_IMPLEMENTATION
#include "external/nob.h"

#define BUILD_DIR "build"
#define CLANG "tools/clang/bin/clang.exe"
#define MINILUA BUILD_DIR "/minilua.exe"
#define DYNASM_LUA "external/LuaJIT/dynasm/dynasm.lua"
#define DYNASM_INJECTION_C BUILD_DIR "/injection_dynasm.c"
#define ZYDIS_ROOT "external/zydis"
#define ZYDIS_BUILD ZYDIS_ROOT "/build"
#define ZYDIS_CONFIG "RelWithDebInfo"
#define ZYDIS_LIB ZYDIS_BUILD "/" ZYDIS_CONFIG "/Zydis.lib"
#define ZYCORE_LIB ZYDIS_BUILD "/zycore/" ZYDIS_CONFIG "/Zycore.lib"
#define MIMALLOC_ROOT "external/mimalloc"

static bool require_file(const char *path)
{
    if (nob_file_exists(path)) return true;
    nob_log(NOB_ERROR, "missing required file: `%s`", path);
    return false;
}

int main(int argc, char **argv)
{
    NOB_GO_REBUILD_URSELF_PLUS(argc, argv, "external/nob.h");

#ifndef _WIN32
    nob_log(NOB_ERROR, "this project currently builds only on Windows");
    return 1;
#endif

    if (!nob_mkdir_if_not_exists(BUILD_DIR)) return 1;

    bool missingFiles = false;
    missingFiles |= !require_file(ZYDIS_ROOT "/CMakeLists.txt");
    missingFiles |= !require_file(DYNASM_LUA);
    missingFiles |= !require_file(MIMALLOC_ROOT "/src/static.c");
    if (missingFiles)
    {
        nob_log(NOB_ERROR, "missing dependency files; run: git submodule update --init --recursive");
        return 1;
    }

    bool needsZydisBuild = !nob_file_exists(ZYDIS_LIB) || !nob_file_exists(ZYCORE_LIB);
    bool needsMiniluaBuild = !nob_file_exists(MINILUA);

    // zydis configure
    Nob_Cmd zydisConfigure = {0};
    nob_cmd_append(&zydisConfigure,
                   "cmake",
                   "-S", ZYDIS_ROOT,
                   "-B", ZYDIS_BUILD,
                   "-A", "x64",
                   "-DZYDIS_BUILD_EXAMPLES=OFF",
                   "-DZYDIS_BUILD_TOOLS=OFF",
                   "-DZYDIS_BUILD_TESTS=OFF",
                   "-DZYDIS_BUILD_DOXYGEN=OFF",
                   "-DZYDIS_BUILD_SHARED_LIB=OFF");

    // zydis
    Nob_Cmd zydis = {0};
    nob_cmd_append(&zydis,
                   "cmake",
                   "--build", ZYDIS_BUILD,
                   "--config", ZYDIS_CONFIG,
                   "--target", "Zydis");

    // minilua
    Nob_Cmd minilua = {0};
    nob_cmd_append(&minilua,
                   CLANG,
                   "external/LuaJIT/src/host/minilua.c",
                   "-o", MINILUA,
                   "-O2",
                   "-D_CRT_SECURE_NO_WARNINGS");

    // dynasm
    Nob_Cmd dynasm = {0};
    nob_cmd_append(&dynasm,
                   MINILUA,
                   DYNASM_LUA,
                   //"-L", // --nolineno   defining this means no source code line information emitted. Meant for release builds. 
                   "-I", "external/LuaJIT/dynasm",
                   "-o", DYNASM_INJECTION_C,
                   "injection.c");

    // main control program
    Nob_Cmd mainProgram = {0};
    nob_cmd_append(&mainProgram, CLANG, "main.c", "-o", BUILD_DIR "/main.exe", "-g");

    // injection dll
    Nob_Cmd injection = {0};
    nob_cmd_append(&injection,
                   CLANG,
                   DYNASM_INJECTION_C,
                   "-o", BUILD_DIR "/injection.dll",
                   "-g",
                   "-shared",
                   "-I" ZYDIS_ROOT "/dependencies/zycore/include",
                   "-I" ZYDIS_ROOT "/include/Zydis",
                   "-I" ZYDIS_ROOT "/include",
                   "-L" ZYDIS_BUILD "/" ZYDIS_CONFIG,
                   "-lZydis",
                   "-L" ZYDIS_BUILD "/zycore/" ZYDIS_CONFIG,
                   "-lZycore",
                   "-lpsapi",
                   "-lshell32",
                   "-luser32",
                   "-ladvapi32",
                   "-lbcrypt",
                   "-DZYDIS_STATIC_BUILD",
                   "-DZYCORE_STATIC_BUILD",
                   "-I.",
                   "-I" MIMALLOC_ROOT "/include",
                   "-Iexternal/LuaJIT/dynasm");

    // tester
    Nob_Cmd tester = {0};
    nob_cmd_append(&tester, CLANG, "tester.c", "-o", BUILD_DIR "/tester.exe", "-g", "-O2", "-luser32");

    if (needsZydisBuild && !nob_cmd_run(&zydisConfigure)) return 1;
    if (needsZydisBuild && !nob_cmd_run(&zydis)) return 1;
    if (!nob_file_exists(ZYDIS_LIB))
    {
        nob_log(NOB_ERROR, "expected Zydis library was not produced: `%s`", ZYDIS_LIB);
        return 1;
    }
    if (!nob_file_exists(ZYCORE_LIB))
    {
        nob_log(NOB_ERROR, "expected Zycore library was not produced: `%s`", ZYCORE_LIB);
        return 1;
    }
    if (needsMiniluaBuild && !nob_cmd_run(&minilua)) return 1;
    if (!nob_cmd_run(&dynasm)) return 1;
    if (!nob_cmd_run(&mainProgram)) return 1;
    if (!nob_cmd_run(&injection)) return 1;
    if (!nob_cmd_run(&tester)) return 1;

    nob_log(NOB_INFO, "build succeeded");
    return 0;
}
