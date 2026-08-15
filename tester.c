

#include <stdio.h>
#include <windows.h>

int numTimes = 500;


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

DWORD somefunction(void* userdata)
{
    ThreadSetName("somefunction");
    printf("Testing! threadid = %lu\n", GetThreadId(GetCurrentThread()));
    int something = 5;
    while (numTimes--)
    {
        while (something % 2 != 0)
        {
            something++;
            if (something % 3 == 0)
            {
                printf("Aaaaa eeeee oooooo\n");
            }
        }
    }
    return 0;
}

int main(int argc, char** argv)
{
    ThreadSetName("main");
    //MessageBox(0, TEXT("Tester here"), TEXT("yupp"), MB_OKCANCEL | MB_ICONQUESTION);
    printf("tester here\n");
    HANDLE otherThreadHdl = CreateThread(0, 0, &somefunction, 0, 0, 0);
    int x = 0;
    for (;;)
    {
        x++;
        if (x == 100)
        {
            break;
        }
        Sleep(1);
    }
    WaitForSingleObject(otherThreadHdl, INFINITE);
    return 0;
}