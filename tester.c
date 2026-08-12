

#include <stdio.h>
#include <windows.h>

int numTimes = 500;

DWORD somefunction(void* userdata)
{
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
    MessageBox(0, TEXT("Tester here"), TEXT("yupp"), MB_OKCANCEL | MB_ICONQUESTION);
    HANDLE otherThreadHdl = CreateThread(0, 0, &somefunction, 0, 0, 0);
    somefunction(0);
    WaitForSingleObject(otherThreadHdl, INFINITE);
    return 0;
}