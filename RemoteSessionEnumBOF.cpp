#include <Windows.h>
#include <wchar.h>
#include <winternl.h>
#include "base\helpers.h"

#ifdef _DEBUG
#include "base\mock.h"
#undef DECLSPEC_IMPORT  
#define DECLSPEC_IMPORT
#pragma comment(lib, "winsta.lib")
#endif

#define MAX_STRINGS 100
#define MAX_STRING_LENGTH 256


typedef WCHAR WINSTATIONNAME[32 + 1];

typedef enum _WINSTATIONSTATECLASS {
    State_Active = 0,
    State_Connected,
    State_ConnectQuery,
    State_Shadow,
    State_Disconnected,
    State_Idle,
    State_Listen,
    State_Reset,
    State_Down,
    State_Init
} WINSTATIONSTATECLASS;

typedef struct _SESSIONIDW {
    union {
        ULONG SessionId;
        ULONG LogonId;
    };
    WINSTATIONNAME WinStationName;
    WINSTATIONSTATECLASS State;
} SESSIONIDW, * PSESSIONIDW;

typedef HANDLE(WINAPI* LPFN_WinStationOpenServerW)(PWSTR);
typedef BOOLEAN(WINAPI* LPFN_WinStationCloseServer)(HANDLE);
typedef BOOLEAN(WINAPI* LPFN_WinStationEnumerateW)(HANDLE, PSESSIONIDW*, PULONG);
typedef BOOLEAN(WINAPI* LPFN_WinStationQueryInformationW)(HANDLE, ULONG, WINSTATIONINFOCLASS, PVOID, ULONG, PULONG);

