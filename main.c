

// This is the main runner program for this DBI engine
// you can point it at a process/thread id and it'll call injection.c to instrument the process/thread
// this program also handles some communication with the remote process - like pumping logs and coordinating other params

#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
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

typedef struct 
{
    int pid;
    DWORD targetThreadId;
} GlobalState;

GlobalState g_state;

typedef struct
{
    SharedLogObject* log;
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

void DrainInjectedLogsToStdout(SharedLogObject* log)
{
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
        DrainInjectedLogsToStdout(state->log);
        if (WaitForSingleObject(state->stopEvent, 50) == WAIT_OBJECT_0)
        {
            break;
        }
    }
    DrainInjectedLogsToStdout(state->log);
    return 0;
}


// =========== CMDLINE ARG PARSING ===============
#define DECLARE_CMDLINE_ARGS \
    X(pid, ParsePid) \
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

RemoteProcInfo InjectCodeIntoProcess(int pid, const char* sharedLibPath)
{
    RemoteProcInfo remoteProcInfo = {0};
    HANDLE remoteProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | SYNCHRONIZE,
        FALSE,
        g_state.pid);
    if (remoteProc == NULL)
    {
        printf("OpenProcess failed %lu. Does PID %i exist?\n", GetLastError(), g_state.pid);
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
        *sharedLog = (SharedLogObject){0};
        g_logPumpState.log = sharedLog;
        g_logPumpState.stopEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (g_logPumpState.stopEvent)
        {
            logPumpThread = CreateThread(NULL, 0, InjectedLogPumpThread, &g_logPumpState, 0, NULL);
        }
    }
#endif

    SharedCommsObject* sharedComms = SharedCommsInitialize();
    *sharedComms = (SharedCommsObject){0};
    sharedComms->targetThreadId = g_state.targetThreadId;

    const char* injectionDllFile = "injection.dll";
    RemoteProcInfo remoteProcInfo = InjectCodeIntoProcess(g_state.pid, injectionDllFile);

    printf("Main injector process waiting...\n");
    WaitForSingleObject(remoteProcInfo.remoteProcHdl, INFINITE);

    printf("Main injector process: Peace Out.\n");

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

    CloseHandle(remoteProcInfo.remoteThreadHdl);
    VirtualFreeEx(remoteProcInfo.remoteProcHdl, remoteProcInfo.remoteMem, 0, MEM_RELEASE);
    CloseHandle(remoteProcInfo.remoteProcHdl);

    return 0;
}
