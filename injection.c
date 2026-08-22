
// this is a shared lib that is injected into the remote process and is in charge of instrumenting the process it is injected into

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <TlHelp32.h>
#include "Zydis.h"

#define DASM_FDEF static
#include "dasm_proto.h"
#include "dasm_x86.h"

|.arch x64
|.actionlist peony_dynasm_actions
|.section code

#define STB_DS_IMPLEMENTATION
#include "external/std_ds.h"
#include "shared_defines.h"

#define PEONY_SHARED_COMMS_IMPLEMENTATION
#include "shared_comms.h"
#undef PEONY_SHARED_COMMS_IMPLEMENTATION

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "user32.lib")

typedef struct
{
    DWORD64 originalR10;
    CONTEXT savedRegs;
} ThreadHijackState;

#define SAVEDREGS_OFF_NUM 16
#define SAVEDREGS_OFF STRINGIFY_MACRO(SAVEDREGS_OFF_NUM)
#define ORIGR10_OFF_NUM 0
#define ORIGR10_OFF STRINGIFY_MACRO(ORIGR10_OFF_NUM)
_Static_assert(offsetof(ThreadHijackState, originalR10) == ORIGR10_OFF_NUM, "unexpected ThreadHijackState layout");
_Static_assert(offsetof(ThreadHijackState, savedRegs) == SAVEDREGS_OFF_NUM, "unexpected ThreadHijackState layout");

typedef struct 
{
    char moduleName[MAX_MODULE_NAME32 + 1];
    char exePath[MAX_PATH];
    DWORD_PTR moduleBaseAddress;
    DWORD moduleSize;
} ModuleInfo;

typedef struct 
{
    // arbitrary limit...
    #define THREAD_NAME_MAX_LEN MAX_PATH
    char threadName[THREAD_NAME_MAX_LEN];
    DWORD threadId;
} ThreadInfo;


typedef struct
{
    uintptr_t key; // app pc
    uint8_t* value; // code cache pc
} CodeCacheEntry;

// code cache location that can & will be backpatched
typedef struct
{
    uint8_t* codeCachePc;
} ExitPatchSite;

// labels placed at the exits of code cache blocks for later backpatching
typedef struct
{
    uintptr_t targetAppPC;
    int label;
} ExitPatchLabel;

typedef struct
{
    uintptr_t key; // target app pc
    ExitPatchSite* value; // arr of exit sites to patch when this target is compiled
} PendingExitPatchEntry;

typedef struct
{
    uint8_t* base;
    size_t capacity;
    size_t used;
    CodeCacheEntry* entries; // hm: app PC -> code-cache PC
} CodeCache;

static CodeCache g_codeCache;
static PendingExitPatchEntry* g_pendingExitPatches;

typedef struct
{
    uint8_t* cursor;
} CodeCursor;

#define DBI_CODE_CACHE_SIZE (50 * MB)
#define DBI_LOG_COMPILATION_VERBOSE 1

static ThreadHijackState g_hijackedThreadState;
static SharedLogObject* g_sharedLog;

// scratch for holding the next PC we should go to
|.define DBI_DISPATCH_REG, r11
// holds our global state, usually g_hijackedThreadState or something inside it
|.define DBI_STATE_REG, r10

// Stay in signed rel32 +/- 2 GB range when searching for nearby code-cache memory
static const uintptr_t DBI_CODE_CACHE_NEAR_SEARCH_RADIUS = 0x70000000ULL;
#define DBI_EXIT_TRAMPOLINE_INDICATE_DISPATCH_REG_HAS_TARGET_PC -1

void Initialize();
DWORD WINAPI InitializeThread(LPVOID param);
void DBIExitTrampoline(void);


BOOL WINAPI DllMain(
    HINSTANCE hinstDLL,
    DWORD fdwReason,
    LPVOID lpReserved)
{
    switch(fdwReason) 
    { 
        case DLL_PROCESS_ATTACH:
        {
            HANDLE initThread = CreateThread(NULL, 0, InitializeThread, NULL, 0, NULL);
            if (initThread)
            {
                CloseHandle(initThread);
            }
        } break;
        case DLL_THREAD_ATTACH:
        {
        } break;
        case DLL_THREAD_DETACH:
        {
        } break;
        case DLL_PROCESS_DETACH:
        {
        } break;
    }
    return TRUE;
}


static uintptr_t AlignDownToPowerOfTwo(uintptr_t value, uintptr_t alignment)
{
    return value & ~(alignment - 1);
}

// "fromNextRip" because x64 relative instructions are calculated from the next instruction after the relative one, not from the instruction start
static bool IsRel32Reachable(uintptr_t fromNextRip, uintptr_t target)
{
    int64_t delta = (int64_t)(target - fromNextRip);
    return delta >= INT32_MIN && delta <= INT32_MAX;
}

static bool X64EmitBytes(uint8_t** out, const void* bytes, size_t length)
{
    memcpy(*out, bytes, length);
    *out += length;
    return true;
}

void PeonyLogWrite(const char* bytes, int length)
{
    if (length <= 0)
    {
        return;
    }
    if (!g_sharedLog)
    {
        g_sharedLog = SharedLogInitialize();
        if (!g_sharedLog)
        {
            return;
        }
    }

    if (length >= PEONY_LOG_BUFFER_SIZE)
    {
        bytes += length - (PEONY_LOG_BUFFER_SIZE - 1);
        length = PEONY_LOG_BUFFER_SIZE - 1;
    }

    LONG readOffset = g_sharedLog->readOffset;
    LONG writeOffset = g_sharedLog->writeOffset;
    if (readOffset < 0 || readOffset >= PEONY_LOG_BUFFER_SIZE ||
        writeOffset < 0 || writeOffset >= PEONY_LOG_BUFFER_SIZE)
    {
        InterlockedExchange(&g_sharedLog->readOffset, 0);
        InterlockedExchange(&g_sharedLog->writeOffset, 0);
        readOffset = 0;
        writeOffset = 0;
    }

    int used = (writeOffset >= readOffset)
        ? (writeOffset - readOffset)
        : (PEONY_LOG_BUFFER_SIZE - readOffset + writeOffset);
    int freeBytes = PEONY_LOG_BUFFER_SIZE - used - 1;
    if (length > freeBytes)
    {
        InterlockedAdd(&g_sharedLog->droppedBytes, length);
        return;
    }

    int firstCopy = PEONY_LOG_BUFFER_SIZE - writeOffset;
    if (firstCopy > length)
    {
        firstCopy = length;
    }
    memcpy(g_sharedLog->buffer + writeOffset, bytes, firstCopy);
    if (firstCopy < length)
    {
        memcpy(g_sharedLog->buffer, bytes + firstCopy, length - firstCopy);
    }

    MemoryBarrier();
    InterlockedExchange(&g_sharedLog->writeOffset, (writeOffset + length) % PEONY_LOG_BUFFER_SIZE);
}

