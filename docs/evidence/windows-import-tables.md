# Windows import tables: what the shipped binaries actually need

Generated from the v0.1.1 Windows package by reading the PE import tables of the
binaries in it. The method is the one in `.github/scripts/check_import_floor.py`,
which the CI now runs on every Windows build.

The point of this file is that a documented minimum client version and the
kernel32 export table are not the same thing. `SetThreadDescription` is
documented as available from Windows 10 1607, and a real 1607 kernel32
(10.0.14393.1715) does not export it -- the documentation describes the
KernelBase and apiset layers, and kernel32 starts forwarding the export later.

## The floor of the shipped v0.1.2 package

Measured on the published `openSCRT-0.1.2-windows-x64-portable.zip` with the same
script the CI runs:

```
$ uv run python .github/scripts/check_import_floor.py --floor 1607 \
    --strict .cache/v012/openscrt-0.1.2-windows-x64/bin/openscrt.exe \
    --report .cache/v012/openscrt-0.1.2-windows-x64/bin/Qt6Core.dll
declared floor: Windows 10.0 build 14393
  openscrt.exe: needs 10.0 build 14393  [ok]
  Qt6Core.dll:  needs 10.0 build 16299
package floor: Windows 10.0 build 16299
```

*这份输出取自 2026-10-24 实测的 v0.1.2 包；上面的命令已按规则 2 改写成 uv 形式
（原跑法是系统 `python3`）。脚本只读 PE 文件，换不换解释器不改变结果。*

**Our own executable now needs 1607** -- the three ConPTY entry points are resolved at
run time, so they are no longer in the import table. The package as a whole still needs
**1709**, because `Qt6Core.dll` imports `SetThreadDescription`, and that symbol is not
ours to remove.

## The floor of the current build

The same gate runs on every Windows build, so the number below is the live one, not a
snapshot of one release. From run #40118988731 (Windows job 120946214460, commit
`2136d0c`):

```
declared floor: Windows 10.0 build 14393
  openscrt.exe: needs 10.0 build 14393  [ok]
  Qt6Core.dll:  needs 10.0 build 16299
  Qt6Gui.dll:   needs 10.0 build 14393
  Qt6Widgets.dll: needs 10.0 build 14393
  Qt6Network.dll: needs 10.0 build 14393
package floor: Windows 10.0 build 16299
```

Two floors, and the release notes promise the looser one: the **hard** floor of the
shipped package is **1709** (build 16299, from `Qt6Core.dll`), while the **supported**
baseline is **1809** (build 17763). The gap is deliberate -- 1709 comes from a
symbol-to-build table, not from a machine anyone ran this on, and Qt 6.8 supports
1809 and later. The CI gate now enforces both: our own binaries may not exceed 1607,
and the package may not exceed 1809 (`--package-floor 1809`).

## The floor, per binary

This table is the **v0.1.1** package, kept because it is the one that shows the
ConPTY symbols in the import table before they were moved to run-time resolution.

| Binary | kernel32 imports | Newest Windows it needs |
| --- | --- | --- |
| `openscrt.exe` | 34 | 10.0 build 17763 |
| `Qt6Core.dll` | 160 | 10.0 build 16299 |
| `Qt6Gui.dll` | 36 | 10.0 build 14393 |
| `Qt6Widgets.dll` | 22 | 10.0 build 14393 |
| `Qt6Network.dll` | 37 | 10.0 build 14393 |

In the v0.1.1 package, our own executable needs **1809** and the Qt libraries need
**1709**, for one symbol between them. Everything else is 1607 or older. Since the
ConPTY calls are now resolved at run time, our executable's floor is 1607 as well, and
the only thing between the package and a 1607 machine is Qt's `SetThreadDescription`.

## Imports newer than Windows 10 1607

| Symbol | Introduced | Imported by |
| --- | --- | --- |
| `ClosePseudoConsole` | 10.0 build 17763 | openscrt |
| `CreatePseudoConsole` | 10.0 build 17763 | openscrt |
| `ResizePseudoConsole` | 10.0 build 17763 | openscrt |
| `SetThreadDescription` | 10.0 build 16299 | Qt6Core |

`CreatePseudoConsole`, `ResizePseudoConsole` and `ClosePseudoConsole` were
imported by our own executable. They are resolved at run time now, so our
executable's floor is 1607 as well.

`SetThreadDescription` is imported by Qt6Core.dll and is not ours to remove.
A Qt build without it would put the package's floor at 1607; the Qt version
matrix question is whether one exists.

## Every kernel32 import, for cross-checking

### `openscrt.exe`

