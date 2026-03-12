#include <windows.h>
#include <cstdio>

#pragma pack(push, 1)
struct ResetOptions {
    DWORD scenarioType;
    BYTE  wipeData;
    BYTE  overwriteSpace;
    BYTE  preserveWorkplace;
    BYTE  usePayload;
    BYTE  padding[16];
};
#pragma pack(pop)

typedef int(WINAPI* pfnCreateSession)(const wchar_t*, int, void**);
typedef int(WINAPI* pfnPrepareSession)(void*, ResetOptions*);
typedef int(WINAPI* pfnStageOfflineBoot)(void*);
typedef int(WINAPI* pfnReleaseSession)(void*);

static void EnableShutdownPrivilege()
{
    HANDLE hToken;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
    {
        TOKEN_PRIVILEGES tp = {};
        LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr, nullptr);
        CloseHandle(hToken);
    }
}

int main(int argc, char* argv[])
{
    bool skipConfirmation = false;
    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--force") == 0 || strcmp(argv[i], "-f") == 0)
        {
            skipConfirmation = true;
            break;
        }
    }


    printf("========================================\n");
    printf("        FACTORY RESET / WIPE TOOL       \n");
    printf("========================================\n\n");
    printf("WARNING: This operation will perform a FULL FACTORY RESET.\n");
    printf("         ALL data on this machine will be PERMANENTLY DELETED.\n");
    printf("         This action CANNOT be undone.\n\n");

    if (!skipConfirmation)
    {
        printf("Type YES to confirm and proceed, or anything else to cancel: ");
        fflush(stdout);
        char input[16] = {};
        if (fgets(input, sizeof(input), stdin) == NULL)
        {
            printf("\nNo input received. Aborting.\n");
            return 1;
        }
        size_t len = strlen(input);
        if (len > 0 && input[len - 1] == '\n')
            input[len - 1] = '\0';
        if (strcmp(input, "YES") != 0)
        {
            printf("Reset cancelled. No changes were made.\n");
            return 0;
        }
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HMODULE hMod = LoadLibraryW(L"ResetEngine.dll");
    if (!hMod)
    {
        printf("[FAIL] Could not load ResetEngine.dll\n");
        return 1;
    }

    auto fnCreate = (pfnCreateSession)GetProcAddress(hMod, "ResetCreateSession");
    auto fnPrepare = (pfnPrepareSession)GetProcAddress(hMod, "ResetPrepareSession");
    auto fnStage = (pfnStageOfflineBoot)GetProcAddress(hMod, "ResetStageOfflineBoot");
    auto fnRelease = (pfnReleaseSession)GetProcAddress(hMod, "ResetReleaseSession");

    if (!fnCreate || !fnPrepare || !fnStage || !fnRelease)
    {
        printf("[FAIL] Could not resolve ResetEngine functions\n");
        FreeLibrary(hMod);
        return 1;
    }

    // 1. Create session
    void* session = nullptr;
    int hr = fnCreate(L"C:", 0, &session);
    printf("[1/3] CreateSession: 0x%08X\n", hr);
    if (hr < 0 || !session) { printf("[FAIL] Could not create session\n"); return 1; }

    // 2. Prepare - scenario 1 = Reset (remove everything)
    ResetOptions opts = {};
    opts.scenarioType = 1;
    opts.wipeData = 1;

    hr = fnPrepare(session, &opts);
    printf("[2/3] PrepareSession: 0x%08X\n", hr);
    if (hr < 0) { fnRelease(session); return 1; }

    // 3. Stage offline boot
    hr = fnStage(session);
    printf("[3/3] StageOfflineBoot: 0x%08X\n", hr);
    if (hr < 0) { fnRelease(session); return 1; }

    fnRelease(session);
    FreeLibrary(hMod);
    CoUninitialize();

    // 4. Reboot
    printf("\nAll stages completed successfully.\n");
    printf("Rebooting system to complete factory reset...\n");

    EnableShutdownPrivilege();
    BOOL ok = InitiateSystemShutdownExW(nullptr, nullptr, 0, TRUE, TRUE, 0x20001);
    if (!ok)
    {
        printf("[WARN] InitiateSystemShutdownExW failed, trying shutdown.exe...\n");
        system("shutdown /r /t 0 /f");
    }

    return 0;
}