# ResetWipe

Programmatic Windows factory reset tool that triggers a full "Reset this PC" (Remove Everything) operation from the command line, without requiring user interaction through the Settings UI.

> [!WARNING]
> **Experimental / Research Only**
> This tool was developed using information obtained through **reverse engineering** and utilizes **undocumented Windows internal functions**. 
> - It is intended strictly for research and investigation.
> - There is no guarantee of stability or compatibility.
> - **Use at your own risk.** If it does not work as expected, please do not ask for support, as it relies on undocumented behavior that may change without notice.

## How It Works

Windows includes an undocumented internal library called `ResetEngine.dll` — the **Push Button Reset (PBR)** engine. This is the same engine that powers the "Reset this PC" feature in **Settings > System > Recovery**. The Settings UI (`SystemSettings.exe`) calls into this library through `ResetEngOnline.dll` to orchestrate the reset.

ResetWipe calls the PBR engine directly, bypassing the UI entirely. The reset sequence is:

| Step | API Call | Purpose |
|------|----------|---------|
| 1 | `ResetCreateSession("C:", 0, &session)` | Creates a reset session targeting the OS volume |
| 2 | `ResetPrepareSession(session, &options)` | Configures the reset scenario and validates the environment |
| 3 | `ResetStageOfflineBoot(session)` | Prepares WinRE to execute the reset on next boot |
| 4 | `InitiateSystemShutdownExW(...)` | Reboots into WinRE, which completes the reset |

After the reboot, Windows Recovery Environment (WinRE) takes over and performs the actual reset — reformatting the OS partition and reinstalling Windows from the local recovery image. The machine boots into OOBE (Out-of-Box Experience) when complete.

## Requirements

- **Windows 10 or Windows 11** — `ResetEngine.dll` is present in all standard installations
- **SYSTEM context** — the PBR engine requires SYSTEM-level access to the registry and BCD store. Local Administrator is not sufficient
- **WinRE enabled** — the Windows Recovery Environment must be present and enabled. Verify with `reagentc /info`
- **Local recovery image** — the default WinSxS-based recovery must be intact

## Usage

### Interactive Mode

```
psexec -s -i ResetWipe.exe
```

Displays a warning and prompts the user to type `YES` to confirm.

### Silent Mode (Automation)

```
psexec -s ResetWipe.exe --force
```

Skips the confirmation prompt. Intended for deployment via endpoint management tools.

### Integration with Intune / SCCM

Since ResetWipe requires SYSTEM context, it can be deployed as:

- **Intune Remediation Script** — wrap in a PowerShell script that calls the executable
- **Intune Win32 App** — package as `.intunewin` with `ResetWipe.exe --force` as the install command
- **SCCM Task Sequence** — add as a "Run Command Line" step
- **Scheduled Task** — create a task running as SYSTEM

Example PowerShell wrapper for Intune:

```powershell
# Deploy ResetWipe as Intune remediation
$exePath = "$env:ProgramData\ResetWipe\ResetWipe.exe"
if (Test-Path $exePath) {
    Start-Process -FilePath $exePath -ArgumentList "--force" -Wait -NoNewWindow
}
```

## Build

### Prerequisites

- Visual Studio 2019+ or Build Tools with C++ workload
- CMake 3.15+

### Compile

