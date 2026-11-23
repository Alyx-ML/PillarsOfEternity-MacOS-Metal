# Import specification for the Win32 runtime (Pillars of Eternity port).
#
# One line per imported function or data object:
#   <dll> <import-name> <kind> <stack-dwords> [<c-name>]
#
# import-name: the name that SRW uses (ordinal imports: ord_<dll>_<n>;
#   names with characters other than [A-Za-z0-9_] are encoded by SRW,
#   see SymbolName in SRW/SRW_loader.c; write them here in the original form).
# kind:
#   std   the C function void rt_<c-name>(_cpu *cpu) reads the arguments
#         from the guest stack and sets the result registers. The thunk then
#         returns to the guest and removes <stack-dwords> argument dwords
#         (callee cleanup, __stdcall and __thiscall). Use 0 for __cdecl.
#   jump  the C function uint32_t rt_<c-name>(_cpu *cpu) sets all guest
#         registers (including esp) and returns the guest address where
#         execution continues. <stack-dwords> must be 0.
#   data  a data object. The runtime defines it with the symbol
#         <encoded-name>_asm2c. <stack-dwords> must be 0.
#   unreachable
#         no translated code calls this import: only code bytes that no
#         code or data refers to (dead code that SRW does not translate)
#         call it. tools/gen_thunks.py writes the C function; a call stops
#         the process with a message.
# c-name: optional; the default is the import name (encoded).
# ---------------------------------------------------------------- KERNEL32

# ---------------------------------------------------------------- ADVAPI32
advapi32 AllocateAndInitializeSid std 11
advapi32 BuildTrusteeWithSidW std 2
advapi32 CopySid std 3
advapi32 CreateProcessWithLogonW std 11
advapi32 CryptAcquireContextW std 5
advapi32 CryptGenRandom std 3
advapi32 CryptReleaseContext std 2
advapi32 DuplicateToken std 3
advapi32 FreeSid std 1
advapi32 GetEffectiveRightsFromAclW std 3
advapi32 GetLengthSid std 1
advapi32 GetNamedSecurityInfoW std 8
advapi32 GetTokenInformation std 5
advapi32 ImpersonateLoggedOnUser std 1
advapi32 LookupAccountSidW std 7
advapi32 OpenProcessToken std 3
advapi32 OpenThreadToken std 4
advapi32 RevertToSelf std 0
advapi32 SetEntriesInAclW std 4
advapi32 SetNamedSecurityInfoW std 7