extern "C" {
#include "beacon.h"
    DFR(KERNEL32, LoadLibraryA)
#define LoadLibraryA KERNEL32$LoadLibraryA
    DFR(KERNEL32, LocalFree)
#define LocalFree KERNEL32$LocalFree
    DFR(KERNEL32, FreeLibrary)
#define FreeLibrary KERNEL32$FreeLibrary
    DFR(KERNEL32, GetProcAddress)
#define GetProcAddress KERNEL32$GetProcAddress
    DFR(MSVCRT, mbstowcs);
#define mbstowcs MSVCRT$mbstowcs
    DFR(MSVCRT, wcslen);
#define wcslen MSVCRT$wcslen
    DFR(MSVCRT, exit);
#define exit MSVCRT$exit
    DFR(MSVCRT, strlen);
#define strlen MSVCRT$strlen
    DFR(MSVCRT, malloc);
#define malloc MSVCRT$malloc


    void wcsCopyMem(wchar_t* dest, const wchar_t* src, size_t destSize) {
        if (!dest || !src || destSize == 0) return;

        size_t len = 0;
        while (src[len] != L'\0' && len < destSize - 1) {
            dest[len] = src[len];
            len++;
        }
        dest[len] = L'\0';  
    }

    int extractWideStrings(const BYTE* byteArray, size_t size, WCHAR outputStrings[MAX_STRINGS][MAX_STRING_LENGTH]) {
        int stringCount = 0;
        WCHAR currentString[MAX_STRING_LENGTH];
        size_t currentPos = 0;

        for (size_t i = 0; i < size / sizeof(WCHAR); ++i) {
            WCHAR currentChar = byteArray[i * sizeof(WCHAR)];

            if (currentChar == 0 && currentPos == 0) {
                continue;
            }

            if (currentChar == 0) {
                currentString[currentPos] = L'\0';
                if (stringCount < MAX_STRINGS) {
                    wcsCopyMem(outputStrings[stringCount], currentString, MAX_STRING_LENGTH);
                    stringCount++;
                }
                currentPos = 0;
            }
            else {
                currentString[currentPos] = currentChar;
                currentPos++;
            }
        }

        return stringCount;
    }

    void* LoadFunctionFromDLL(HINSTANCE hDLL, const char* functionName) {
        void* functionPtr = GetProcAddress(hDLL, functionName);
        if (functionPtr == NULL) {
            BeaconPrintf(CALLBACK_ERROR, "Failed to find function: %s\n", functionName);
            FreeLibrary(hDLL);
            exit(1);
        }
        return functionPtr;
    }

        int RemoteSessionEnum(int argc, char* argv[]) {
        if (argc != 1) {
            BeaconPrintf(CALLBACK_ERROR, "Usage: %s <serverName>\n", argv[0]);
            return 1;
        }

        // wchar server name
        wchar_t serverName[256];
        mbstowcs(serverName, argv[1], strlen(argv[1]) + 1);

        if (wcslen(serverName) > 20) {
            BeaconPrintf(CALLBACK_ERROR, "Server name exceeds maximum length.\n");
            return 1;
        }

        HINSTANCE hDLL = LoadLibraryA(TEXT("winsta.dll"));
        if (hDLL == NULL) {
            BeaconPrintf(CALLBACK_ERROR, "Failed to load winsta.dll\n");
            return 1;
        }

        LPFN_WinStationOpenServerW pfnWinStationOpenServerW = (LPFN_WinStationOpenServerW)LoadFunctionFromDLL(hDLL, "WinStationOpenServerW");
        LPFN_WinStationCloseServer pfnWinStationCloseServer = (LPFN_WinStationCloseServer)LoadFunctionFromDLL(hDLL, "WinStationCloseServer");
        LPFN_WinStationEnumerateW pfnWinStationEnumerateW = (LPFN_WinStationEnumerateW)LoadFunctionFromDLL(hDLL, "WinStationEnumerateW");
        LPFN_WinStationQueryInformationW pfnWinStationQueryInformationW = (LPFN_WinStationQueryInformationW)LoadFunctionFromDLL(hDLL, "WinStationQueryInformationW");

        // opening server
        HANDLE hServer = pfnWinStationOpenServerW(serverName);
        if (hServer == NULL) {
            BeaconPrintf(CALLBACK_ERROR, "Failed to open server\n");
            FreeLibrary(hDLL);
            return 1;
        }

        // enumerating sessions
        PSESSIONIDW pSessionIds = NULL;
        ULONG count = 0;
        BOOLEAN enumResult = pfnWinStationEnumerateW(hServer, &pSessionIds, &count);

        if (enumResult) {
            BeaconPrintf(CALLBACK_OUTPUT, "Number of sessions: %lu\n", count);
            for (ULONG i = 0; i < count; i++) {
                BeaconPrintf(CALLBACK_OUTPUT, "SessionID: %lu\n", pSessionIds[i].SessionId);
                BeaconPrintf(CALLBACK_OUTPUT, "State: %d\n", pSessionIds[i].State);
                BeaconPrintf(CALLBACK_OUTPUT, "SessionName: %ls\n", pSessionIds[i].WinStationName);

                WINSTATIONINFORMATIONW wsInfo = {};
                ULONG ReturnLen;
                if (pfnWinStationQueryInformationW &&
                    pfnWinStationQueryInformationW(hServer,
                        pSessionIds[i].SessionId,
                        WinStationInformation,
                        &wsInfo,
                        sizeof(wsInfo),
                        &ReturnLen) &&
                    (wsInfo.LogonId != 0)) {

                    // extracting strings
                    WCHAR(*reserved3Strings)[MAX_STRING_LENGTH] = (WCHAR(*)[MAX_STRING_LENGTH])malloc(MAX_STRINGS * MAX_STRING_LENGTH * sizeof(WCHAR));
                    if (reserved3Strings == NULL) {
                        BeaconPrintf(CALLBACK_ERROR, "Memory allocation failed.\n");
                        return 1;
                    }
                }
                else {
                    BeaconPrintf(CALLBACK_OUTPUT, "Failed to query session info for SessionName: %ls\n", pSessionIds[i].WinStationName);
                }
            } 
            LocalFree(pSessionIds);
        }
        else {
            BeaconPrintf(CALLBACK_OUTPUT, "Failed to enumerate sessions.\n");
        }


        // closing server
        pfnWinStationCloseServer(hServer);
        FreeLibrary(hDLL);


        return 0;
    }

        void go(char* args, int len) {
            datap parser;
            BeaconDataParse(&parser, args, len);

            // Estrai il nome del server
            char* serverName = args;

            BeaconPrintf(CALLBACK_OUTPUT, "Raw data received: %s\n", args);
            BeaconPrintf(CALLBACK_OUTPUT, "Extracted server name: %s\n", serverName);

            // Se non   stato estratto correttamente il serverName
            if (serverName == NULL || strlen(serverName) == 0) {
                BeaconPrintf(CALLBACK_ERROR, "Server name not provided.\n");
                return;
            }

            // Utilizza solo il nome del server
            char* argv[] = { serverName };
            int argc = 1;

            RemoteSessionEnum(argc, argv);
        }

}

#if defined(_DEBUG) && !defined(_GTEST)

int main(int argc, char* argv[]) {
    char args[] = "hostname";
    int len = sizeof(args);

    bof::runMocked<>(go, args, len);
    return 0;
}

#elif defined(_GTEST)
#include <gtest\gtest.h>
#endif 
