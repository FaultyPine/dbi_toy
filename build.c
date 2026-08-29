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

static bool require_file(const char *path, const char *message)
{
    if (nob_file_exists(path)) return true;
    nob_log(NOB_ERROR, "%s", message);
    return false;
}

static bool ensure_zydis(void)
{
    if (nob_file_exists(ZYDIS_LIB) && nob_file_exists(ZYCORE_LIB)) return true;

    if (!nob_file_exists(ZYDIS_ROOT "/CMakeLists.txt")) {
        nob_log(NOB_ERROR, "missing Zydis checkout at `%s`", ZYDIS_ROOT);
        nob_log(NOB_ERROR, "run: git submodule update --init --recursive");
        return false;
    }

    nob_log(NOB_INFO, "building Zydis static libraries");

    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd,
                   "cmake",
                   "-S", ZYDIS_ROOT,
                   "-B", ZYDIS_BUILD,
                   "-A", "x64",
                   "-DZYDIS_BUILD_EXAMPLES=OFF",
                   "-DZYDIS_BUILD_TOOLS=OFF",
                   "-DZYDIS_BUILD_TESTS=OFF",
                   "-DZYDIS_BUILD_DOXYGEN=OFF",
                   "-DZYDIS_BUILD_SHARED_LIB=OFF");
    if (!nob_cmd_run(&cmd)) return false;

    nob_cmd_append(&cmd,
                   "cmake",
                   "--build", ZYDIS_BUILD,
                   "--config", ZYDIS_CONFIG,
                   "--target", "Zydis");
    if (!nob_cmd_run(&cmd)) return false;

    if (!nob_file_exists(ZYDIS_LIB)) {
        nob_log(NOB_ERROR, "expected Zydis library was not produced: `%s`", ZYDIS_LIB);
        return false;
    }
    if (!nob_file_exists(ZYCORE_LIB)) {
        nob_log(NOB_ERROR, "expected Zycore library was not produced: `%s`", ZYCORE_LIB);
        return false;
    }

    return true;
}

static bool build_minilua(void)
{
    if (nob_file_exists(MINILUA)) return true;

    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd,
                   CLANG,
                   "external/LuaJIT/src/host/minilua.c",
                   "-o", MINILUA,
                   "-O2",
                   "-D_CRT_SECURE_NO_WARNINGS");
    return nob_cmd_run(&cmd);
}

static bool run_dynasm(void)
{
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd,
                   MINILUA,
                   DYNASM_LUA,
                   //"-L", // --nolineno   defining this means no source code line information emitted. Meant for release builds. 
                   "-I", "external/LuaJIT/dynasm",
                   "-o", DYNASM_INJECTION_C,
                   "injection.c");
    return nob_cmd_run(&cmd);
}

static bool build_main(void)
{
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, CLANG, "main.c", "-o", BUILD_DIR "/main.exe", "-g");
    return nob_cmd_run(&cmd);
}

static bool build_injection(void)
{
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd,
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
    return nob_cmd_run(&cmd);
}

static bool build_tester(void)
{
    Nob_Cmd cmd = {0};
    nob_cmd_append(&cmd, CLANG, "tester.c", "-o", BUILD_DIR "/tester.exe", "-g", "-luser32");
    return nob_cmd_run(&cmd);
}

int main(int argc, char **argv)
{
    NOB_GO_REBUILD_URSELF_PLUS(argc, argv, "external/nob.h");

#ifndef _WIN32
    nob_log(NOB_ERROR, "this project currently builds only on Windows");
    return 1;
#endif

    if (!nob_mkdir_if_not_exists(BUILD_DIR)) return 1;

    if (!require_file(DYNASM_LUA, "missing LuaJIT DynASM checkout, make sure you've pulled in the git submodules")) return 1;
    if (!require_file(MIMALLOC_ROOT "/src/static.c", "missing mimalloc checkout, make sure you've pulled in the git submodules")) return 1;
    if (!build_minilua()) return 1;
    if (!ensure_zydis()) return 1;
    if (!run_dynasm()) return 1;
    if (!build_main()) return 1;
    if (!build_injection()) return 1;
    if (!build_tester()) return 1;

    nob_log(NOB_INFO, "build succeeded");
    return 0;
}