# ---------------------------------------------------------------- KERNEL32
kernel32 AddVectoredExceptionHandler std 2
kernel32 CloseHandle std 1
kernel32 CompareStringW std 6
kernel32 CopyFileW std 3
kernel32 CreateDirectoryA std 2
kernel32 CreateDirectoryW std 2
kernel32 CreateEventW std 4
kernel32 CreateFileA std 7
kernel32 CreateFileMappingW std 6
kernel32 CreateFileW std 7
kernel32 CreateMutexW std 3
kernel32 CreatePipe std 4
kernel32 CreateProcessA std 10
kernel32 CreateProcessW std 10
kernel32 CreateSemaphoreW std 4
kernel32 CreateThread std 6
kernel32 DebugBreak jump 0
kernel32 DecodePointer std 1
kernel32 DeleteCriticalSection std 1
kernel32 DeleteFileA std 1
kernel32 DeleteFileW std 1
kernel32 DuplicateHandle std 7
kernel32 EncodePointer std 1
kernel32 EnterCriticalSection std 1
kernel32 EnumSystemLocalesA std 2
kernel32 ExitProcess std 1
kernel32 ExitThread jump 0
kernel32 FileTimeToLocalFileTime std 2
kernel32 FileTimeToSystemTime std 2
kernel32 FindClose std 1
kernel32 FindFirstFileExA std 6
kernel32 FindFirstFileExW std 6
kernel32 FindFirstFileW std 2
kernel32 FindNextFileW std 2
kernel32 FlushFileBuffers std 1
kernel32 FormatMessageW std 7
kernel32 FreeConsole std 0
kernel32 FreeEnvironmentStringsW std 1
kernel32 FreeLibrary std 1
kernel32 GetACP std 0
kernel32 GetCommandLineA std 0
kernel32 GetComputerNameW std 2
kernel32 GetConsoleCP std 0
kernel32 GetConsoleMode std 2
kernel32 GetCPInfo std 2
kernel32 GetCurrentDirectoryW std 2
kernel32 GetCurrentProcess std 0
kernel32 GetCurrentProcessId std 0
kernel32 GetCurrentThread std 0
kernel32 GetCurrentThreadId std 0
kernel32 GetDiskFreeSpaceExW std 4
kernel32 GetDriveTypeA std 1
kernel32 GetDriveTypeW std 1
kernel32 GetEnvironmentStringsW std 0
kernel32 GetEnvironmentVariableW std 3
kernel32 GetExitCodeProcess std 2
kernel32 GetExitCodeThread std 2
kernel32 GetFileAttributesA std 1
kernel32 GetFileAttributesExW std 3
kernel32 GetFileAttributesW std 1
kernel32 GetFileInformationByHandle std 2
kernel32 GetFileSize std 2
kernel32 GetFileType std 1
kernel32 GetFullPathNameA std 4
kernel32 GetFullPathNameW std 4
kernel32 GetLastError std 0
kernel32 GetLocaleInfoA std 4
kernel32 GetLocaleInfoW std 4
kernel32 GetLogicalDriveStringsW std 2
kernel32 GetModuleFileNameA std 3
kernel32 GetModuleFileNameW std 3
kernel32 GetModuleHandleW std 1
kernel32 GetOEMCP std 0
kernel32 GetPriorityClass std 1
kernel32 GetProcAddress std 2
kernel32 GetProcessHeap std 0
kernel32 GetProcessTimes std 5
kernel32 GetProcessWorkingSetSize std 3
kernel32 GetStartupInfoW std 1
kernel32 GetStdHandle std 1
kernel32 GetStringTypeW std 4
kernel32 GetSystemDirectoryA std 2
kernel32 GetSystemInfo std 1
kernel32 GetSystemTimeAsFileTime std 1
kernel32 GetTempPathW std 2
kernel32 GetThreadContext std 2
kernel32 GetTickCount std 0
kernel32 GetTimeZoneInformation std 1
kernel32 GetUserDefaultLCID std 0
kernel32 GetVersion std 0
kernel32 GetVersionExW std 1
kernel32 GetVolumeInformationW std 8
kernel32 GlobalAlloc std 2
kernel32 GlobalFree std 1
kernel32 GlobalReAlloc std 3
kernel32 HeapAlloc std 3
kernel32 HeapCreate std 3
kernel32 HeapDestroy std 1
kernel32 HeapFree std 3
kernel32 HeapReAlloc std 4
kernel32 HeapSize std 3
kernel32 InitializeCriticalSection std 1
kernel32 InitializeCriticalSectionAndSpinCount std 2
kernel32 InterlockedCompareExchange std 3
kernel32 InterlockedDecrement std 1
kernel32 InterlockedExchange std 2
kernel32 InterlockedIncrement std 1
kernel32 IsDebuggerPresent std 0
kernel32 IsProcessorFeaturePresent std 1
kernel32 IsValidCodePage std 1
kernel32 IsValidLocale std 2
kernel32 IsWow64Process std 2
kernel32 LCMapStringW std 6
kernel32 LeaveCriticalSection std 1
kernel32 LoadLibraryA std 1
kernel32 LoadLibraryW std 1
kernel32 LocalFileTimeToFileTime std 2
kernel32 LocalFree std 1
kernel32 LockFile std 5
kernel32 MapViewOfFile std 5
kernel32 MoveFileA std 2
kernel32 MoveFileW std 2
kernel32 MultiByteToWideChar std 6
kernel32 OpenEventW std 3
kernel32 OpenMutexW std 3
kernel32 OpenProcess std 3
kernel32 OpenSemaphoreW std 3
kernel32 OpenThread std 3
kernel32 OutputDebugStringW std 1
kernel32 PeekNamedPipe std 6
kernel32 QueryPerformanceCounter std 1
kernel32 QueryPerformanceFrequency std 1
kernel32 QueueUserAPC std 3
kernel32 RaiseException jump 0
kernel32 ReadFile std 5
kernel32 ReleaseMutex std 1
kernel32 ReleaseSemaphore std 3
kernel32 RemoveDirectoryW std 1
kernel32 RemoveVectoredExceptionHandler std 1
kernel32 ReplaceFileW std 6
kernel32 ResetEvent std 1
kernel32 ResumeThread std 1
kernel32 RtlUnwind std 4
kernel32 SetCurrentDirectoryW std 1
kernel32 SetEndOfFile std 1
kernel32 SetEnvironmentVariableA std 2
kernel32 SetEnvironmentVariableW std 2
kernel32 SetErrorMode std 1
kernel32 SetEvent std 1
kernel32 SetFileAttributesW std 2
kernel32 SetFilePointer std 4
kernel32 SetFileTime std 4
kernel32 SetHandleCount std 1
kernel32 SetLastError std 1
kernel32 SetPriorityClass std 2
kernel32 SetProcessWorkingSetSize std 3
kernel32 SetStdHandle std 2
kernel32 SetUnhandledExceptionFilter std 1
kernel32 SignalObjectAndWait std 4
kernel32 Sleep std 1
kernel32 SleepEx std 2
kernel32 SuspendThread std 1
kernel32 SystemTimeToFileTime std 2
kernel32 TerminateProcess std 2
kernel32 TlsAlloc std 0
kernel32 TlsFree std 1
kernel32 TlsGetValue std 1
kernel32 TlsSetValue std 2
kernel32 UnhandledExceptionFilter std 1
kernel32 UnlockFile std 5
kernel32 UnmapViewOfFile std 1
kernel32 VerLanguageNameW std 3
kernel32 VirtualAlloc std 4
kernel32 VirtualFree std 3
kernel32 VirtualProtect std 4
kernel32 VirtualQuery std 3
kernel32 WaitForMultipleObjectsEx std 5
kernel32 WaitForSingleObject std 2
kernel32 WaitForSingleObjectEx std 3
kernel32 WideCharToMultiByte std 8
kernel32 WriteConsoleW std 5
kernel32 WriteFile std 5