void PeonyLogf(const char* format, ...)
{
    char line[1024];
    int prefixLength = snprintf(line, sizeof(line), "[injection:%lu] ", GetCurrentThreadId());
    if (prefixLength < 0)
    {
        return;
    }
    if (prefixLength >= (int)sizeof(line))
    {
        prefixLength = sizeof(line) - 1;
    }

    va_list args;
    va_start(args, format);
    int bodyLength = vsnprintf(line + prefixLength, sizeof(line) - prefixLength, format, args);
    va_end(args);

    int totalLength = prefixLength;
    if (bodyLength > 0)
    {
        int spaceLeft = (int)sizeof(line) - prefixLength;
        totalLength += (bodyLength < spaceLeft) ? bodyLength : spaceLeft - 1;
    }
    if (totalLength < (int)sizeof(line) - 1 && (totalLength == 0 || line[totalLength - 1] != '\n'))
    {
        line[totalLength++] = '\n';
        line[totalLength] = 0;
    }
    PeonyLogWrite(line, totalLength);
}


static void LogDecodedInstructionRange(
    const char* title,
    ZydisDecoder* decoder,
    ZydisFormatter* formatter,
    uintptr_t runtimeStart,
    size_t byteLength)
{
    PeonyLogf("%s [%p, %p) %llu bytes", title, (void*)runtimeStart, (void*)(runtimeStart + byteLength), (DWORD64)byteLength);

    uintptr_t runtimePc = runtimeStart;
    const uint8_t* bytes = (const uint8_t*)runtimeStart;
    size_t remaining = byteLength;
    while (remaining > 0)
    {
        ZydisDecodedInstruction instr;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        ZyanStatus status = ZydisDecoderDecodeFull(decoder, bytes, remaining, &instr, operands);
        if (ZYAN_FAILED(status))
        {
            PeonyLogf("  %p  <decode failed: %lu>", (void*)runtimePc, status);
            return;
        }

        char instrBuf[256] = {0};
        if (ZYAN_FAILED(ZydisFormatterFormatInstruction(
                formatter,
                &instr,
                operands,
                instr.operand_count_visible,
                instrBuf,
                sizeof(instrBuf),
                runtimePc,
                NULL)))
        {
            PeonyLogf("  %p <format failed>", (void*)runtimePc);
            return;
        }

        PeonyLogf("%s", instrBuf);

        runtimePc += instr.length;
        bytes += instr.length;
        remaining -= instr.length;
    }
}

static void LogCompiledBasicBlockComparison(
    ZydisDecoder* decoder,
    ZydisFormatter* formatter,
    uintptr_t appStart,
    uintptr_t appEnd,
    uint8_t* cacheStart,
    uint8_t* cacheEnd)
{
    PeonyLogf(
        "\n--------------------\nBasic block compile comparison app [%p, %p) -> cache [%p, %p)",
        (void*)appStart,
        (void*)appEnd,
        cacheStart,
        cacheEnd);
    LogDecodedInstructionRange(
        "Original app instructions",
        decoder,
        formatter,
        appStart,
        (size_t)(appEnd - appStart));
    LogDecodedInstructionRange(
        "Emitted code cache instructions",
        decoder,
        formatter,
        (uintptr_t)cacheStart,
        (size_t)(cacheEnd - cacheStart));
    PeonyLogf("\n--------------------\n");
}

DWORD WINAPI InitializeThread(LPVOID param)
{
    (void)param;
    Initialize();
    return 0;
}

ModuleInfo* ListProcessModules(DWORD pid)
{
    ModuleInfo* result = NULL;
    HANDLE moduleSnapHdl = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (moduleSnapHdl == INVALID_HANDLE_VALUE) 
    {
        PeonyLogf("Failed to take module snapshot. Error: %lu", GetLastError());
        return result;
    }
    MODULEENTRY32 me32;
    me32.dwSize = sizeof(MODULEENTRY32);
    if (!Module32First(moduleSnapHdl, &me32)) 
    {
        PeonyLogf("Failed to retrieve the first module.");
        CloseHandle(moduleSnapHdl);
        return result;
    }
    do {
        ModuleInfo mi = {0};
        char* moduleName = me32.szModule;
        strcpy_s(mi.moduleName, sizeof(mi.moduleName), moduleName);
        char* exePath = me32.szExePath;
        strcpy_s(mi.exePath, sizeof(mi.exePath), exePath);
        DWORD_PTR moduleBaseAddress = (DWORD_PTR)me32.modBaseAddr;
        mi.moduleBaseAddress = moduleBaseAddress;
        DWORD moduleSize = me32.modBaseSize;
        mi.moduleSize = moduleSize;
        arrput(result, mi);
    } while (Module32Next(moduleSnapHdl, &me32));
    CloseHandle(moduleSnapHdl);
    return result;
}


ThreadInfo* ListProcessThreads(DWORD targetPid) 
{
    ThreadInfo* result = NULL;
    HANDLE threadSnapshotHdl = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (threadSnapshotHdl == INVALID_HANDLE_VALUE) 
    {
        PeonyLogf("Failed to create snapshot.");
        return result;
    }
    THREADENTRY32 te;
    te.dwSize = sizeof(THREADENTRY32);
    if (Thread32First(threadSnapshotHdl, &te)) 
    {
        do 
        {
            // only threads that belong to target process, since the snapshot has *all* threads...
            if (te.th32OwnerProcessID == targetPid) 
            {
                ThreadInfo ti = {0};
                ti.threadId = te.th32ThreadID;
                strcpy_s(ti.threadName, sizeof(ti.threadName), "[Unnamed Thread]");

                HANDLE hThread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
                if (hThread != NULL) 
                {
                    PWSTR threadName = NULL;
                    HRESULT hr = GetThreadDescription(hThread, &threadName);
                    if (SUCCEEDED(hr) && threadName != NULL && wcslen(threadName) > 0) 
                    {
                        int converted = WideCharToMultiByte(
                            CP_UTF8,
                            0,
                            threadName,
                            -1,
                            ti.threadName,
                            sizeof(ti.threadName),
                            NULL,
                            NULL);
                        if (!converted)
                        {
                            strcpy_s(ti.threadName, sizeof(ti.threadName), "[Name Conversion Failed]");
                        }
                    }
                    if (threadName) 
                    {
                        LocalFree(threadName);
                    }
                    CloseHandle(hThread);
                } 
                else 
                {
                    strcpy_s(ti.threadName, sizeof(ti.threadName), "[Access Denied]");
                }
                arrput(result, ti);
            }
        } while (Thread32Next(threadSnapshotHdl, &te));
    }
    CloseHandle(threadSnapshotHdl);
    return result;
}

bool ShouldWeCareAboutThisModule(SharedCommsObject* sharedComms, const ModuleInfo* moduleInfo)
{
    // TODO: filtering
    return true;
}