- `AcquireSRWLockExclusive`
- `CloseHandle`
- `ClosePseudoConsole` -- needs 10.0 build 17763
- `CreatePipe`
- `CreateProcessW`
- `CreatePseudoConsole` -- needs 10.0 build 17763
- `DeleteProcThreadAttributeList`
- `GetConsoleProcessList`
- `GetConsoleWindow`
- `GetCurrentProcess`
- `GetCurrentProcessId`
- `GetCurrentThreadId`
- `GetModuleHandleW`
- `GetSystemTimeAsFileTime`
- `GetTickCount64`
- `InitializeProcThreadAttributeList`
- `InitializeSListHead`
- `IsDebuggerPresent`
- `IsProcessorFeaturePresent`
- `QueryPerformanceCounter`
- `ReadFile`
- `ReleaseSRWLockExclusive`
- `ResizePseudoConsole` -- needs 10.0 build 17763
- `RtlCaptureContext`
- `RtlLookupFunctionEntry`
- `RtlVirtualUnwind`
- `SetUnhandledExceptionFilter`
- `SleepConditionVariableSRW`
- `TerminateProcess`
- `UnhandledExceptionFilter`
- `UpdateProcThreadAttribute`
- `WaitForSingleObject`
- `WakeAllConditionVariable`
- `WriteFile`

### `Qt6Core.dll`

- `AcquireSRWLockExclusive`
- `AllocConsole`
- `AttachConsole`
- `CancelIoEx`
- `CloseHandle`
- `CloseThreadpoolWait`
- `CompareStringEx`
- `CompareStringW`
- `ConnectNamedPipe`
- `CopyFileW`
- `CreateDirectoryW`
- `CreateEventW`
- `CreateFileMappingW`
- `CreateFileW`
- `CreateMutexW`
- `CreateNamedPipeW`
- `CreateProcessW`
- `CreateSemaphoreW`
- `CreateThread`
- `CreateThreadpoolWait`
- `DeleteFileW`
- `DeviceIoControl`
- `DisableThreadLibraryCalls`
- `DuplicateHandle`
- `FileTimeToSystemTime`
- `FindClose`
- `FindCloseChangeNotification`
- `FindFirstChangeNotificationW`
- `FindFirstFileExW`
- `FindFirstFileW`
- `FindNextChangeNotification`
- `FindNextFileW`
- `FlushFileBuffers`
- `FormatMessageW`
- `FreeConsole`
- `FreeEnvironmentStringsW`
- `FreeLibrary`
- `GetACP`
- `GetCommandLineW`
- `GetComputerNameExW`
- `GetConsoleWindow`
- `GetCurrencyFormatW`
- `GetCurrentDirectoryW`
- `GetCurrentPackageFullName`
- `GetCurrentProcess`
- `GetCurrentProcessId`
- `GetCurrentThread`
- `GetCurrentThreadId`
- `GetDateFormatW`
- `GetDiskFreeSpaceExW`
- `GetDriveTypeW`
- `GetEnvironmentStringsW`
- `GetExitCodeProcess`
- `GetFileAttributesExW`
- `GetFileAttributesW`
- `GetFileInformationByHandle`
- `GetFileInformationByHandleEx`
- `GetFileType`
- `GetFullPathNameW`
- `GetGeoInfoW`
- `GetLastError`
- `GetLocalTime`
- `GetLocaleInfoW`
- `GetLogicalDrives`
- `GetLongPathNameW`
- `GetModuleFileNameW`
- `GetModuleHandleExW`
- `GetModuleHandleW`
- `GetNativeSystemInfo`
- `GetOverlappedResult`
- `GetProcAddress`
- `GetProcessHeap`
- `GetProcessId`
- `GetStartupInfoW`
- `GetStdHandle`
- `GetSystemDirectoryW`
- `GetSystemInfo`
- `GetSystemTime`
- `GetSystemTimeAsFileTime`
- `GetTempPathW`
- `GetThreadPriority`
- `GetTimeFormatW`
- `GetTimeZoneInformation`
- `GetUserDefaultLCID`
- `GetUserGeoID`
- `GetUserPreferredUILanguages`
- `GetVolumeInformationW`
- `GetVolumeNameForVolumeMountPointW`
- `GetVolumePathNameW`
- `GetVolumePathNamesForVolumeNameW`
- `HeapAlloc`
- `HeapFree`
- `InitializeSListHead`
- `InterlockedFlushSList`
- `InterlockedPushEntrySList`
- `IsDebuggerPresent`
- `IsProcessorFeaturePresent`
- `K32GetModuleFileNameExW`
- `LCMapStringW`
- `LoadLibraryExW`
- `LoadLibraryW`
- `LocalAlloc`
- `LocalFree`
- `LocaleNameToLCID`
- `MapViewOfFile`
- `MoveFileExW`
- `MoveFileW`
- `MultiByteToWideChar`
- `OpenFileMappingW`
- `OpenProcess`
- `OutputDebugStringW`
- `PeekNamedPipe`
- `QueryPerformanceCounter`
- `ReadFile`
- `ReleaseMutex`
- `ReleaseSRWLockExclusive`
- `ReleaseSemaphore`
- `RemoveDirectoryW`
- `ResetEvent`
- `ResumeThread`
- `RtlCaptureContext`
- `RtlLookupFunctionEntry`
- `RtlVirtualUnwind`
- `SetCurrentDirectoryW`
- `SetEndOfFile`
- `SetErrorMode`
- `SetEvent`
- `SetFileInformationByHandle`
- `SetFilePointer`
- `SetFilePointerEx`
- `SetFileTime`
- `SetLastError`
- `SetThreadDescription` -- needs 10.0 build 16299
- `SetThreadPriority`
- `SetThreadpoolWait`
- `SetUnhandledExceptionFilter`
- `Sleep`
- `SleepConditionVariableSRW`
- `SwitchToThread`
- `SystemTimeToFileTime`
- `TerminateProcess`
- `TerminateThread`
- `TlsAlloc`
- `TlsFree`
- `TlsGetValue`
- `TlsSetValue`
- `TzSpecificLocalTimeToSystemTime`
- `UnhandledExceptionFilter`
- `UnmapViewOfFile`
- `VirtualAlloc`
- `VirtualFree`
- `VirtualQuery`
- `WaitForMultipleObjects`
- `WaitForMultipleObjectsEx`
- `WaitForSingleObject`
- `WaitForSingleObjectEx`
- `WaitForThreadpoolWaitCallbacks`
- `WakeAllConditionVariable`
- `WideCharToMultiByte`
- `WriteFile`