# ---------------------------------------------------------------- MSWSOCK
mswsock TransmitFile std 7

# ---------------------------------------------------------------- OLE32
ole32 CoCreateFreeThreadedMarshaler std 2
ole32 CoInitializeEx std 2
ole32 CoTaskMemAlloc std 1
ole32 CoTaskMemFree std 1
ole32 CoTaskMemRealloc std 2
ole32 CoUninitialize std 0

# ---------------------------------------------------------------- OLEAUT32
oleaut32 ord_oleaut32_148 std 3 SafeArrayPtrOfIndex
oleaut32 ord_oleaut32_15 std 3 SafeArrayCreate
oleaut32 ord_oleaut32_16 std 1 SafeArrayDestroy
oleaut32 ord_oleaut32_17 std 1 SafeArrayGetDim
oleaut32 ord_oleaut32_19 std 3 SafeArrayGetUBound
oleaut32 ord_oleaut32_20 std 3 SafeArrayGetLBound
oleaut32 ord_oleaut32_26 std 3 SafeArrayPutElement
oleaut32 ord_oleaut32_4 std 2 SysAllocStringLen
oleaut32 ord_oleaut32_6 std 1 SysFreeString
oleaut32 ord_oleaut32_7 std 1 SysStringLen

# ---------------------------------------------------------------- PSAPI
psapi EnumProcesses std 3
psapi EnumProcessModules std 4
psapi GetModuleBaseNameW std 4
psapi GetModuleFileNameExW std 4
psapi GetModuleInformation std 4

# ---------------------------------------------------------------- SHELL32
shell32 ShellExecuteExW std 1
shell32 SHGetFolderPathW std 5
shell32 SHGetSpecialFolderPathW std 4

# ---------------------------------------------------------------- USER32
user32 FindWindowA std 2
user32 MessageBoxA std 4
user32 SendMessageTimeoutW std 7
user32 WaitForInputIdle std 2

# ---------------------------------------------------------------- VERSION
version GetFileVersionInfoSizeW std 2
version GetFileVersionInfoW std 4
version VerQueryValueW std 4

# ---------------------------------------------------------------- WINMM
winmm timeBeginPeriod std 1
winmm timeEndPeriod std 1
winmm timeGetDevCaps std 2
winmm timeSetEvent std 5

