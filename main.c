

// This is the main runner program for this DBI engine
// you can point it at a process/thread id and it'll call injection.c to instrument the process/thread
// this program also handles some communication with the remote process - like pumping logs and coordinating other params

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <tlhelp32.h>
#include <string.h>

#include "shared_defines.h"

#define PEONY_SHARED_COMMS_IMPLEMENTATION
#include "shared_comms.h"
#undef PEONY_SHARED_COMMS_IMPLEMENTATION

// since we might be running code in another non-console process
// we expose this shared mapping the injected dll can write to
// then this main process can pump those logs to it's own stdout 
// so we can essentially log from the external injected process
#define PEONY_PUMP_INJECTED_LOGS 1
#define PEONY_LOG_FILE_CHUNK_SIZE (10 * MB)

typedef struct 
{
    int pid;
    DWORD targetThreadId;
    char exeName[MAX_PATH];
} GlobalState;

GlobalState g_state;

typedef struct
{
    SharedLogObject* log;
    HANDLE logFile;
    HANDLE logFileMapping;
    void* logFileMem;
    uint64_t logFileCapacity;
    uint64_t logFileOffset;
    HANDLE stopEvent;
} LogPumpState;

LogPumpState g_logPumpState;


const char* EatChars(const char* str, const char* sub)
{
    char *match = strstr(str, sub); 
    if (match != NULL) 
    {
        return match + strlen(sub); 
    }
    return str;
}

bool EnsureMappedLogFileCapacity(LogPumpState* state, uint64_t bytesNeeded)
{
    if (!state->logFile || state->logFile == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return false;
    }

    if (bytesNeeded > UINT64_MAX - state->logFileOffset)
    {
        SetLastError(ERROR_FILE_TOO_LARGE);
        return false;
    }

    uint64_t requiredCapacity = state->logFileOffset + bytesNeeded;
    if (requiredCapacity <= state->logFileCapacity)
    {
        return true;
    }

    uint64_t newCapacity = state->logFileCapacity;
    if (newCapacity == 0)
    {
        newCapacity = PEONY_LOG_FILE_CHUNK_SIZE;
    }
    while (newCapacity < requiredCapacity)
    {
        if (newCapacity > UINT64_MAX - PEONY_LOG_FILE_CHUNK_SIZE)
        {
            SetLastError(ERROR_FILE_TOO_LARGE);
            return false;
        }
        newCapacity += PEONY_LOG_FILE_CHUNK_SIZE;
    }

    if (state->logFileMem)
    {
        FlushViewOfFile(state->logFileMem, (SIZE_T)state->logFileOffset);
    }

    HANDLE newMapping = CreateFileMapping(
        state->logFile,
        NULL,
        PAGE_READWRITE,
        (DWORD)(newCapacity >> 32),
        (DWORD)(newCapacity & 0xFFFFFFFFu),
        NULL
    );
    if (newMapping == NULL)
    {
        return false;
    }

    void* newMem = MapViewOfFile(
        newMapping,
        FILE_MAP_ALL_ACCESS,
        0, 0,
        (SIZE_T)newCapacity
    );
    if (newMem == NULL)
    {
        DWORD error = GetLastError();
        CloseHandle(newMapping);
        SetLastError(error);
        return false;
    }

    if (state->logFileMem)
    {
        UnmapViewOfFile(state->logFileMem);
    }
    if (state->logFileMapping)
    {
        CloseHandle(state->logFileMapping);
    }

    state->logFileMapping = newMapping;
    state->logFileMem = newMem;
    state->logFileCapacity = newCapacity;
    return true;
}