static bool IsBasicBlockTerminator(const ZydisDecodedInstruction* instr)
{
    switch (instr->meta.category)
    {
        case ZYDIS_CATEGORY_COND_BR:
        case ZYDIS_CATEGORY_UNCOND_BR:
        case ZYDIS_CATEGORY_CALL:
        case ZYDIS_CATEGORY_RET:
        case ZYDIS_CATEGORY_INTERRUPT:
        case ZYDIS_CATEGORY_SYSCALL:
        case ZYDIS_CATEGORY_SYSRET:
            return true;

        default:
            break;
    }

    switch (instr->mnemonic)
    {
        case ZYDIS_MNEMONIC_IRET:
        case ZYDIS_MNEMONIC_IRETD:
        case ZYDIS_MNEMONIC_IRETQ:
        case ZYDIS_MNEMONIC_UIRET:
            return true;

        default:
            return false;
    }
}

uint8_t* CodeCacheLookup(uint64_t appPc)
{
    if (!g_codeCache.entries || appPc == DBI_EXIT_TRAMPOLINE_INDICATE_DISPATCH_REG_HAS_TARGET_PC)
    {
        return NULL;
    }
    CodeCacheEntry* entry = hmgetp_null(g_codeCache.entries, appPc);
    return entry ? entry->value : NULL;
}

static void DbiDynasmInit(dasm_State** Dst)
{
    dasm_init(Dst, DASM_MAXSECTION);
    dasm_setupglobal(Dst, NULL, 0);
    dasm_setup(Dst, peony_dynasm_actions);
}

static bool DbiDynasmEncode(dasm_State** Dst, uint8_t* out, size_t* outSize)
{
    size_t size = 0;
    int status = dasm_link(Dst, &size);
    if (status != 0)
    {
        PeonyLogf("DynASM link failed with status %d", status);
        return false;
    }

    status = dasm_encode(Dst, out);
    if (status != 0)
    {
        PeonyLogf("DynASM encode failed with status %d", status);
        return false;
    }

    *outSize = size;
    return true;
}

static void CodeCacheAddPendingPatch(uintptr_t targetAppPC, uint8_t* patchSite);

static bool DbiDynasmEncodeSnippet(dasm_State** Dst, CodeCursor* cursor, ExitPatchLabel* patchLabels)
{
    uint8_t* base = cursor->cursor;
    size_t size = 0;
    if (!DbiDynasmEncode(Dst, base, &size))
    {
        return false;
    }

    for (int i = 0; i < arrlen(patchLabels); i++)
    {
        int offset = dasm_getpclabel(Dst, patchLabels[i].label);
        if (offset < 0)
        {
            PeonyLogf("DynASM patch label %d was not resolved", patchLabels[i].label);
            return false;
        }
        CodeCacheAddPendingPatch(patchLabels[i].targetAppPC, base + offset);
    }

    cursor->cursor += size;
    return true;
}

static bool DbiPatchExitToDirectJump(uint8_t* patchSite, uint8_t* targetCodeCachePC)
{
    dasm_State* D = NULL;
    dasm_State** Dst = &D;
    DbiDynasmInit(Dst);

    | jmp &targetCodeCachePC

    size_t size = 0;
    int status = dasm_link(Dst, &size);
    if (status != 0)
    {
        PeonyLogf("DynASM link failed while patching exit: %d", status);
        dasm_free(Dst);
        return false;
    }

    if (size > 5)
    {
        PeonyLogf("Direct exit patch was unexpectedly %llu bytes", (unsigned long long)size);
        dasm_free(Dst);
        return false;
    }

    status = dasm_encode(Dst, patchSite);
    dasm_free(Dst);
    if (status != 0)
    {
        PeonyLogf("DynASM encode failed while patching exit: %d", status);
        return false;
    }

    FlushInstructionCache(GetCurrentProcess(), patchSite, size);
    return true;
}

static void CodeCacheAddPendingPatch(uintptr_t targetAppPC, uint8_t* patchSite)
{
    PendingExitPatchEntry* entry = g_pendingExitPatches
        ? hmgetp_null(g_pendingExitPatches, targetAppPC)
        : NULL;

    ExitPatchSite site = {.codeCachePc = patchSite};
    if (entry)
    {
        arrput(entry->value, site);
        return;
    }

    ExitPatchSite* sites = NULL;
    arrput(sites, site);
    hmput(g_pendingExitPatches, targetAppPC, sites);
}

static void CodeCachePatchPendingExits(uintptr_t blockStartPC, uint8_t* targetCodeCachePC)
{
    if (!g_pendingExitPatches)
    {
        return;
    }

    PendingExitPatchEntry* entry = hmgetp_null(g_pendingExitPatches, blockStartPC);
    if (!entry)
    {
        return;
    }

    ExitPatchSite* sites = entry->value;
    for (int i = 0; i < arrlen(sites); i++)
    {
        if (!DbiPatchExitToDirectJump(sites[i].codeCachePc, targetCodeCachePC))
        {
            PeonyLogf("Failed to patch exit at %p to %p", sites[i].codeCachePc, targetCodeCachePC);
            continue;
        }
    }

    arrfree(sites);
    hmdel(g_pendingExitPatches, blockStartPC);
}

static void CodeCachePublishBlock(uintptr_t appPc, uint8_t* blockStart)
{
    hmput(g_codeCache.entries, appPc, blockStart);
    CodeCachePatchPendingExits(appPc, blockStart);
}

// tries to initialize code cache memory within rel32 range of 'nearPc'
// if we can get our code cache within rel32 range, we can relocate rip-relative stuff much easier/more consistently
// if we can't get it nearby, we still initialize, but some rip-relative stuff might be more complicated to properly jit
// and for this project idk if i'll even bother with that situation
bool CodeCacheInit(uintptr_t nearPc)
{
    if (g_codeCache.base)
    {
        return true;
    }
    
    // here we start at nearPc and fan out in both directions
    // up and down in memory (+ and -)
    // trying to find a contiguous DBI_CODE_CACHE_SIZE block of available memory we can allocate for our code cache

    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    uintptr_t granularity = sysInfo.dwAllocationGranularity;
    uintptr_t anchor = AlignDownToPowerOfTwo(nearPc, granularity);

    for (uintptr_t distance = 0; distance < DBI_CODE_CACHE_NEAR_SEARCH_RADIUS; distance += granularity)
    {
        uintptr_t candidates[2] = {anchor + distance, anchor - distance};
        for (int i = 0; i < 2; i++)
        {
            if (candidates[i] < granularity)
            {
                continue;
            }

            uint8_t* memory = (uint8_t*)VirtualAlloc(
                (void*)candidates[i],
                DBI_CODE_CACHE_SIZE,
                MEM_RESERVE | MEM_COMMIT,
                PAGE_EXECUTE_READWRITE);
            if (memory && IsRel32Reachable((uintptr_t)memory, nearPc))
            {
                g_codeCache.base = memory;
                g_codeCache.capacity = DBI_CODE_CACHE_SIZE;
                g_codeCache.used = 0;
                PeonyLogf("Code cache allocated at %p near %p", memory, (void*)nearPc);
                return true;
            }
            if (memory)
            {
                VirtualFree(memory, 0, MEM_RELEASE);
            }
        }
    }

    g_codeCache.base = (uint8_t*)VirtualAlloc(
        NULL,
        DBI_CODE_CACHE_SIZE,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_EXECUTE_READWRITE);
    if (!g_codeCache.base)
    {
        PeonyLogf("Failed to allocate code cache: %lu", GetLastError());
        return false;
    }
    g_codeCache.capacity = DBI_CODE_CACHE_SIZE;
    g_codeCache.used = 0;
    PeonyLogf("Code cache allocated at %p, but not necessarily near %p", g_codeCache.base, (void*)nearPc);
    return true;
}

