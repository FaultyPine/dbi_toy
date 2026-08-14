
// this is a shared lib that is injected into the remote process and is in charge of instrumenting the process it is injected into

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <windows.h>
#include <TlHelp32.h>
#include "Zydis.h"

#define STB_DS_IMPLEMENTATION
#include "external/std_ds.h"

#include "shared_defines.h"

#define PEONY_SHARED_COMMS_IMPLEMENTATION
#include "shared_comms.h"
#undef PEONY_SHARED_COMMS_IMPLEMENTATION

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "user32.lib")

void Initialize();
DWORD WINAPI InitializeThread(LPVOID param);

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

static ThreadHijackState g_hijackedThreadState;
static SharedLogObject* g_sharedLog;

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



__declspec(noinline)
void OnThreadHijack(ThreadHijackState* state)
{
    // TODO: look at PC of this thread
    // if we have this basic block in the code cache, jump there i think?
    // else...
    // go from that PC to end of next basic block.
    // disassemble ^ that range
    // allocate destination machine code in code cache so we have a known address of the code
    // transform disassembly with any desired instrumentation
    // re-encode transformed disasm to machine code
    // copy to destination code cache block
    // patch original code to jump to our code cache block
    // make sure our code cache block jumps back to original code
    PeonyLogf("We have hijacked the thread!\n");
}

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

DWORD WINAPI InitializeThread(LPVOID param)
{
    (void)param;
    Initialize();
    return 0;
}

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
void RipHijackTrampoline(void)
{
    // we want to call a C function so we must adhere to Windows x64 ABI
    // https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170
    // https://learn.microsoft.com/en-us/cpp/build/stack-usage?view=msvc-170
    // which includes "The stack will always be maintained 16-byte aligned"
    // Integer arguments are passed in registers RCX, RDX, R8, and R9
    // https://dev.to/mirrai/x64-windows-assembly-fundamentals-part-1-what-you-actually-need-to-know-6lg
    // "Before any call the caller must allocate at least 32 bytes (0x20) on the stack even if the function takes fewer than four arguments. This is called the shadow space and it exists so the called function has somewhere to spill its register arguments if it needs to. We also allocate 8 bytes to align the stack because the winapi functions need the stack to be 16 byte aligned or the program crashes"
    // I've already backed up r10 in g_hijackedThreadState, so that can be scratch reg at the start
    __asm__(
        ".intel_syntax noprefix\n"

        // "rip-relative". We can't just load the 64bit pointer of this global
        // there's no way to express that in x64, so this lea must be a 32bit address, but our module (and therefore the static)
        // may be loaded outside the 4GB range a 32bit number could express. So we need to compute a 32bit address of this global
        // so we use the current instruction pointer (rip) as a base and then offset from that to the global. 
        "lea rcx, [rip + g_hijackedThreadState]\n"   // param 1 of the function is the state ptr
        // don't really need to save regs here because we copy the CONTEXT before this func
        "mov r13, rsp\n" // save stack ptr to nonvolatile reg so we can restore after fn call
        "and rsp, -0x10\n" // align stack ptr to 16 bytes per win x64 abi
        "sub rsp, 0x20\n" // alloc 32 bytes of shadow space on stack for win abi
        "call OnThreadHijack\n"
        "mov rsp, r13\n" // restore stack ptr
        "lea r10, [rip + g_hijackedThreadState]\n"  // get state ptr back
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
    // just ctrl regs and standard int regs for now...?
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER; 
    if (!GetThreadContext(threadHdl, &context))
    {
        PeonyLogf("GetThreadContext(%lu) failed: %lu", targetThreadId, GetLastError());
        ResumeThread(threadHdl);
        CloseHandle(threadHdl);
        return false;
    }

    g_hijackedThreadState = (ThreadHijackState){0};
    // r10 is our "scratch space" before we save off regs, so we back it up here
    g_hijackedThreadState.originalR10 = context.R10;
    // saving off the thread context so we can restore it after our trampoline
    g_hijackedThreadState.savedRegs = context;
    // as soon as the thread resumes, it'll run our hijack asm which eventually calls our C func OnThreadHijack
    context.Rip = (DWORD64)(uintptr_t)RipHijackTrampoline;

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
        context.Rip,
        RipHijackTrampoline,
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

#define DEBUG_PICK_THREAD_WITH_NAME "main"

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
#ifdef DEBUG_PICK_THREAD_WITH_NAME
    if (sharedComms->targetThreadId == 0)
    {
        // pick a thread matching the hardcoded name for debugging/testing
        ThreadInfo* threadInfos = ListProcessThreads(pid);
        for (int i = 0; i < arrlen(threadInfos); i++)
        {
            ThreadInfo* threadInfo = &threadInfos[i];
            if (strncmp(threadInfo->threadName, DEBUG_PICK_THREAD_WITH_NAME, THREAD_NAME_MAX_LEN) == 0)
            {
                sharedComms->targetThreadId = threadInfo->threadId;
            }
        }
        if (sharedComms->targetThreadId == 0)
        {
            PeonyLogf("Failed to find a thread in the target process with name: %s", DEBUG_PICK_THREAD_WITH_NAME);
            for (int i = 0; i < arrlen(threadInfos); i++)
            {
                ThreadInfo* threadInfo = &threadInfos[i];
                PeonyLogf("Thread %lu | %s", threadInfo->threadId, threadInfo->threadName);
            }
            PeonyLogf("Picking the first thread: %lu", threadInfos[0].threadId);
            sharedComms->targetThreadId = threadInfos[0].threadId;
        }
        arrfree(threadInfos);
    }
#endif
    HijackThreadRip(sharedComms->targetThreadId);
}
