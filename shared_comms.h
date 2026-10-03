#ifndef PEONY_SHARED_COMMS_H
#define PEONY_SHARED_COMMS_H

#ifdef __INTELLISENSE__
#define PEONY_SHARED_COMMS_IMPLEMENTATION
#endif

// shared mapping that the main driver program and the injected dll both use to communicate

#include <windows.h>
#include "shared_defines.h"
char* sharedCommsMappingName = "Peony_SharedMapping";
char* sharedLogMappingName = "Peony_LogMapping";

#define PEONY_LOG_BUFFER_SIZE (100 * MB)

typedef struct 
{
    DWORD targetThreadId;
} SharedCommsObject;

typedef struct
{
    volatile LONG writeOffset;
    volatile LONG readOffset;
    volatile LONG droppedBytes;
    char outputLogFilename[MAX_PATH];
    HANDLE logFile;
    void* logFileMem;

    // must be last
    char buffer[PEONY_LOG_BUFFER_SIZE];
    long initialized;
} SharedLogObject;

SharedCommsObject* SharedCommsInitializeForProcess(DWORD processId);
SharedLogObject* SharedLogInitialize();

#define PeonyLogf(...) _PeonyLogf(__FILE__, __VA_ARGS__)

extern SharedLogObject* g_sharedLog;

#ifdef PEONY_SHARED_COMMS_IMPLEMENTATION

#include <windows.h>
#include <stdio.h>

static SharedLogObject* g_sharedLog;

SharedCommsObject* SharedCommsInitializeForProcess(DWORD processId)
{
    size_t mappingSize = sizeof(SharedCommsObject);
    char mappingName[64];
    snprintf(mappingName, sizeof(mappingName), "%s_%lu", sharedCommsMappingName, processId);
    HANDLE filemapping = CreateFileMapping(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        mappingSize,
        mappingName
    );
    if (filemapping == NULL) 
    {
        printf("CreateFileMapping failed (%lu).\n", GetLastError());
        return 0;
    }
    BOOL mappingAlreadyExisted = (GetLastError() == ERROR_ALREADY_EXISTS);
    LPVOID sharedMem = MapViewOfFile(
        filemapping,
        FILE_MAP_ALL_ACCESS,
        0, 0,
        mappingSize
    );
    if (sharedMem == NULL) 
    {
        printf("MapViewOfFile failed (%lu).\n", GetLastError());
        CloseHandle(filemapping);
        return 0;
    }
    if (mappingAlreadyExisted)
    {
        printf("Connected to existing shared memory\n");
    }
    else
    {
        printf("Created shared memory block\n");
    }
    // "leaking" the shared mem/file mapping obj because these should exist for the lifetime of the callee
    // UnmapViewOfFile(sharedMem);
    // CloseHandle(filemapping);
    return (SharedCommsObject*)sharedMem;
}

SharedLogObject* SharedLogInitialize()
{
    size_t mappingSize = sizeof(SharedLogObject);
    HANDLE filemapping = CreateFileMapping(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        mappingSize,
        sharedLogMappingName
    );
    if (filemapping == NULL)
    {
        printf("CreateFileMapping for log failed (%lu).\n", GetLastError());
        return 0;
    }
    LPVOID sharedMem = MapViewOfFile(
        filemapping,
        FILE_MAP_ALL_ACCESS,
        0, 0,
        mappingSize
    );
    if (sharedMem == NULL)
    {
        printf("MapViewOfFile for log failed (%lu).\n", GetLastError());
        CloseHandle(filemapping);
        return 0;
    }
    
    SharedLogObject* sharedLog = (SharedLogObject*)sharedMem;
    // null out the shared log only once across all processes that share it
    if (!InterlockedCompareExchange(&sharedLog->initialized, true, false))
    {
        // SharedLogObject might hold a large buffer, so using a zero-assignment here can cause a stack overflow so we just 0 out the non-buffer members
        memset(sharedLog, 0, offsetof(SharedLogObject, buffer));
    }
    return sharedLog;
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

void _PeonyLogf(const char* file, const char* format, ...)
{
    char line[1024];
    int prefixLength = snprintf(line, sizeof(line), "[%s:%lu] ", file, GetCurrentThreadId());
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


#endif


#endif
