

#include <stdint.h>
#include <stdio.h>
#include <windows.h>

#define PEONY_SHARED_COMMS_IMPLEMENTATION
#include "shared_comms.h"

// to ensure the dbihotlook isn't optimized out
volatile uint64_t g_dbiSink;

typedef uint64_t (*IndirectFunction)(uint64_t value);

__declspec(noinline)
uint64_t AddOne(uint64_t value)
{
    return value + 1;
}

__declspec(noinline)
uint64_t AddTwo(uint64_t value)
{
    return value + 2;
}

__declspec(noinline)
uint64_t CallIndexedFunction(IndirectFunction* functions, size_t index, uint64_t value)
{
    return functions[index](value) + 1;
}

__declspec(noinline)
uint64_t TailCallIndexedFunction(IndirectFunction* functions, size_t index, uint64_t value)
{
    return functions[index](value);
}


const DWORD MS_VC_EXCEPTION = 0x406D1388;

#pragma pack(push,8)
typedef struct tagTHREADNAME_INFO
{
	DWORD dwType; // Must be 0x1000.
	LPCSTR szName; // Pointer to name (in user addr space).
	DWORD dwThreadID; // Thread ID (-1=caller thread).
	DWORD dwFlags; // Reserved for future use, must be zero.
} THREADNAME_INFO;
#pragma pack(pop)


void ThreadSetName(const char* threadName)
{
    #if 0
	THREADNAME_INFO info;
	info.dwType = 0x1000;
	info.szName = threadName;
	info.dwThreadID = GetCurrentThreadId();
	info.dwFlags = 0;
	__try
	{
		RaiseException( MS_VC_EXCEPTION, 0, sizeof(info)/sizeof(ULONG_PTR), (ULONG_PTR*)&info );
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
	}
    #else
    wchar_t wideName[128];

    int wideLen = MultiByteToWideChar(CP_UTF8, 0, threadName, -1, wideName, _countof(wideName));
    if (wideLen <= 0 || wideLen > (int)_countof(wideName))
    {
        return;
    }

    SetThreadDescription(GetCurrentThread(), wideName);
    #endif
}

__declspec(noinline)
uint64_t DbiHotLoop(uint64_t seed)
{
    uint64_t x = seed;
    for (uint64_t i = 0; i < 500000000ULL; i++)
    {
        x += (i ^ 0x9E3779B97F4A7C15ULL);
        if ((x & 7) == 3)
        {
            x = (x << 9) ^ (x >> 5) ^ i;
        }
        else
        {
            x = (x * 33) + (i | 1);
        }
    }
    return x;
}

DWORD OffThreadFunc(void* _)
{
    DbiHotLoop(21);
    IndirectFunction functions[] = { AddOne, AddTwo };
    uint64_t indirectResult = CallIndexedFunction(functions, 1, 40);
    uint64_t indirectJumpResult = TailCallIndexedFunction(functions, 0, 40);
    PeonyLogf("indexed indirect call (success=%i) result=%llu expected=43\n",
            indirectResult == 43, (unsigned long long)indirectResult);
    PeonyLogf("indexed indirect jump (success=%i) result=%llu expected=41\n",
            indirectJumpResult == 41, (unsigned long long)indirectJumpResult);
    return 0;
}

int main(int argc, char** argv)
{
    ThreadSetName("main");
    PeonyLogf("tester pid=%lu main_tid=%lu\n", GetCurrentProcessId(), GetCurrentThreadId());
    HANDLE offthread = CreateThread(NULL, 0, OffThreadFunc, 0, 0, 0);  
    g_dbiSink = DbiHotLoop(0x123456789ABCDEF0ULL);
    unsigned long long expected = 16815769637575916324LLU;
    PeonyLogf("tester done (success=%i) sink=%llu  expected=%llu\n", 
            (unsigned long long)g_dbiSink == expected, (unsigned long long)g_dbiSink, expected);
    WaitForSingleObject(offthread, INFINITE);
    return 0;
}