# ---------------------------------------------------------------- WS2_32
ws2_32 ord_ws2_32_1 std 3 ws_accept
ws2_32 ord_ws2_32_10 std 3 ws_ioctlsocket
ws2_32 ord_ws2_32_11 std 1 ws_inet_addr
ws2_32 ord_ws2_32_111 std 0 ws_WSAGetLastError
ws2_32 ord_ws2_32_112 std 1 WSASetLastError
ws2_32 ord_ws2_32_115 std 2 ws_WSAStartup
ws2_32 ord_ws2_32_116 std 0 ws_WSACleanup
ws2_32 ord_ws2_32_12 std 1 ws_inet_ntoa
ws2_32 ord_ws2_32_13 std 2 ws_listen
ws2_32 ord_ws2_32_14 std 1 ws_ntohl
ws2_32 ord_ws2_32_15 std 1 ws_ntohs
ws2_32 ord_ws2_32_151 std 2 ws___WSAFDIsSet
ws2_32 ord_ws2_32_16 std 4 ws_recv
ws2_32 ord_ws2_32_17 std 6 ws_recvfrom
ws2_32 ord_ws2_32_18 std 5 ws_select
ws2_32 ord_ws2_32_19 std 4 ws_send
ws2_32 ord_ws2_32_2 std 3 ws_bind
ws2_32 ord_ws2_32_20 std 6 ws_sendto
ws2_32 ord_ws2_32_21 std 5 ws_setsockopt
ws2_32 ord_ws2_32_22 std 2 ws_shutdown
ws2_32 ord_ws2_32_23 std 3 ws_socket
ws2_32 ord_ws2_32_3 std 1 ws_closesocket
ws2_32 ord_ws2_32_4 std 3 ws_connect
ws2_32 ord_ws2_32_5 std 3 ws_getpeername
ws2_32 ord_ws2_32_51 std 3 ws_gethostbyaddr
ws2_32 ord_ws2_32_52 std 1 ws_gethostbyname
ws2_32 ord_ws2_32_53 std 1 ws_getprotobyname
ws2_32 ord_ws2_32_55 std 2 ws_getservbyname
ws2_32 ord_ws2_32_56 std 2 ws_getservbyport
ws2_32 ord_ws2_32_57 std 2 ws_gethostname
ws2_32 ord_ws2_32_6 std 3 ws_getsockname
ws2_32 ord_ws2_32_7 std 5 ws_getsockopt
ws2_32 ord_ws2_32_8 std 1 ws_htonl
ws2_32 ord_ws2_32_9 std 1 ws_htons
ws2_32 WSAIoctl std 9
ws2_32 WSARecv std 7
ws2_32 WSASend std 7
ws2_32 WSASocketW std 6


# ---------------------------------------------------------------- PillarsOfEternity.exe: KERNEL32
kernel32 CancelIo std 1
kernel32 CreateEventA std 4
kernel32 CreateFileMappingA std 6
kernel32 CreateMutexA std 3
kernel32 CreateSemaphoreA std 4
kernel32 ExpandEnvironmentStringsA std 3
kernel32 FileTimeToDosDateTime std 3
kernel32 FlushConsoleInputBuffer std 1
kernel32 FormatMessageA std 7
kernel32 GetCommandLineW std 0
kernel32 GetCurrentDirectoryA std 2
kernel32 GetDateFormatA std 6
kernel32 GetEnvironmentVariableA std 3
kernel32 GetFileTime std 4
kernel32 GetModuleHandleA std 1
kernel32 GetOverlappedResult std 4
kernel32 GetProcessAffinityMask std 3
kernel32 GetTimeFormatA std 6
kernel32 GetUserDefaultLangID std 0
kernel32 GetVersionExA std 1
kernel32 GetWindowsDirectoryA std 2
kernel32 GlobalLock std 1
kernel32 GlobalMemoryStatus std 1
kernel32 GlobalMemoryStatusEx std 1
kernel32 GlobalUnlock std 1
kernel32 HeapSetInformation std 4
kernel32 HeapValidate std 3
kernel32 HeapWalk std 2
kernel32 InterlockedExchangeAdd std 2
kernel32 IsBadReadPtr std 2
kernel32 MoveFileExW std 3
kernel32 OpenEventA std 3
kernel32 OutputDebugStringA std 1
kernel32 ReadConsoleInputA std 4
kernel32 SetConsoleCtrlHandler std 2
kernel32 SetConsoleMode std 2
kernel32 SetThreadAffinityMask std 2
kernel32 SetThreadPriority std 2
kernel32 TerminateThread std 2
kernel32 TryEnterCriticalSection std 1
kernel32 VerSetConditionMask std 4
kernel32 VerifyVersionInfoW std 4
kernel32 WaitForMultipleObjects std 4
kernel32 WritePrivateProfileStringW std 4
kernel32 lstrcmpiA std 2
kernel32 lstrcpyA std 2
kernel32 lstrcpynA std 3
kernel32 lstrcpynW std 3
kernel32 lstrlenA std 1