void WriteMappedLogFileChunk(LogPumpState* state, const void* bytes, DWORD length)
{
    if (!state->logFileMem || length == 0)
    {
        return;
    }

    DWORD bytesToWrite = length;
    if (!EnsureMappedLogFileCapacity(state, length))
    {
        DWORD error = GetLastError();
        printf("[main] Failed to grow injected log file mapping (%lu); log bytes may be dropped.\n", error);

        uint64_t remaining = (state->logFileOffset < state->logFileCapacity)
            ? (state->logFileCapacity - state->logFileOffset)
            : 0;
        if ((uint64_t)bytesToWrite > remaining)
        {
            bytesToWrite = (DWORD)remaining;
        }
    }

    if (bytesToWrite > 0)
    {
        memcpy((uint8_t*)state->logFileMem + state->logFileOffset, bytes, bytesToWrite);
        state->logFileOffset += bytesToWrite;
    }
}

void DrainInjectedLogs(LogPumpState* state)
{
    SharedLogObject* log = state->log;
    if (!log)
    {
        return;
    }

    for (;;)
    {
        LONG readOffset = log->readOffset;
        LONG writeOffset = log->writeOffset;
        if (readOffset == writeOffset)
        {
            break;
        }
        if (readOffset < 0 || readOffset >= PEONY_LOG_BUFFER_SIZE ||
            writeOffset < 0 || writeOffset >= PEONY_LOG_BUFFER_SIZE)
        {
            InterlockedExchange(&log->readOffset, 0);
            InterlockedExchange(&log->writeOffset, 0);
            break;
        }

        int available = (writeOffset > readOffset)
            ? (writeOffset - readOffset)
            : (PEONY_LOG_BUFFER_SIZE - readOffset);
        fwrite(log->buffer + readOffset, 1, available, stdout);
        WriteMappedLogFileChunk(state, log->buffer + readOffset, (DWORD)available);

        InterlockedExchange(&log->readOffset, (readOffset + available) % PEONY_LOG_BUFFER_SIZE);
    }

    LONG droppedBytes = InterlockedExchange(&log->droppedBytes, 0);
    if (droppedBytes > 0)
    {
        printf("[main] Dropped %ld injected log bytes because the shared log buffer was full.\n", droppedBytes);
    }
}

DWORD WINAPI InjectedLogPumpThread(LPVOID param)
{
    LogPumpState* state = (LogPumpState*)param;
    for (;;)
    {
        DrainInjectedLogs(state);
        if (WaitForSingleObject(state->stopEvent, 50) == WAIT_OBJECT_0)
        {
            break;
        }
    }
    DrainInjectedLogs(state);
    return 0;
}


// =========== CMDLINE ARG PARSING ===============
#define DECLARE_CMDLINE_ARGS \
    X(pid, ParsePid) \
    X(exeName, ParseExeName) \
    X(targetThreadId, ParseTargetThreadId)

typedef enum
{
    #define X(name, ...) name,
    DECLARE_CMDLINE_ARGS
    #undef X
    CMDLINE_ARGS_COUNT,
} CmdlineArg;

const char* CmdlineArgToString(CmdlineArg arg)
{
    switch (arg)
    {
        #define X(name, ...) case name: return #name;
        DECLARE_CMDLINE_ARGS
        #undef X
        default: return "Unknown";
    }
}

void CmdlineArgNoParse(const char* content) {};

void ParsePid(const char* content)
{
    g_state.pid = atoi(content);
}

void ParseExeName(const char* content)
{
    strncpy_s(g_state.exeName, sizeof(g_state.exeName), content, MAX_PATH);
}

void ParseTargetThreadId(const char* content)
{
    g_state.targetThreadId = (DWORD)strtoul(content, NULL, 10);
}

typedef void(*CmdlineParseContent)(const char*);

CmdlineParseContent cmdlineParsers[] = 
{
    #define X(name, fn) fn, 
    DECLARE_CMDLINE_ARGS
    #undef X
};
// ======================================================

typedef struct 
{
    HANDLE remoteProcHdl;
    HANDLE remoteThreadHdl;
    void* remoteMem;
} RemoteProcInfo;

