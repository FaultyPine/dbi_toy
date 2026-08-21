#ifndef PEONY_SHARED_COMMS_H
#define PEONY_SHARED_COMMS_H

#ifdef __INTELLISENSE__
#define PEONY_SHARED_COMMS_IMPLEMENTATION
#endif

// shared mapping that the main driver program and the injected dll both use to communicate

#include <windows.h>

char* sharedCommsMappingName = "Peony_SharedMapping";
char* sharedLogMappingName = "Peony_LogMapping";

#define PEONY_LOG_BUFFER_SIZE (1 * MB)

typedef struct 
{
    DWORD targetThreadId;
} SharedCommsObject;

typedef struct
{
    volatile LONG writeOffset;
    volatile LONG readOffset;
    volatile LONG droppedBytes;
    char buffer[PEONY_LOG_BUFFER_SIZE];
} SharedLogObject;

SharedCommsObject* SharedCommsInitialize();
SharedLogObject* SharedLogInitialize();

#ifdef PEONY_SHARED_COMMS_IMPLEMENTATION

#include <windows.h>
#include <stdio.h>

SharedCommsObject* SharedCommsInitialize()
{
    size_t mappingSize = sizeof(SharedCommsObject);
    HANDLE filemapping = CreateFileMapping(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        0,
        mappingSize,
        sharedCommsMappingName
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
    return (SharedLogObject*)sharedMem;
}

#endif


#endif