uint8_t* CodeCacheReserve(size_t bytes)
{
    if (g_codeCache.used + bytes > g_codeCache.capacity)
    {
        PeonyLogf("Code cache is full!\n");
        return NULL;
    }
    uint8_t* result = g_codeCache.base + g_codeCache.used;
    g_codeCache.used += bytes;
    return result;
}


void OnCompileBasicBlock(CodeCursor* pOut)
{
    
}

// are we looking at a direct (immediate) relative instruction
bool GetDirectRelativeTarget(
    const ZydisDecodedInstruction* instr,
    const ZydisDecodedOperand* operands,
    uintptr_t instructionAddress,
    uintptr_t* outTarget)
{
    for (int i = 0; i < instr->operand_count_visible; i++)
    {
        if (operands[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operands[i].imm.is_relative)
        {
            ZyanU64 target = 0;
            if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(instr, &operands[i], instructionAddress, &target)))
            {
                *outTarget = (uintptr_t)target;
                return true;
            }
        }
    }
    return false;
}

bool IsRipRelativeMemoryOp(ZydisDecodedOperand* op)
{
    return op->type == ZYDIS_OPERAND_TYPE_MEMORY && (op->mem.base == ZYDIS_REGISTER_RIP || op->mem.base == ZYDIS_REGISTER_EIP);
}

bool HasRipRelativeMemoryOp(ZydisDecodedInstruction* instr, ZydisDecodedOperand* operands)
{
    for (int i = 0; i < instr->operand_count_visible; i++)
    {
        ZydisDecodedOperand* operand = &operands[i];
        if (IsRipRelativeMemoryOp(operand))
        {
            return true;
        }
    }
    return false;
}

bool EmitAndPossiblyRelocateInstruction(CodeCursor* cursor, uintptr_t appPc, ZydisDecodedInstruction* instr, ZydisDecodedOperand* operands)
{
    // if it's not a rip-relative instruction, we can just emit the original one exactly as it was
    bool isRipRelativeMemoryOperand = HasRipRelativeMemoryOp(instr, operands);
    if (!isRipRelativeMemoryOperand)
    {
        return X64EmitBytes(&cursor->cursor, (void*)appPc, instr->length);
    }

    // preparing to emit relocated instruction
    ZydisEncoderRequest req;
    if (ZYAN_FAILED(ZydisEncoderDecodedInstructionToEncoderRequest(
            instr,
            operands,
            instr->operand_count_visible,
            &req)))
    {
        PeonyLogf("Failed to create encoder request for RIP-relative instruction at %p", (void*)appPc);
        return false;
    }

    // rip relative relocation
    // example original instruction
    // mov rax, qword ptr [rip + disp32]
    // disp32 is relative to the next instruction addr, so the real memory addr we're moving into rax is
    // absoluteTarget = original_next_rip + original_disp32;
    // so the new displacement is
    // new_disp32 = absoluteTarget - cache_next_rip;

    uint8_t* codeCachePc = cursor->cursor;
    for (int i = 0; i < instr->operand_count_visible; i++)
    {
        ZydisDecodedOperand* operand = &operands[i];
        if (!IsRipRelativeMemoryOp(operand))
        {
            continue;
        }
        // code cache rip to relocate to
        // x64 rip-relative memory operands always are relative to the *next* address, hence the `+ instr->length`
        uintptr_t newCodeCacheNextRip = (uintptr_t)codeCachePc + instr->length;

        #if DBI_LOG_COMPILATION_VERBOSE
        PeonyLogf("Relocating rip-relative mem op %p -> %p", appPc, newCodeCacheNextRip);
        #endif

        ZyanU64 absoluteTarget = 0;
        if (ZYAN_FAILED(ZydisCalcAbsoluteAddress(instr, operand, appPc, &absoluteTarget)))
        {
            PeonyLogf("Failed to resolve RIP-relative target at %p", (void*)appPc);
            return false;
        }

        // from the code cache rip, can i still address the original memory target via rel32?
        if (!IsRel32Reachable(newCodeCacheNextRip, (uintptr_t)absoluteTarget))
        {
            PeonyLogf("Relocated RIP-relative target is not within rel32 range! What the heck do we do???\n");
            assert(false && "If we hit this, the program would crash/break. There's probably a way to handle this case with some extra instrumentation");
            return false;
        }
        // rel32 is enough to address the original memory, so we can just do the simple math and reassign the rip-relative part of the operand
        req.operands[i].mem.displacement = (int64_t)((uintptr_t)absoluteTarget - newCodeCacheNextRip);
    }

    // emit relocated instruction
    ZyanUSize encodedLength = ZYDIS_MAX_INSTRUCTION_LENGTH;
    if (ZYAN_FAILED(ZydisEncoderEncodeInstruction(&req, cursor->cursor, &encodedLength)))
    {
        PeonyLogf("Failed to encode relocated instruction at %p", (void*)appPc);
        return false;
    }
    cursor->cursor += encodedLength;

    return true;
}

static void DbiEmitExitTrampoline(dasm_State** Dst, uintptr_t targetAppPC)
{
    bool dispatchRegHasTargetPC = targetAppPC == DBI_EXIT_TRAMPOLINE_INDICATE_DISPATCH_REG_HAS_TARGET_PC;

    | push DBI_STATE_REG
    if (!dispatchRegHasTargetPC)
    {
        | push DBI_DISPATCH_REG
    }

    | mov64 DBI_STATE_REG, (uintptr_t)&g_hijackedThreadState.savedRegs.Rip
    if (!dispatchRegHasTargetPC)
    {
        | mov64 DBI_DISPATCH_REG, targetAppPC
    }

    | mov qword [DBI_STATE_REG], DBI_DISPATCH_REG
    
    if (!dispatchRegHasTargetPC)
    {
        | pop DBI_DISPATCH_REG
    }
    
    | pop DBI_STATE_REG
    
    // NOTE: even if the dispatch reg had the target pc coming into this function, we still pop it because we assume the caller had
    // saved the original dispatch reg content on the stack
    if (dispatchRegHasTargetPC)
    {
        | pop DBI_DISPATCH_REG
    }

    // jmp qword ptr [rip + 0]    <- this is an absolute indirect jump using a 64bit pointer embedded right after this jmp instruction
    | jmp qword [>9]
    |9:
    |.quad (uintptr_t)DBIExitTrampoline
}