# ---------------------------------------------------------------- PillarsOfEternity.exe: ADVAPI32
advapi32 CryptAcquireContextA std 5
advapi32 CryptCreateHash std 5
advapi32 CryptDestroyHash std 1
advapi32 CryptDestroyKey std 1
advapi32 CryptGetHashParam std 5
advapi32 CryptHashData std 4
advapi32 CryptImportKey std 6
advapi32 CryptVerifySignatureA std 6
advapi32 DeregisterEventSource std 1
advapi32 GetUserNameA std 2
advapi32 RegCloseKey std 1
advapi32 RegCreateKeyA std 3
advapi32 RegCreateKeyExW std 9
advapi32 RegDeleteValueA std 2
advapi32 RegOpenKeyExA std 5
advapi32 RegOpenKeyExW std 5
advapi32 RegQueryValueExA std 6
advapi32 RegQueryValueExW std 6
advapi32 RegSetValueExA std 6
advapi32 RegSetValueExW std 6
advapi32 RegisterEventSourceA std 2
advapi32 ReportEventA std 9

# ---------------------------------------------------------------- PillarsOfEternity.exe: VERSION
version GetFileVersionInfoA std 4
version GetFileVersionInfoSizeA std 2
version VerQueryValueA std 4

# ---------------------------------------------------------------- PillarsOfEternity.exe: SHLWAPI
shlwapi PathCanonicalizeW std 2
shlwapi PathFileExistsW std 1
shlwapi PathIsDirectoryW std 1
shlwapi SHDeleteKeyA std 2

# ---------------------------------------------------------------- PillarsOfEternity.exe: SHELL32
shell32 CommandLineToArgvW std 2
shell32 ShellExecuteW std 6

# ---------------------------------------------------------------- PillarsOfEternity.exe: OLE32
ole32 CoCreateGuid std 1
ole32 CoCreateInstance std 5
ole32 CoInitialize std 1
ole32 CoSetProxyBlanket std 8
ole32 StringFromGUID2 std 3

# ---------------------------------------------------------------- PillarsOfEternity.exe: OLEAUT32
oleaut32 ord_oleaut32_2 std 1 SysAllocString
oleaut32 ord_oleaut32_8 std 1 VariantInit
oleaut32 ord_oleaut32_9 std 1 VariantClear
oleaut32 ord_oleaut32_12 std 4 VariantChangeType

# ---------------------------------------------------------------- PillarsOfEternity.exe: WS2_32
ws2_32 freeaddrinfo std 1
ws2_32 getaddrinfo std 4
ws2_32 ord_ws2_32_103 std 5 ws_WSAAsyncGetHostByName
ws2_32 ord_ws2_32_108 std 1 ws_WSACancelAsyncRequest

# ---------------------------------------------------------------- PillarsOfEternity.exe: DNSAPI, IPHLPAPI, WINHTTP
dnsapi DnsFree std 2
dnsapi DnsQuery_A std 6
iphlpapi GetIpAddrTable std 3
winhttp WinHttpGetIEProxyConfigForCurrentUser std 1