### `Qt6Gui.dll`

- `AcquireSRWLockExclusive`
- `AcquireSRWLockShared`
- `CloseHandle`
- `CreateEventW`
- `DeleteCriticalSection`
- `DisableThreadLibraryCalls`
- `EnterCriticalSection`
- `GetConsoleWindow`
- `GetCurrentProcess`
- `GetCurrentProcessId`
- `GetCurrentThreadId`
- `GetLastError`
- `GetLongPathNameW`
- `GetProcAddress`
- `GetSystemTimeAsFileTime`
- `GetUserDefaultLangID`
- `GetUserDefaultLocaleName`
- `InitializeCriticalSection`
- `InitializeSListHead`
- `InitializeSRWLock`
- `IsDebuggerPresent`
- `IsProcessorFeaturePresent`
- `LeaveCriticalSection`
- `QueryPerformanceCounter`
- `ReleaseSRWLockExclusive`
- `ReleaseSRWLockShared`
- `RtlCaptureContext`
- `RtlLookupFunctionEntry`
- `RtlVirtualUnwind`
- `SetUnhandledExceptionFilter`
- `SleepConditionVariableSRW`
- `TerminateProcess`
- `UnhandledExceptionFilter`
- `WaitForSingleObject`
- `WaitForSingleObjectEx`
- `WakeAllConditionVariable`

### `Qt6Widgets.dll`

- `AcquireSRWLockExclusive`
- `DisableThreadLibraryCalls`
- `GetCurrentProcess`
- `GetCurrentProcessId`
- `GetCurrentThreadId`
- `GetDriveTypeW`
- `GetSystemTimeAsFileTime`
- `GetVolumeInformationW`
- `InitializeSListHead`
- `IsDebuggerPresent`
- `IsProcessorFeaturePresent`
- `QueryPerformanceCounter`
- `ReleaseSRWLockExclusive`
- `RtlCaptureContext`
- `RtlLookupFunctionEntry`
- `RtlVirtualUnwind`
- `SetUnhandledExceptionFilter`
- `Sleep`
- `SleepConditionVariableSRW`
- `TerminateProcess`
- `UnhandledExceptionFilter`
- `WakeAllConditionVariable`

### `Qt6Network.dll`

- `AcquireSRWLockExclusive`
- `CloseHandle`
- `ConnectNamedPipe`
- `CreateEventW`
- `CreateFileW`
- `CreateNamedPipeW`
- `DisableThreadLibraryCalls`
- `DisconnectNamedPipe`
- `FormatMessageW`
- `GetCurrentProcess`
- `GetCurrentProcessId`
- `GetCurrentThreadId`
- `GetLastError`
- `GetOverlappedResult`
- `GetSystemTimeAsFileTime`
- `GlobalFree`
- `InitializeSListHead`
- `IsDebuggerPresent`
- `IsProcessorFeaturePresent`
- `QueryPerformanceCounter`
- `ReleaseSRWLockExclusive`
- `ResetEvent`
- `RtlCaptureContext`
- `RtlLookupFunctionEntry`
- `RtlVirtualUnwind`
- `SetEvent`
- `SetLastError`
- `SetUnhandledExceptionFilter`
- `SleepConditionVariableSRW`
- `SleepEx`
- `TerminateProcess`
- `UnhandledExceptionFilter`
- `WaitForMultipleObjects`
- `WaitForMultipleObjectsEx`
- `WaitForSingleObject`
- `WaitNamedPipeW`
- `WakeAllConditionVariable`