```bash
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

The output binary will be in `build/Release/ResetWipe.exe`.

### Manual Compile (without CMake)

```bash
cl /O2 /W4 /EHsc src/main.cpp /Fe:ResetWipe.exe ole32.lib oleaut32.lib advapi32.lib
```

## Reset Scenarios

The PBR engine supports multiple reset scenarios configured via the `scenarioType` field:

| ID | Name | Description |
|----|------|-------------|
| 0 | Refresh | Keep personal files, remove apps and settings |
| 1 | **Reset** | **Remove everything** (used by ResetWipe) |
| 2 | SignatureRefresh | Refresh with signature verification |
| 3 | SignatureReset | Reset with signature verification |
| 4 | Reprovision | Reprovision the OS |
| 5 | BareMetal | Bare metal recovery (requires recovery partition) |
| 9 | ProtectedWipe | Protected wipe |
| 10 | CloudBareMetal | Cloud-based bare metal recovery |

### Configuration Options

```cpp
struct ResetOptions {
    DWORD scenarioType;       // See table above
    BYTE  wipeData;           // 1 = delete all user data
    BYTE  overwriteSpace;     // 1 = secure erase (writes zeros, much slower)
    BYTE  preserveWorkplace;  // 1 = keep Azure AD / workplace join
    BYTE  usePayload;         // 1 = use cloud recovery image instead of local
    BYTE  reserved[16];       // Must be zeroed
};
```

## Reverse Engineering Notes

The function signatures were obtained through static analysis of `ResetEngine.dll` (version 10.0.26100.7705) and `ResetEngOnline.dll` using Ghidra. Key findings:

### ResetCreateSession

```
Ordinal 14 | RVA 0x0000A660
```

```cpp
int ResetCreateSession(
    const wchar_t* drivePath,   // Target volume, e.g. L"C:"
    int flags,                  // Pass 0
    void** ppSession            // OUT: opaque session handle
);
```

The session handle is an opaque pointer to an internal `PushButtonReset::Session` object. The object contains a telemetry context at offset `+0xA8` and operation queues for the reset procedure.

### ResetPrepareSession

```
Ordinal 38 | RVA 0x0000CC50
```

```cpp
int ResetPrepareSession(
    void* pSession,             // Session from ResetCreateSession
    ResetOptions* pOptions      // Reset configuration (24 bytes)
);
```

Internally calls `PushButtonReset::Session::Construct` to build the operation queue, then validates the session with `PushButtonReset::Session::Validate`. Logs messages to `C:\$SysReset\Logs\`.

Returns `ERROR_BAD_ENVIRONMENT` (0x0000000A) if WinRE is not enabled or the recovery image is missing.

### ResetStageOfflineBoot

```
Ordinal 50 | RVA 0x0000D690
```

```cpp
int ResetStageOfflineBoot(
    void* pSession              // Prepared session
);
```

Configures the BCD (Boot Configuration Data) store to boot into WinRE on next restart. Serializes the session state to `C:\$SysReset\` so WinRE can resume the operation. Also checks free disk space on the target volume.

### ResetReleaseSession

```
Ordinal 41 | RVA 0x0000D1E0
```

```cpp
int ResetReleaseSession(
    void* pSession              // Session to release
);
```

### Boot Flow

The complete boot flow after `StageOfflineBoot`:

```
ResetWipe.exe (SYSTEM)
    │
    ├── ResetCreateSession
    ├── ResetPrepareSession
    ├── ResetStageOfflineBoot  ──→  Modifies BCD store
    ├── InitiateSystemShutdownExW  ──→  Triggers reboot
    │
    ▼
Windows Recovery Environment (WinRE)
    │
    ├── ResetEngine.exe -ExecOnline  ──→  Reads session from C:\$SysReset\
    ├── Formats OS partition
    ├── Reinstalls Windows from recovery image
    │
    ▼
OOBE (Out-of-Box Experience)
```

### Key Discovery: ArmBootTrigger is NOT Required

Initial reverse engineering suggested that `ResetArmBootTrigger` was needed to configure the WinRE boot entry. Analysis of `ResetEngOnline.dll` revealed that the actual Windows "Reset this PC" flow does **not** call `ArmBootTrigger` — `StageOfflineBoot` handles the BCD configuration internally.

`ResetArmBootTrigger` is used for different recovery scenarios (BMR, System Restore, etc.) and accepts a `triggerType` parameter:

| triggerType | Recovery Action |
|-------------|----------------|
| 0 | Invalid (E_INVALIDARG) |
| 1 | Bare Metal Recovery (Re-image) |
| 2 | System Restore |
| 3 | WinRE menu (Choose an option) |
| 4 | WinRE menu (same as 3) |

None of these trigger the Push Button Reset flow — that's handled entirely by `StageOfflineBoot`.

## Troubleshooting

### ERROR_ACCESS_DENIED (0x80070005)

The process is not running as SYSTEM. Use `psexec -s` or deploy through an endpoint management tool that runs as SYSTEM.

### ERROR_BAD_ENVIRONMENT (0x0000000A)

WinRE is not enabled or the recovery partition is missing. Check with:

```
reagentc /info
```

If disabled, enable it:

```
reagentc /enable
```

### ERROR_NOT_READY (0x80070015)

The session was created but not properly initialized. Ensure you're passing `L"C:"` (not `nullptr`) as the drive path to `ResetCreateSession`.

### StageOfflineBoot succeeds but reset doesn't start after reboot

The BCD store was configured but the reboot didn't target WinRE. Ensure you're using the correct shutdown flags:

```cpp
InitiateSystemShutdownExW(nullptr, nullptr, 0, TRUE, TRUE, 0x20001);
//                                              force  reboot  reason flags
```

### Works in test but not in production

Verify that the target machine has:
1. A valid recovery partition (`reagentc /info`)
2. Sufficient free disk space on C: for the staging operation
3. No Group Policy blocking recovery features (`ResetDisabledByPolicy`)

## License

MIT