// emits a dbi exit trampoline, but also appends to a list of exit labels so we can later backpatch this
static bool DbiEmitPatchableExit(dasm_State** Dst, uintptr_t targetAppPC, ExitPatchLabel** patchLabels)
{
    uint8_t* targetCodeCachePC = CodeCacheLookup(targetAppPC);
    if (targetCodeCachePC)
    {
        | jmp &targetCodeCachePC
        return true;
    }

    int label = arrlen(*patchLabels);
    dasm_growpc(Dst, label + 1);

    ExitPatchLabel patchLabel = {.targetAppPC = targetAppPC, .label = label};
    arrput(*patchLabels, patchLabel);

    |=>label:
    // the bytes here (emitted in the below function) are what gets backpatched
    DbiEmitExitTrampoline(Dst, targetAppPC);
    return true;
}

static void DbiEmitPush(dasm_State** Dst, uintptr_t returnAppPC)
{
    | push DBI_DISPATCH_REG
    | mov64 DBI_DISPATCH_REG, returnAppPC
    | xchg qword [rsp], DBI_DISPATCH_REG
}

static bool DbiEmitJccToLabel1(dasm_State** Dst, ZydisMnemonic mnemonic)
{
    switch (mnemonic)
    {
        case ZYDIS_MNEMONIC_JO:
        | jo >1
        return true;
        case ZYDIS_MNEMONIC_JNO:
        | jno >1
        return true;
        case ZYDIS_MNEMONIC_JB:
        | jb >1
        return true;
        case ZYDIS_MNEMONIC_JNB:
        | jnb >1
        return true;
        case ZYDIS_MNEMONIC_JZ:
        | jz >1
        return true;
        case ZYDIS_MNEMONIC_JNZ:
        | jnz >1
        return true;
        case ZYDIS_MNEMONIC_JBE:
        | jbe >1
        return true;
        case ZYDIS_MNEMONIC_JNBE:
        | jnbe >1
        return true;
        case ZYDIS_MNEMONIC_JS:
        | js >1
        return true;
        case ZYDIS_MNEMONIC_JNS:
        | jns >1
        return true;
        case ZYDIS_MNEMONIC_JP:
        | jp >1
        return true;
        case ZYDIS_MNEMONIC_JNP:
        | jnp >1
        return true;
        case ZYDIS_MNEMONIC_JL:
        | jl >1
        return true;
        case ZYDIS_MNEMONIC_JNL:
        | jnl >1
        return true;
        case ZYDIS_MNEMONIC_JLE:
        | jle >1
        return true;
        case ZYDIS_MNEMONIC_JNLE:
        | jnle >1
        return true;
        default:
            return false;
    }
}