# ---------------------------------------------------------------- PillarsOfEternity.exe: USER32
user32 AdjustWindowRectEx std 4
user32 ChangeDisplaySettingsA std 2
user32 CheckDlgButton std 3
user32 ClientToScreen std 2
user32 ClipCursor std 1
user32 CloseClipboard std 0
user32 CopyRect std 2
user32 CreateDialogParamA std 5
user32 CreateDialogParamW std 5
user32 CreateIconIndirect std 1
user32 CreateWindowExW std 12
user32 DefWindowProcW std 4
user32 DestroyCursor std 1
user32 DestroyWindow std 1
user32 DialogBoxParamA std 5
user32 DialogBoxParamW std 5
user32 DispatchMessageA std 1
user32 DispatchMessageW std 1
user32 EmptyClipboard std 0
user32 EnableWindow std 2
user32 EndDialog std 2
user32 EnumDisplayDevicesA std 4
user32 EnumDisplayMonitors std 4
user32 EnumDisplaySettingsA std 3
user32 EnumWindows std 2
user32 GetAncestor std 2
user32 GetAsyncKeyState std 1
user32 GetCaretBlinkTime std 0
user32 GetClientRect std 2
user32 GetClipboardData std 1
user32 GetCursorPos std 1
user32 GetDC std 1
user32 GetDesktopWindow std 0
user32 GetDlgItem std 2
user32 GetFocus std 0
user32 GetForegroundWindow std 0
user32 GetKeyState std 1
user32 GetMessageA std 4
user32 GetMonitorInfoA std 2
user32 GetParent std 1
user32 GetProcessWindowStation std 0
user32 GetRawInputData std 5
user32 GetRawInputDeviceInfoW std 4
user32 GetRawInputDeviceList std 3
user32 GetSystemMetrics std 1
user32 GetThreadDesktop std 1
user32 GetUserObjectInformationA std 5
user32 GetUserObjectInformationW std 5
user32 GetWindowLongA std 2
user32 GetWindowLongW std 2
user32 GetWindowPlacement std 2
user32 GetWindowRect std 2
user32 IsClipboardFormatAvailable std 1
user32 IsDialogMessageW std 2
user32 IsDlgButtonChecked std 2
user32 IsIconic std 1
user32 IsWindowVisible std 1
user32 LoadCursorA std 2
user32 LoadIconA std 2
user32 LoadImageA std 6
user32 MessageBoxW std 4
user32 MonitorFromWindow std 2
user32 MsgWaitForMultipleObjects std 5
user32 OffsetRect std 3
user32 OpenClipboard std 1
user32 PeekMessageA std 5
user32 PeekMessageW std 5
user32 PostQuitMessage std 1
user32 RegisterClassExW std 1
user32 RegisterClassW std 1
user32 RegisterDeviceNotificationW std 3
user32 RegisterRawInputDevices std 3
user32 RegisterWindowMessageA std 1
user32 ReleaseCapture std 0
user32 ReleaseDC std 2
user32 ScreenToClient std 2
user32 SendDlgItemMessageW std 5
user32 SendMessageA std 4
user32 SendMessageTimeoutA std 7
user32 SetCapture std 1
user32 SetClipboardData std 2
user32 SetCursor std 1
user32 SetCursorPos std 2
user32 SetDlgItemTextA std 3
user32 SetDlgItemTextW std 3
user32 SetFocus std 1
user32 SetForegroundWindow std 1
user32 SetWindowLongA std 3
user32 SetWindowLongW std 3
user32 SetWindowPos std 7
user32 SetWindowTextW std 2
user32 ShowCursor std 1
user32 ShowWindow std 2
user32 SystemParametersInfoW std 4
user32 TranslateMessage std 1
user32 UnregisterClassW std 2
user32 UnregisterDeviceNotification std 1
user32 ValidateRect std 2
user32 WindowFromPoint std 2
user32 wsprintfA std 0
user32 wvsprintfA std 3

# ---------------------------------------------------------------- PillarsOfEternity.exe: GDI32
gdi32 ChoosePixelFormat std 2
gdi32 CreateBitmap std 5
gdi32 CreateDIBSection std 6
gdi32 DeleteObject std 1
gdi32 GetDeviceCaps std 2
gdi32 GetObjectA std 3
gdi32 SetPixelFormat std 3
gdi32 SwapBuffers std 1

# ---------------------------------------------------------------- PillarsOfEternity.exe: IMM32
imm32 ImmAssociateContext std 2
imm32 ImmAssociateContextEx std 3
imm32 ImmGetCompositionStringW std 4
imm32 ImmGetContext std 1
imm32 ImmGetConversionStatus std 3
imm32 ImmReleaseContext std 2
imm32 ImmSetCompositionStringW std 6
imm32 ImmSetOpenStatus std 2

