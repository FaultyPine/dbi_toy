
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
    DWORD64 r15;
    DWORD64 r14;
    DWORD64 r13;
    DWORD64 r12;
    DWORD64 r11;
    DWORD64 r10;
    DWORD64 r9;
    DWORD64 r8;
    DWORD64 rbp;
    DWORD64 rdi;
    DWORD64 rsi;
    DWORD64 rdx;
    DWORD64 rcx;
    DWORD64 rbx;
    DWORD64 rax;
    DWORD64 rflags;
    DWORD64 rsp;
} SavedRegisterState;

typedef struct
{
    void* originalRip;
    DWORD64 originalR10;
    DWORD64 originalR11;
    volatile unsigned long long basicBlockCount;
    SavedRegisterState savedRegs;
} ThreadHijackState;

_Static_assert(offsetof(ThreadHijackState, originalRip) == 0x00, "unexpected ThreadHijackState layout");
_Static_assert(offsetof(ThreadHijackState, originalR10) == 0x08, "unexpected ThreadHijackState layout");
_Static_assert(offsetof(ThreadHijackState, originalR11) == 0x10, "unexpected ThreadHijackState layout");
_Static_assert(offsetof(ThreadHijackState, basicBlockCount) == 0x18, "unexpected ThreadHijackState layout");
_Static_assert(offsetof(ThreadHijackState, savedRegs) == 0x20, "unexpected ThreadHijackState layout");
_Static_assert(offsetof(ThreadHijackState, savedRegs.rsp) == 0xa0, "unexpected ThreadHijackState layout");

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
void OnBasicBlockEnter(ThreadHijackState* state, SavedRegisterState* savedRegs)
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
    (void)savedRegs;
    state->basicBlockCount++;
    PeonyLogf("basic block count inc %llu", state->basicBlockCount);
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

#if defined(_M_X64) || defined(__x86_64__)
// Called with R11 = ThreadHijackState*. The CALL return address is on the stack,
// so saved RSP is [rsp + 8], which is the target thread's original RSP.
__attribute__((naked))
void SaveHijackedThreadRegisters(void)
{
    // r11 is g_hijackedThreadState
    __asm__(
        ".intel_syntax noprefix\n"
        "mov qword ptr [r11 + 0x90], rax\n"
        "mov qword ptr [r11 + 0x88], rbx\n"
        "mov qword ptr [r11 + 0x80], rcx\n"
        "mov qword ptr [r11 + 0x78], rdx\n"
        "mov qword ptr [r11 + 0x70], rsi\n"
        "mov qword ptr [r11 + 0x68], rdi\n"
        "mov qword ptr [r11 + 0x60], rbp\n"
        "mov qword ptr [r11 + 0x58], r8\n"
        "mov qword ptr [r11 + 0x50], r9\n"
        "mov rax, qword ptr [r11 + 0x8]\n"
        "mov qword ptr [r11 + 0x48], rax\n"
        "mov rax, qword ptr [r11 + 0x10]\n"
        "mov qword ptr [r11 + 0x40], rax\n"
        "mov qword ptr [r11 + 0x38], r12\n"
        "mov qword ptr [r11 + 0x30], r13\n"
        "mov qword ptr [r11 + 0x28], r14\n"
        "mov qword ptr [r11 + 0x20], r15\n"
        "pushfq\n"
        "pop rax\n"
        "mov qword ptr [r11 + 0x98], rax\n"
        "lea rax, [rsp + 0x8]\n"
        "mov qword ptr [r11 + 0xa0], rax\n"
        "mov rax, qword ptr [r11 + 0x90]\n"
        "ret\n"
        ".att_syntax prefix\n"
    );
}

// Does not return. It restores the target thread state and jumps to originalRip.
__attribute__((naked))
void RestoreHijackedThreadRegisters(void)
{
    __asm__(
        ".intel_syntax noprefix\n"
        "mov rax, qword ptr [rcx]\n"
        "mov rdx, qword ptr [rcx + 0xa0]\n"
        "mov qword ptr [rdx - 0x8], rax\n"
        "mov r15, qword ptr [rcx + 0x20]\n"
        "mov r14, qword ptr [rcx + 0x28]\n"
        "mov r13, qword ptr [rcx + 0x30]\n"
        "mov r12, qword ptr [rcx + 0x38]\n"
        "mov r11, qword ptr [rcx + 0x40]\n"
        "mov r10, qword ptr [rcx + 0x48]\n"
        "mov r9, qword ptr [rcx + 0x50]\n"
        "mov r8, qword ptr [rcx + 0x58]\n"
        "mov rbp, qword ptr [rcx + 0x60]\n"
        "mov rdi, qword ptr [rcx + 0x68]\n"
        "mov rsi, qword ptr [rcx + 0x70]\n"
        "mov rdx, qword ptr [rcx + 0x78]\n"
        "mov rbx, qword ptr [rcx + 0x88]\n"
        "mov rax, qword ptr [rcx + 0x98]\n"
        "push rax\n"
        "popfq\n"
        "mov rax, qword ptr [rcx + 0x90]\n"
        "mov rsp, qword ptr [rcx + 0xa0]\n"
        "mov rcx, qword ptr [rcx + 0x80]\n"
        "jmp qword ptr [rsp - 0x8]\n"
        ".att_syntax prefix\n"
    );
}

__attribute__((naked))
void RipHijackTrampoline(void)
{
    __asm__(
        ".intel_syntax noprefix\n"
        "lea r11, [rip + g_hijackedThreadState]\n"
        "call SaveHijackedThreadRegisters\n"
        "mov r12, r11\n"
        "mov rcx, r11\n"
        "lea rdx, [r11 + 0x20]\n"
        "mov r13, rsp\n"
        "and rsp, -0x10\n"
        "sub rsp, 0x20\n"
        "call OnBasicBlockEnter\n"
        "mov rsp, r13\n"
        "mov rcx, r12\n"
        "call RestoreHijackedThreadRegisters\n"
        "ud2\n"
        ".att_syntax prefix\n"
    );
}
#endif

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

    void* originalRip = (void*)(uintptr_t)context.Rip;
    g_hijackedThreadState = (ThreadHijackState){0};
    g_hijackedThreadState.originalRip = originalRip;
    g_hijackedThreadState.originalR10 = context.R10;
    g_hijackedThreadState.originalR11 = context.R11;

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
        originalRip,
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