bool CompileBlockTerminator(
    CodeCursor* cursor, 
    uintptr_t currentPC, 
    ZydisDecodedInstruction* instr, 
    ZydisDecodedOperand* operands,
    dasm_State** Dst,
    ExitPatchLabel** patchLabels)
{
    // the PC after this terminator instruction
    // also used as the "fallthrough" PC when we decide a branch shouldn't be taken
    uintptr_t nextSeqAppPC = currentPC + instr->length;
    
    switch (instr->meta.category)
    {
        // BOOKMARK: still need to support memory targets, rip-relative memory targets, register targets
        //      for both uncond br and call
        case ZYDIS_CATEGORY_CALL:
        {
            uintptr_t targetAddress = 0;
            if (!GetDirectRelativeTarget(instr, operands, currentPC, &targetAddress))
            {
                PeonyLogf("Unsupported non-relative conditional branch at %p", currentPC);
                return false;
            }
            // we push the "return" address on the stack.
            // we will be returning to this current (really, next) pc
            // we dont want to clobber regs here, so we use this xchg trick on the stack 
            // to make sure DBI_DISPATCH_REG is the same as before, and top of the stack has the return address (nextSeqAppPC)
            DbiEmitPush(Dst, nextSeqAppPC);
            if (!DbiEmitPatchableExit(Dst, targetAddress, patchLabels))
            {
                return false;
            }
            return DbiDynasmEncodeSnippet(Dst, cursor, *patchLabels);
        } break;
        case ZYDIS_CATEGORY_UNCOND_BR:
        {
            assert(instr->meta.branch_type != ZYDIS_BRANCH_TYPE_FAR); // NYI

            uintptr_t targetAppPC = nextSeqAppPC;
            // immediate jmp EX: jmp 0xDEADBEEF
            if (instr->operand_count_visible > 0 && operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
            {
                ZyanU64 absoluteTarget = 0;
                if (ZYAN_FAILED(ZydisCalcAbsoluteAddress(instr, &operands[0], currentPC, &absoluteTarget)))
                {
                    PeonyLogf("Failed to resolve direct branch target at %p", (void*)currentPC);
                    return false;
                }
                targetAppPC = (uintptr_t)absoluteTarget;
            }
            // static memory indirect jmp EX: "jmp [0x00007FF6327770A8]"
            else if (instr->operand_count_visible > 0 && operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY)
            {
                ZyanU64 jmpMemAddress = 0; // ^  this is the absolute address of the memory that contains the address we should jump to
                if (ZYAN_FAILED(ZydisCalcAbsoluteAddress(instr, &operands[0], currentPC, &jmpMemAddress)))
                {
                    PeonyLogf("Failed to resolve direct branch target at %p", (void*)currentPC);
                    return false;
                }
                PeonyLogf("memory uncondbr jmpMemAddress = %p  nextSeqAppPC = %p currentPC = %p", jmpMemAddress, nextSeqAppPC, currentPC);
                // the emitexit call will always pop dispatch_reg back
                | push DBI_DISPATCH_REG
                | mov64 DBI_DISPATCH_REG, jmpMemAddress
                | mov DBI_DISPATCH_REG, qword [DBI_DISPATCH_REG]
                // TODO: this is not currently backpatchable, but getting indirect jumps to be backpatchable is complicated.
                DbiEmitExitTrampoline(Dst, DBI_EXIT_TRAMPOLINE_INDICATE_DISPATCH_REG_HAS_TARGET_PC);
                return DbiDynasmEncodeSnippet(Dst, cursor, *patchLabels);
            }
            else
            {
                PeonyLogf("Unsupported indirect unconditional branch at %p", (void*)currentPC);
                return false;
            }
            if (!DbiEmitPatchableExit(Dst, targetAppPC, patchLabels))
            {
                return false;
            }
            return DbiDynasmEncodeSnippet(Dst, cursor, *patchLabels);
        } break;
        case ZYDIS_CATEGORY_COND_BR:
        {
            assert(instr->meta.branch_type != ZYDIS_BRANCH_TYPE_FAR);
            // for conditional branches in x64 they're, always rel32 or rel8.
            // at this point, the progrma already has done the comparison, so rflags is ready.
            // we need to emit the original branch jcc code, and make sure the branch target address
            // hits the DBI again with the correct target pc
            uintptr_t takenAddress = 0;
            if (!GetDirectRelativeTarget(instr, operands, currentPC, &takenAddress))
            {
                PeonyLogf("Unsupported non-relative conditional branch at %p", currentPC);
                return false;
            }

            if (!DbiEmitJccToLabel1(Dst, instr->mnemonic))
            {
                PeonyLogf("Unsupported conditional branch Jcc code at %p", currentPC);
                return false;
            }
            if (!DbiEmitPatchableExit(Dst, nextSeqAppPC, patchLabels))
            {
                return false;
            }
            |1:
            if (!DbiEmitPatchableExit(Dst, takenAddress, patchLabels))
            {
                return false;
            }
            return DbiDynasmEncodeSnippet(Dst, cursor, *patchLabels);
        } break;
        case ZYDIS_CATEGORY_RET:
        {
            // save the original dispatch reg on the stack, the emitexit function will always pop this back into dispatch_reg
            | xchg qword [rsp], DBI_DISPATCH_REG
            DbiEmitExitTrampoline(Dst, DBI_EXIT_TRAMPOLINE_INDICATE_DISPATCH_REG_HAS_TARGET_PC);
            return DbiDynasmEncodeSnippet(Dst, cursor, *patchLabels);
        } break;
        case ZYDIS_CATEGORY_SYSCALL:
        {
            if (!EmitAndPossiblyRelocateInstruction(cursor, currentPC, instr, operands))
            {
                PeonyLogf("Something went wrong emitting instructions for syscall/sysret");
                return false;
            }
            if (!DbiEmitPatchableExit(Dst, nextSeqAppPC, patchLabels))
            {
                return false;
            }
            return DbiDynasmEncodeSnippet(Dst, cursor, *patchLabels);
        } break;
        default:
        {
            PeonyLogf("Unsupported terminator category %u at %p. ", instr->meta.category, currentPC);
            return false;
        } break;
    }
    return false;
}

// TODO: interesting idea: what if we removed the execute permissions on the pages of memory for modules we care about
// and install an exception handler to catch when any thread tries to execute that code. 
// Then we dbilookuporcompile, restore exe permissions, and jump to the code cache
// with that we can start compiling all those blocks from any threads that touch the code
// instead of what we have now where we just pick a single thread and start jitting from there


uint8_t* DbiCompileBasicBlock(uintptr_t appPc)
{
    dasm_State* D = NULL;
    dasm_State** Dst = &D;
    DbiDynasmInit(Dst);
    ExitPatchLabel* patchLabels = NULL;

    if (!CodeCacheInit(appPc))
    {
        goto error;
    }

    size_t reserveSize = 4096;
    uint8_t* blockStart = CodeCacheReserve(reserveSize);
    if (!blockStart)
    {
        goto error;
    }

    ZydisDecoder decoder;
    if (ZYAN_FAILED(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)))
    {
        PeonyLogf("Failed to init zydis decoder");
        goto error;
    }

    ZydisFormatter fmt;
    if (ZYAN_FAILED(ZydisFormatterInit(&fmt, ZYDIS_FORMATTER_STYLE_INTEL)))
    {
        PeonyLogf("Failed to init zydis formatter");
        goto error;
    }

    uint64_t currentPC = appPc;
    // code emitting "cursor". this points to the code cache we need to write the jitted instructions to
    CodeCursor codeOut = {.cursor = blockStart}; 

#if DBI_LOG_COMPILATION_VERBOSE
    PeonyLogf("Compiling basic block at %p -> %p", (void*)appPc, blockStart);
#endif

    OnCompileBasicBlock(&codeOut);

    for (;;)
    {
        // decode/process single instructions until we hit a control flow instr that ends this "basic block"

        ZydisDecodedInstruction instr;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        ZyanStatus status = ZydisDecoderDecodeFull(&decoder, (void*)currentPC, ZYDIS_MAX_INSTRUCTION_LENGTH, &instr, operands);
        if (ZYAN_FAILED(status))
        {
            PeonyLogf("Failed to decode instruction at %p: %lu", (void*)currentPC, status);
            goto error;
        }
        
        // we've hit an instruction like a branch or ret or call that "ends" the current basic block
        // for these, we need to emit some special code that 
        if (IsBasicBlockTerminator(&instr))
        {
            if (!CompileBlockTerminator(&codeOut, currentPC, &instr, operands, Dst, &patchLabels))
            {
                LogDecodedInstructionRange("Terminator that failed to compile", &decoder, &fmt, currentPC, instr.length);
                goto error;
            }
            currentPC += instr.length;
            break;
        }

        if (!EmitAndPossiblyRelocateInstruction(&codeOut, currentPC, &instr, operands))
        {
            PeonyLogf("failed to emit/relocate instruction at %p", (void*)currentPC);
            goto error;
        }
        currentPC += instr.length;
    }

    FlushInstructionCache(GetCurrentProcess(), blockStart, codeOut.cursor - blockStart);

    CodeCachePublishBlock(appPc, blockStart);
    
#if DBI_LOG_COMPILATION_VERBOSE
    LogCompiledBasicBlockComparison(&decoder, &fmt, appPc, currentPC, blockStart, codeOut.cursor);
#endif
    PeonyLogf("There are now %llu entries in the code cache", (unsigned long long)hmlenu(g_codeCache.entries));

    // return the code cache, so now the program will be executing in our instrumented code
    arrfree(patchLabels);
    dasm_free(Dst);
    return (uint8_t*)blockStart;

error:
    arrfree(patchLabels);
    dasm_free(Dst);
    return (uint8_t*)appPc;
}

uint8_t* DbiLookupOrCompile(uintptr_t appPc)
{
    uint8_t* existingCodeCacheEntryForThisPc = CodeCacheLookup(appPc);
    if (existingCodeCacheEntryForThisPc)
    {
        return existingCodeCacheEntryForThisPc;
    }
    // we didn't find an entry in the code cache for this pc, we need to compile this basic block
    return DbiCompileBasicBlock(appPc);
}

__declspec(noinline)
void OnDBIExit(ThreadHijackState* state)
{
    uint64_t appTargetRip = state->savedRegs.Rip;
    uint8_t* codeCacheRip = DbiLookupOrCompile(appTargetRip);
    state->savedRegs.Rip = (DWORD64)codeCacheRip;
}