# ---------------------------------------------------------------- PillarsOfEternity.exe: HID
hid HidD_FreePreparsedData std 1
hid HidD_GetHidGuid std 1
hid HidD_GetPreparsedData std 2
hid HidD_GetProductString std 3
hid HidP_GetButtonCaps std 4
hid HidP_GetCaps std 2
hid HidP_GetData std 6
hid HidP_GetValueCaps std 4
hid HidP_MaxDataListLength std 2
winmm timeGetTime std 0
winmm waveInAddBuffer std 3
winmm waveInClose std 1
winmm waveInGetDevCapsA std 3
winmm waveInGetDevCapsW std 3
winmm waveInGetNumDevs std 0
winmm waveInOpen std 6
winmm waveInPrepareHeader std 3
winmm waveInReset std 1
winmm waveInStart std 1
winmm waveInUnprepareHeader std 3
winmm waveOutClose std 1
winmm waveOutGetDevCapsA std 3
winmm waveOutGetDevCapsW std 3
winmm waveOutGetNumDevs std 0
winmm waveOutGetPosition std 3
winmm waveOutOpen std 6
winmm waveOutPrepareHeader std 3
winmm waveOutReset std 1
winmm waveOutUnprepareHeader std 3
winmm waveOutWrite std 3
msacm32 acmFormatSuggest std 5
msacm32 acmStreamConvert std 3
msacm32 acmStreamOpen std 8
msacm32 acmStreamPrepareHeader std 3
msacm32 acmStreamSize std 4
msacm32 acmStreamUnprepareHeader std 3
opengl32 glAlphaFunc std 2
opengl32 glBegin std 1
opengl32 glBindTexture std 2
opengl32 glBlendFunc std 2
opengl32 glClear std 1
opengl32 glClearColor std 4
opengl32 glClearDepth std 2
opengl32 glClearStencil std 1
opengl32 glColor4f std 4
opengl32 glColor4fv std 1
opengl32 glColorMask std 4
opengl32 glColorMaterial std 2
opengl32 glColorPointer std 4
opengl32 glCopyTexSubImage2D std 8
opengl32 glCullFace std 1
opengl32 glDeleteTextures std 2
opengl32 glDepthFunc std 1
opengl32 glDepthMask std 1
opengl32 glDisable std 1
opengl32 glDisableClientState std 1
opengl32 glDrawArrays std 3
opengl32 glDrawBuffer std 1
opengl32 glDrawElements std 4
opengl32 glEnable std 1
opengl32 glEnableClientState std 1
opengl32 glEnd std 0
opengl32 glFinish std 0
opengl32 glFogf std 2
opengl32 glFogfv std 2
opengl32 glFogi std 2
opengl32 glFrontFace std 1
opengl32 glGenTextures std 2
opengl32 glGetError std 0
opengl32 glGetFloatv std 2
opengl32 glGetIntegerv std 2
opengl32 glGetString std 1
opengl32 glGetTexParameteriv std 3
opengl32 glHint std 2
opengl32 glIsEnabled std 1
opengl32 glIsTexture std 1
opengl32 glLightModelf std 2
opengl32 glLightModelfv std 2
opengl32 glLightModeli std 2
opengl32 glLightf std 3
opengl32 glLightfv std 3
opengl32 glLoadIdentity std 0
opengl32 glLoadMatrixf std 1
opengl32 glMaterialf std 3
opengl32 glMaterialfv std 3
opengl32 glMatrixMode std 1
opengl32 glMultMatrixf std 1
opengl32 glNormal3f std 3
opengl32 glNormalPointer std 3
opengl32 glPixelStorei std 2
opengl32 glPolygonMode std 2
opengl32 glPolygonOffset std 2
opengl32 glReadBuffer std 1
opengl32 glReadPixels std 7
opengl32 glScissor std 4
opengl32 glStencilFunc std 3
opengl32 glStencilMask std 1
opengl32 glStencilOp std 3
opengl32 glTexCoordPointer std 4
opengl32 glTexEnvf std 3
opengl32 glTexEnvfv std 3
opengl32 glTexEnvi std 3
opengl32 glTexGenfv std 3
opengl32 glTexGeni std 3
opengl32 glTexImage2D std 9
opengl32 glTexParameteri std 3
opengl32 glTexSubImage2D std 9
opengl32 glVertex3f std 3
opengl32 glVertexPointer std 4
opengl32 glViewport std 4
opengl32 wglCreateContext std 1
opengl32 wglDeleteContext std 1
opengl32 wglGetCurrentContext std 0
opengl32 wglGetCurrentDC std 0
opengl32 wglGetProcAddress std 1
opengl32 wglMakeCurrent std 2
opengl32 wglShareLists std 2