DWORD FindPidByExeName(const char* exeName)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        printf("CreateToolhelp32Snapshot failed: %lu\n", GetLastError());
        return 0;
    }

    PROCESSENTRY32 entry = {0};
    entry.dwSize = sizeof(entry);

    DWORD pid = 0;
    if (Process32First(snapshot, &entry))
    {
        do
        {
            if (_strnicmp(exeName, entry.szExeFile, MAX_PATH) == 0)
            {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return pid;
}

DWORD WaitForPidByExeName(const char* exeName)
{
    printf("Waiting for executable \"%s\"...\n", exeName);
    for (;;)
    {
        DWORD pid = FindPidByExeName(exeName);
        if (pid != 0)
        {
            return pid;
        }
        Sleep(1000);
    }
}

RemoteProcInfo InjectCodeIntoProcess(int pid, const char* sharedLibPath)
{
    RemoteProcInfo remoteProcInfo = {0};
    HANDLE remoteProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | SYNCHRONIZE,
        FALSE,
        pid);
    if (remoteProc == NULL)
    {
        printf("OpenProcess failed %lu. Does PID %i exist?\n", GetLastError(), pid);
        return remoteProcInfo;
    }

    LPVOID remoteMem = VirtualAllocEx(remoteProc, NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remoteMem == NULL)
    {
        printf("VirtualAllocEx failed: %lu\n", GetLastError());
        CloseHandle(remoteProc);
        return remoteProcInfo;
    }

    BOOL ret = WriteProcessMemory(remoteProc, remoteMem, sharedLibPath, strlen(sharedLibPath)+1, NULL);
    if (!ret)
    {
        printf("WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(remoteProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(remoteProc);
        return remoteProcInfo;
    }

    LPTHREAD_START_ROUTINE loadLibraryFn = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandle("kernel32.dll"), "LoadLibraryA");
    if (!loadLibraryFn)
    {
        printf("Failed to find LoadLibraryA: %lu\n", GetLastError());
        VirtualFreeEx(remoteProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(remoteProc);
        return remoteProcInfo;
    }

    size_t remoteThreadParam = 0;
    HANDLE remoteThread = CreateRemoteThread(remoteProc, NULL, 0, loadLibraryFn, remoteMem, 0, NULL);
    if (remoteThread == NULL)
    {
        printf("CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(remoteProc, remoteMem, 0, MEM_RELEASE);
        CloseHandle(remoteProc);
        return remoteProcInfo;
    }
    remoteProcInfo.remoteMem = remoteMem;
    remoteProcInfo.remoteProcHdl = remoteProc;
    remoteProcInfo.remoteThreadHdl = remoteThread;
    return remoteProcInfo;
}

int main(int argc, char** argv)
{
    memset(&g_state, 0, sizeof(g_state));
    int exitCode = 0;

    int cmdlineArgsMask = 0;
    for (int i = 0; i < argc; i++)
    {
        const char* arg = argv[i];
        arg = EatChars(arg, "--");
        for (CmdlineArg cmdlineArg = 0; cmdlineArg < CMDLINE_ARGS_COUNT; cmdlineArg++)
        {
            if (strcmp(arg, CmdlineArgToString(cmdlineArg)) == 0)
            {
                cmdlineParsers[cmdlineArg](argv[i+1]);
                i++;
                cmdlineArgsMask |= (1 << i);
                break;
            }
        }
    }

    if (g_state.pid == 0 && g_state.exeName[0] != '\0')
    {
        g_state.pid = (int)WaitForPidByExeName(g_state.exeName);
    }

    if (g_state.pid == 0)
    {
        // no pid supplied, we should prompt the user
        printf("Enter PID to attach to: ");
        char pidStr[50];
        fgets(pidStr, sizeof(pidStr), stdin);
        sscanf_s(pidStr, "%d", &g_state.pid);
    }
    printf("PID = %i\n", g_state.pid);
    printf("Target thread ID = %lu\n", g_state.targetThreadId);

#if PEONY_PUMP_INJECTED_LOGS
    HANDLE logPumpThread = NULL;
    SharedLogObject* sharedLog = SharedLogInitialize();
    if (sharedLog)
    {
        // SharedLogObject might hold a large buffer, so using a zero-assignment here can cause a stack overflow so we just 0 out the non-buffer members
        memset(sharedLog, 0, offsetof(SharedLogObject, buffer));

        strncpy_s(sharedLog->outputLogFilename, sizeof(sharedLog->outputLogFilename), "PeonyLog.txt", sizeof(sharedLog->outputLogFilename));
        HANDLE logFile = CreateFile(sharedLog->outputLogFilename, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (logFile == INVALID_HANDLE_VALUE)
        {
            printf("CreateFile for logfile failed (%lu).\n", GetLastError());
            return 0;
        }

        g_logPumpState.log = sharedLog;
        g_logPumpState.logFile = logFile;
        g_logPumpState.logFileOffset = 0;
        if (!EnsureMappedLogFileCapacity(&g_logPumpState, PEONY_LOG_FILE_CHUNK_SIZE))
        {
            printf("Initial logfile mapping failed (%lu).\n", GetLastError());
            CloseHandle(logFile);
            memset(&g_logPumpState, 0, sizeof(g_logPumpState));
            return 0;
        }

        g_logPumpState.stopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (g_logPumpState.stopEvent)
        {
            logPumpThread = CreateThread(NULL, 0, InjectedLogPumpThread, &g_logPumpState, 0, NULL);
        }
    }
    else
    {
        printf("Control process failed to created shared log object");
    }
#endif

    SharedCommsObject* sharedComms = SharedCommsInitialize();
    *sharedComms = (SharedCommsObject){0};
    sharedComms->targetThreadId = g_state.targetThreadId;

    const char* injectionDllFile = "injection.dll";
    RemoteProcInfo remoteProcInfo = InjectCodeIntoProcess(g_state.pid, injectionDllFile);
    if (!remoteProcInfo.remoteProcHdl)
    {
        printf("Main injector process: attach failed.\n");
        exitCode = 1;
        goto cleanup;
    }

    printf("Main injector process waiting...\n");
    WaitForSingleObject(remoteProcInfo.remoteProcHdl, INFINITE);

    printf("Main injector process: Peace Out.\n");

cleanup:
    if (g_logPumpState.stopEvent)
    {
        SetEvent(g_logPumpState.stopEvent);
    }
    if (logPumpThread)
    {
        // i'd rather the app quit fast than wait for potentially slow log pumping. 
        // i know that's "bad" but while it's a small personal proj i prefer speed over correctness
        WaitForSingleObject(logPumpThread, 16); 
        CloseHandle(logPumpThread);
    }
    if (g_logPumpState.stopEvent)
    {
        CloseHandle(g_logPumpState.stopEvent);
    }
    if (g_logPumpState.logFileMem)
    {
        FlushViewOfFile(g_logPumpState.logFileMem, (SIZE_T)g_logPumpState.logFileOffset);
        UnmapViewOfFile(g_logPumpState.logFileMem);
    }
    if (g_logPumpState.logFileMapping)
    {
        CloseHandle(g_logPumpState.logFileMapping);
    }
    if (g_logPumpState.logFile && g_logPumpState.logFile != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER endOfLog = {0};
        endOfLog.QuadPart = g_logPumpState.logFileOffset;
        if (SetFilePointerEx(g_logPumpState.logFile, endOfLog, NULL, FILE_BEGIN))
        {
            SetEndOfFile(g_logPumpState.logFile);
        }
        FlushFileBuffers(g_logPumpState.logFile);
        CloseHandle(g_logPumpState.logFile);
    }

    if (remoteProcInfo.remoteThreadHdl)
    {
        CloseHandle(remoteProcInfo.remoteThreadHdl);
    }
    if (remoteProcInfo.remoteMem)
    {
        VirtualFreeEx(remoteProcInfo.remoteProcHdl, remoteProcInfo.remoteMem, 0, MEM_RELEASE);
    }
    if (remoteProcInfo.remoteProcHdl)
    {
        CloseHandle(remoteProcInfo.remoteProcHdl);
    }

    return exitCode;
}