_Static_assert(offsetof(CONTEXT, EFlags) == 68, "CONTEXT offset wrong");
#define CTXOFFSET_RFLAGS STRINGIFY(68)
_Static_assert(offsetof(CONTEXT, Rax) == 120, "CONTEXT offset wrong");
#define CTXOFFSET_RAX STRINGIFY(120)
_Static_assert(offsetof(CONTEXT, Rcx) == 128, "CONTEXT offset wrong");
#define CTXOFFSET_RCX STRINGIFY(128)
_Static_assert(offsetof(CONTEXT, Rdx) == 136, "CONTEXT offset wrong");
#define CTXOFFSET_RDX STRINGIFY(136)
_Static_assert(offsetof(CONTEXT, Rbx) == 144, "CONTEXT offset wrong");
#define CTXOFFSET_RBX STRINGIFY(144)
_Static_assert(offsetof(CONTEXT, Rsp) == 152, "CONTEXT offset wrong");
#define CTXOFFSET_RSP STRINGIFY(152)
_Static_assert(offsetof(CONTEXT, Rbp) == 160, "CONTEXT offset wrong");
#define CTXOFFSET_RBP STRINGIFY(160)
_Static_assert(offsetof(CONTEXT, Rsi) == 168, "CONTEXT offset wrong");
#define CTXOFFSET_RSI STRINGIFY(168)
_Static_assert(offsetof(CONTEXT, Rdi) == 176, "CONTEXT offset wrong");
#define CTXOFFSET_RDI STRINGIFY(176)
_Static_assert(offsetof(CONTEXT, R8) == 184, "CONTEXT offset wrong");
#define CTXOFFSET_R8 STRINGIFY(184)
_Static_assert(offsetof(CONTEXT, R9) == 192, "CONTEXT offset wrong");
#define CTXOFFSET_R9 STRINGIFY(192)
_Static_assert(offsetof(CONTEXT, R10) == 200, "CONTEXT offset wrong");
#define CTXOFFSET_R10 STRINGIFY(200)
_Static_assert(offsetof(CONTEXT, R11) == 208, "CONTEXT offset wrong");
#define CTXOFFSET_R11 STRINGIFY(208)
_Static_assert(offsetof(CONTEXT, R12) == 216, "CONTEXT offset wrong");
#define CTXOFFSET_R12 STRINGIFY(216)
_Static_assert(offsetof(CONTEXT, R13) == 224, "CONTEXT offset wrong");
#define CTXOFFSET_R13 STRINGIFY(224)
_Static_assert(offsetof(CONTEXT, R14) == 232, "CONTEXT offset wrong");
#define CTXOFFSET_R14 STRINGIFY(232)
_Static_assert(offsetof(CONTEXT, R15) == 240, "CONTEXT offset wrong");
#define CTXOFFSET_R15 STRINGIFY(240)
_Static_assert(offsetof(CONTEXT, Rip) == 248, "CONTEXT offset wrong");
#define CTXOFFSET_RIP STRINGIFY(248)


__attribute__((naked))
void RestoreRegisters(void)
{
    // r10 should contain the ThreadHijackState
    __asm__(
        ".intel_syntax noprefix\n"

        // cpu flags restore
        "mov eax, dword ptr [r10 + " SAVEDREGS_OFF " + " CTXOFFSET_RFLAGS "]\n" // rax = (uint32)state->savedRegs.EFlags
        "push rax\n" // push savedRegs.EFlags onto stack
        "popfq\n" // pop savedRegs.EFlags from stack into CPU RFlags register

        "mov r15, qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_R15"]\n"
        "mov r14, qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_R14"]\n"
        "mov r13, qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_R13"]\n"
        "mov r12, qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_R12"]\n"
        "mov r11, qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_R11"]\n"
        
        "mov r9, qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_R9"]\n"
        "mov r8, qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_R8"]\n"
        "mov rdi, qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_RDI"]\n"
        "mov rsi, qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_RSI"]\n"
        "mov rbp, qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_RBP"]\n"
        
        "mov rbx, qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_RBX"]\n"
        "mov rdx, qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_RDX"]\n"
        "mov rcx, qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_RCX"]\n"
        "mov rax, qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_RAX"]\n"
        
        // restore sp
        "mov rsp, qword ptr [r10 + " SAVEDREGS_OFF " + " CTXOFFSET_RSP "]\n"
        
        // restore final scratch reg
        "mov r10, qword ptr [r10 + " SAVEDREGS_OFF " + " CTXOFFSET_R10 "]\n"
        // jump to original program Rip
        "jmp qword ptr [rip + g_hijackedThreadState + " SAVEDREGS_OFF " + " CTXOFFSET_RIP "]\n"
        
        ".att_syntax prefix\n"
    );
}


__attribute__((naked))
void SaveRegisters(void)
{
    // r10 should contain the ThreadHijackState
    __asm__(
        ".intel_syntax noprefix\n"
        // saving rax before clobber
        "mov qword ptr [r10 + ("CTXOFFSET_RAX" + " SAVEDREGS_OFF ")], rax\n"
        // r10 contains important state, we use rax as tmp to store it
        "mov rax, qword ptr [r10 + "ORIGR10_OFF"]\n"
        "mov qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_R10"], rax\n"
        
        // *(r10 + offset) = register    where r10 is (char*)(ThreadHijackState*)
        // rax already saved
        "mov qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_RCX"], rcx\n"
        "mov qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_RDX"], rdx\n"
        "mov qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_RBX"], rbx\n"
        "mov qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_RBP"], rbp\n"
        "mov qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_RSI"], rsi\n"
        "mov qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_RDI"], rdi\n"
        
        "mov qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_R8"], r8\n"
        "mov qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_R9"], r9\n"
        // r10 already saved
        "mov qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_R11"], r11\n"
        "mov qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_R12"], r12\n"
        "mov qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_R13"], r13\n"
        "mov qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_R14"], r14\n"
        "mov qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_R15"], r15\n"
        // rip should already be stored in the ThreadHijackState by C before this was called. We can't read from it here.
        
        // save rsp, which is a lil different because we used this function itself is 8 bytes on stack
        // so 8 bytes back is the "original" sp
        "lea rax, [rsp + 8]\n" // load up rax with the real "original" sp and then save it off
        "mov qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_RSP"], rax\n"
        
        "pushfq\n" // push flags onto stack
        "pop rax\n" // rax is our "scratch" reg, we pop the flags from stack onto there
        "mov qword ptr [r10 + " SAVEDREGS_OFF " + "CTXOFFSET_RFLAGS"], rax\n" // pushing RFLAGS onto the CONTEXT's EFlags
        
        // restore original rax before returning
        "mov rax, qword ptr [r10 + "CTXOFFSET_RAX" + " SAVEDREGS_OFF "]\n"
        "ret\n"

        ".att_syntax prefix\n"
    );
}


