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
#define MIMALLOC_ROOT "external/mimalloc"

typedef enum { DEBUG, RELEASE } BuildMode;
static BuildMode g_buildMode = DEBUG;

static void append_build_flags(Nob_Cmd *cmd)
{
    if (g_buildMode == RELEASE) nob_cmd_append(cmd, "-O2", "-DNDEBUG");
    else nob_cmd_append(cmd, "-O0", "-g");
}

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

    if (argc == 2 && strcmp(argv[1], "release") == 0)
    {
        g_buildMode = RELEASE;
    }
    else if (argc != 1)
    {
        nob_log(NOB_ERROR, "usage: %s [release]", argv[0]);
        return 1;
    }

    const char *zydisConfig = g_buildMode == RELEASE ? "Release" : "Debug";
    const char *zydisLibDir = nob_temp_sprintf(ZYDIS_BUILD "/%s", zydisConfig);
    const char *zycoreLibDir = nob_temp_sprintf(ZYDIS_BUILD "/zycore/%s", zydisConfig);
    const char *zydisLib = nob_temp_sprintf("%s/Zydis.lib", zydisLibDir);
    const char *zycoreLib = nob_temp_sprintf("%s/Zycore.lib", zycoreLibDir);
    nob_log(NOB_INFO, "build mode: %s", zydisConfig);

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

    bool needsZydisBuild = !nob_file_exists(zydisLib) || !nob_file_exists(zycoreLib);

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
                   "--config", zydisConfig,
                   "--target", "Zydis");

    // minilua
    Nob_Cmd minilua = {0};
    nob_cmd_append(&minilua,
                   CLANG,
                   "external/LuaJIT/src/host/minilua.c",
                   "-o", MINILUA,
                   "-D_CRT_SECURE_NO_WARNINGS");
    append_build_flags(&minilua);

    // dynasm
    Nob_Cmd dynasm = {0};
    nob_cmd_append(&dynasm,
                   MINILUA,
                   DYNASM_LUA,
                   "-I", "external/LuaJIT/dynasm",
                   "-o", DYNASM_INJECTION_C,
                   "injection.c");

    // main control program
    Nob_Cmd mainProgram = {0};
    nob_cmd_append(&mainProgram, CLANG, "main.c", "-o", BUILD_DIR "/main.exe");
    append_build_flags(&mainProgram);

    // injection dll
    Nob_Cmd injection = {0};
    nob_cmd_append(&injection,
                   CLANG,
                   DYNASM_INJECTION_C,
                   "-o", BUILD_DIR "/injection.dll",
                   "-shared",
                   "-I" ZYDIS_ROOT "/dependencies/zycore/include",
                   "-I" ZYDIS_ROOT "/include/Zydis",
                   "-I" ZYDIS_ROOT "/include",
                   "-L", zydisLibDir,
                   "-lZydis",
                   "-L", zycoreLibDir,
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
    append_build_flags(&injection);

    // tester
    Nob_Cmd tester = {0};
    nob_cmd_append(&tester, CLANG, "tester.c", "-o", BUILD_DIR "/tester.exe", "-luser32");
    append_build_flags(&tester);

    if (needsZydisBuild && !nob_cmd_run(&zydisConfigure)) return 1;
    if (needsZydisBuild && !nob_cmd_run(&zydis)) return 1;
    if (!nob_file_exists(zydisLib))
    {
        nob_log(NOB_ERROR, "expected Zydis library was not produced: `%s`", zydisLib);
        return 1;
    }
    if (!nob_file_exists(zycoreLib))
    {
        nob_log(NOB_ERROR, "expected Zycore library was not produced: `%s`", zycoreLib);
        return 1;
    }
    if (!nob_cmd_run(&minilua)) return 1;
    if (!nob_cmd_run(&dynasm)) return 1;
    if (!nob_cmd_run(&mainProgram)) return 1;
    if (!nob_cmd_run(&injection)) return 1;
    if (!nob_cmd_run(&tester)) return 1;

    nob_log(NOB_INFO, "build succeeded");
    return 0;
}