# ---------------------------------------------------------------- PillarsOfEternity.exe: D3D9 (rt_d3d9.m)
# The player loads d3d9.dll with LoadLibrary and gets Direct3DCreate9 with
# GetProcAddress. d3d11.dll and dxgi.dll are not runtime DLLs, so the
# player uses Direct3D 9.
d3d9 Direct3DCreate9 std 1
# ---------------------------------------------------------------- PillarsOfEternity.exe: DSOUND (rt_dsound.c)
# FMOD (the audio library of the player) loads dsound.dll with LoadLibrary
# and gets these functions with GetProcAddress. FMOD stops the audio
# initialization when dsound.dll is not available.
dsound DirectSoundCreate std 3
dsound DirectSoundCreate8 std 3
dsound DirectSoundEnumerateW std 2
# ---------------------------------------------------------------- ShroudUnityPlugin.dll: KERNEL32, MSVCR100, MSVCP100
# PillarsOfEternity_data/Plugins/ShroudUnityPlugin.dll (work/shroud) is the
# only module that imports the Visual C++ 2010 runtime (rt_msvcr100.c and
# rt_seh.c). The C functions are __cdecl (0 stack dwords).
kernel32 DisableThreadLibraryCalls std 1
msvcr100 _initterm std 0
msvcr100 _initterm_e std 0
msvcr100 _amsg_exit std 0
msvcr100 __CppXcptFilter std 0
msvcr100 _crt_debugger_hook std 0
msvcr100 _except_handler4_common std 0
msvcr100 __clean_type_info_names_internal std 0
msvcr100 _encoded_null std 0
msvcr100 free std 0 msvcr100_free
msvcr100 _malloc_crt std 0
msvcr100 _onexit std 0
msvcr100 _lock std 0
msvcr100 __dllonexit std 0
msvcr100 _unlock std 0
msvcr100 _CIacos std 0
msvcr100 strstr std 0 msvcr100_strstr
msvcr100 strtod std 0 msvcr100_strtod
msvcr100 strtol std 0 msvcr100_strtol
msvcr100 strtoul std 0 msvcr100_strtoul
msvcr100 strchr std 0 msvcr100_strchr
msvcr100 sprintf_s std 0 msvcr100_sprintf_s
msvcr100 rand std 0 msvcr100_rand
msvcr100 __libm_sse2_cosf std 0
msvcr100 __libm_sse2_sinf std 0
msvcr100 memset std 0 msvcr100_memset
msvcr100 _aligned_free std 0
msvcr100 _aligned_malloc std 0
msvcr100 ??_V@YAXPAX@Z std 0 msvcr100_delete_array
msvcr100 memcpy std 0 msvcr100_memcpy
msvcr100 memmove std 0 msvcr100_memmove
msvcr100 ??2@YAPAXI@Z std 0 msvcr100_new
msvcr100 _purecall std 0
msvcr100 ??3@YAXPAX@Z std 0 msvcr100_delete
msvcr100 _invoke_watson std 0
msvcp100 ?_Xlength_error@std@@YAXPBD@Z std 0 msvcp100_Xlength_error
msvcp100 ?_Xout_of_range@std@@YAXPBD@Z std 0 msvcp100_Xout_of_range
msvcp100 ?_Raise_handler@std@@3P6AXABVexception@stdext@@@ZA data 0
# ---------------------------------------------------------------- PortGraphics.dll: PORTGFX (rt_portgfx.c)
# The port's rows on the Graphics tab of the options window
# (tools/port_graphics/PortGraphics.cs) call these functions with P/Invoke
# (__stdcall, ANSI strings).
portgfx PortGfxGet std 1
portgfx PortGfxSet std 2
portgfx PortGfxFail std 1
# The controller layer (tools/port_graphics/PortController.cs) reads the
# game controller and presses keys with these functions (rt_gamepad.m).
portgfx PortPadRead std 1
portgfx PortPadCapture std 1
portgfx PortPadKey std 2