__attribute__((naked))
void DBIExitTrampoline(void)
{
    // we want to call a C function so we must adhere to Windows x64 ABI
    // https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170
    // https://learn.microsoft.com/en-us/cpp/build/stack-usage?view=msvc-170
    // which includes "The stack will always be maintained 16-byte aligned"
    // Integer arguments are passed in registers RCX, RDX, R8, and R9
    // https://dev.to/mirrai/x64-windows-assembly-fundamentals-part-1-what-you-actually-need-to-know-6lg
    // "Before any call the caller must allocate at least 32 bytes (0x20) on the stack even if the function takes fewer than four arguments. This is called the shadow space and it exists so the called function has somewhere to spill its register arguments if it needs to. We also allocate 8 bytes to align the stack because the winapi functions need the stack to be 16 byte aligned or the program crashes"
    __asm__(
        ".intel_syntax noprefix\n"

        // Save the guest r10 before using r10 as our ThreadHijackState pointer.
        "mov qword ptr [rip + g_hijackedThreadState + " ORIGR10_OFF "], r10\n"
        "lea r10, [rip + g_hijackedThreadState]\n"
        "call SaveRegisters\n"

        "mov rcx, r10\n" // param 1 of OnDBIExit is the state ptr
        "mov r13, rsp\n" // save stack ptr to nonvolatile reg so we can restore after fn call
        "and rsp, -0x10\n" // align stack ptr to 16 bytes per win x64 abi
        "sub rsp, 0x20\n" // alloc 32 bytes of shadow space on stack for win abi
        "call OnDBIExit\n"
        "mov rsp, r13\n" // restore stack ptr
        "lea r10, [rip + g_hijackedThreadState]\n" // get state ptr back
        "call RestoreRegisters\n" // this never returns, it jmps to original rip
        "ud2\n" // we should never hit this, because RestoreRegisters should jmp to the original thread's Rip

        ".att_syntax prefix\n"
    );
}

bool HijackThreadRip(DWORD targetThreadId)
{
#if defined(_M_X64) || defined(__x86_64__)
    if (targetThreadId == 0)
    {
        PeonyLogf("No valid targetThreadId was provided in shared comms.");
        return false;
    }

    BOOL isWow64 = FALSE;
    BOOL result = IsWow64Process(GetCurrentProcess(), &isWow64);
    if (result && isWow64) 
    {
        PeonyLogf("Not currently supporting Wow64 (32 bit emulated) processes");
        return false;
    }


    DWORD currentThreadId = GetCurrentThreadId();
    if (targetThreadId == currentThreadId)
    {
        PeonyLogf("Refusing to suspend the current initialization thread (%lu).", targetThreadId);
        return false;
    }

    HANDLE threadHdl = OpenThread(
        THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
        FALSE,
        targetThreadId);
    if (!threadHdl)
    {
        PeonyLogf("OpenThread(%lu) failed: %lu", targetThreadId, GetLastError());
        return false;
    }

    DWORD suspendCount = SuspendThread(threadHdl);
    if (suspendCount == (DWORD)-1)
    {
        PeonyLogf("SuspendThread(%lu) failed: %lu", targetThreadId, GetLastError());
        CloseHandle(threadHdl);
        return false;
    }

    CONTEXT context;
    memset(&context, 0, sizeof(context));
    context.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(threadHdl, &context))
    {
        PeonyLogf("GetThreadContext(%lu) failed: %lu", targetThreadId, GetLastError());
        ResumeThread(threadHdl);
        CloseHandle(threadHdl);
        return false;
    }

    DWORD64 originalRip = context.Rip;
    g_hijackedThreadState = (ThreadHijackState){0};
    // SaveRegisters captures the registers after the hijacked thread resumes,
    // but it cannot recover the original app RIP after SetThreadContext redirects it.
    g_hijackedThreadState.savedRegs.Rip = originalRip;
    // as soon as the thread resumes, it'll run our hijack asm which eventually calls our C func OnDBIExit
    context.Rip = (DWORD64)(uintptr_t)DBIExitTrampoline;

    if (!SetThreadContext(threadHdl, &context))
    {
        PeonyLogf("SetThreadContext(%lu) failed: %lu", targetThreadId, GetLastError());
        ResumeThread(threadHdl);
        CloseHandle(threadHdl);
        return false;
    }

    PeonyLogf(
        "Hijacked thread %lu RIP: %p -> %p, threadState=%p\n",
        targetThreadId,
        originalRip,
        DBIExitTrampoline,
        &g_hijackedThreadState);

    ResumeThread(threadHdl);
    CloseHandle(threadHdl);

    return true;
#else
    (void)targetThreadId;
    PeonyLogf("Thread RIP hijacking currently only supports x64.");
    return false;
#endif
}

#define DEBUG_PICK_THREAD_CONTAINS_NAME "main"

void Initialize()
{
    PeonyLogf("Hello from injection dll! Zydis version = %llu", ZydisGetVersion());
    DWORD pid = GetProcessId(GetCurrentProcess());
    SharedCommsObject* sharedComms = SharedCommsInitialize();
    if (!sharedComms)
    {
        return;
    }

    ModuleInfo* moduleInfos = ListProcessModules(pid);
    PeonyLogf("Target thread id from shared comms: %lu", sharedComms->targetThreadId);
#ifdef DEBUG_PICK_THREAD_CONTAINS_NAME
    if (sharedComms->targetThreadId == 0)
    {
        // pick a thread matching the hardcoded name for debugging/testing
        ThreadInfo* threadInfos = ListProcessThreads(pid);
        for (int i = 0; i < arrlen(threadInfos); i++)
        {
            ThreadInfo* threadInfo = &threadInfos[i];
            if (strstr(threadInfo->threadName, DEBUG_PICK_THREAD_CONTAINS_NAME))
            {
                PeonyLogf("Found thread with name %s threadid %lu\n", threadInfo->threadName, threadInfo->threadId);
                sharedComms->targetThreadId = threadInfo->threadId;
            }
        }
        if (sharedComms->targetThreadId == 0)
        {
            PeonyLogf("Failed to find a thread in the target process with name: %s", DEBUG_PICK_THREAD_CONTAINS_NAME);
            for (int i = 0; i < arrlen(threadInfos); i++)
            {
                ThreadInfo* threadInfo = &threadInfos[i];
                PeonyLogf("Thread %lu | %s", threadInfo->threadId, threadInfo->threadName);
            }
            DWORD pickedThreadId = threadInfos[0].threadId;
            PeonyLogf("Picking the first thread: %lu", pickedThreadId);
            sharedComms->targetThreadId = pickedThreadId;
        }
        arrfree(threadInfos);
    }
#endif
    HijackThreadRip(sharedComms->targetThreadId);
}
