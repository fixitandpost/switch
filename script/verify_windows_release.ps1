[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo',
    [string] $ObsPath = 'C:\Program Files\obs-studio\bin\64bit\obs64.exe',
    [string] $MotionModelUrl = $env:SWITCH_MOTION_MODEL_URL,
    [switch] $SkipInstall,
    [switch] $SkipLaunch
)

$ErrorActionPreference = 'Stop'

function Add-KnownToolPath([string] $Path) {
    if ( ( Test-Path -LiteralPath $Path ) -and
         ( $env:Path -split ';' | Where-Object { $_ -ieq $Path } ).Count -eq 0 ) {
        $env:Path = "${Path};${env:Path}"
    }
}

function Require-File([string] $Path, [string] $Description) {
    if ( ! ( Test-Path -LiteralPath $Path ) ) {
        throw "${Description} not found at ${Path}"
    }
}

function Require-Command([string] $Name, [string] $Description) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ( ! $command ) {
        throw "${Description} (${Name}) was not found on PATH."
    }
    return $command
}

function Get-ObsOwnedTopLevelWindows([System.Diagnostics.Process] $Process, [string[]] $WindowTitles) {
    if ( ! $Process -or $Process.HasExited ) {
        return @()
    }

    if ( ! ( 'SwitchReleaseVerify.WindowCloser' -as [type] ) ) {
        Add-Type -Namespace SwitchReleaseVerify -Name WindowCloser -MemberDefinition @'
public delegate bool EnumWindowsProc(System.IntPtr hWnd, System.IntPtr lParam);

[DllImport("user32.dll")]
public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, System.IntPtr lParam);

[DllImport("user32.dll", SetLastError=true)]
public static extern int GetWindowText(System.IntPtr hWnd, System.Text.StringBuilder lpString, int nMaxCount);

[DllImport("user32.dll")]
public static extern uint GetWindowThreadProcessId(System.IntPtr hWnd, out uint lpdwProcessId);

[DllImport("user32.dll")]
public static extern bool IsWindowVisible(System.IntPtr hWnd);

[DllImport("user32.dll")]
public static extern bool PostMessage(System.IntPtr hWnd, uint Msg, System.IntPtr wParam, System.IntPtr lParam);
'@
    }

    $processId = [uint32] $Process.Id
    $matches = New-Object System.Collections.Generic.List[object]
    $callback = [SwitchReleaseVerify.WindowCloser+EnumWindowsProc] {
        param([IntPtr] $hWnd, [IntPtr] $lParam)

        [uint32] $windowProcessId = 0
        [void] [SwitchReleaseVerify.WindowCloser]::GetWindowThreadProcessId($hWnd, [ref] $windowProcessId)
        if ( $windowProcessId -ne $processId -or
             ! [SwitchReleaseVerify.WindowCloser]::IsWindowVisible($hWnd) ) {
            return $true
        }

        $title = New-Object System.Text.StringBuilder 512
        [void] [SwitchReleaseVerify.WindowCloser]::GetWindowText($hWnd, $title, $title.Capacity)
        $windowTitle = $title.ToString()
        if ( $WindowTitles -contains $windowTitle ) {
            $matches.Add([pscustomobject]@{
                Handle = $hWnd
                Title = $windowTitle
            })
        }

        return $true
    }

    [void] [SwitchReleaseVerify.WindowCloser]::EnumWindows($callback, [IntPtr]::Zero)
    return @($matches)
}

function Close-ObsOwnedTopLevelWindows(
    [System.Diagnostics.Process] $Process,
    [string[]] $WindowTitles,
    [int] $TimeoutSeconds = 15
) {
    if ( ! $Process -or $Process.HasExited ) {
        return
    }

    try {
        Add-Type -AssemblyName UIAutomationClient
        Add-Type -AssemblyName UIAutomationTypes
    } catch {
        Write-Host "UI Automation is unavailable; skipping OBS-owned window pre-close."
        return
    }

    $findWindows = {
        if ( ! $Process -or $Process.HasExited ) {
            return @()
        }

        $root = [System.Windows.Automation.AutomationElement]::RootElement
        $pidCondition = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty,
            $Process.Id
        )
        $elements = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $pidCondition)
        $windows = @()
        for ( $index = 0; $index -lt $elements.Count; $index++ ) {
            $element = $elements.Item($index)
            if ( $element.Current.ControlType -ne [System.Windows.Automation.ControlType]::Window ) {
                continue
            }
            if ( $WindowTitles -notcontains $element.Current.Name ) {
                continue
            }
            $windows += $element
        }
        return $windows
    }

    foreach ( $window in (& $findWindows) ) {
        Write-Host "Closing OBS-owned window '$($window.Current.Name)'"
        try {
            $pattern = $window.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern)
            $pattern.Close()
        } catch {
            Write-Host "Could not close OBS-owned window '$($window.Current.Name)': $($_.Exception.Message)"
        }
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ( (Get-Date) -lt $deadline ) {
        $Process.Refresh()
        if ( $Process.HasExited ) {
            return
        }

        if ( (& $findWindows).Count -eq 0 ) {
            return
        }

        Start-Sleep -Milliseconds 500
    }
}

function Close-ObsGracefully([System.Diagnostics.Process[]] $Processes, [int] $TimeoutSeconds = 60) {
    foreach ( $process in $Processes ) {
        if ( ! $process -or $process.HasExited ) {
            continue
        }

        Close-ObsOwnedTopLevelWindows $process @('Switch')
        if ( $process.HasExited ) {
            continue
        }

        Write-Host "Closing OBS normally (pid $($process.Id))"
        $null = $process.CloseMainWindow()
        $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
        while ( (Get-Date) -lt $deadline ) {
            Start-Sleep -Milliseconds 500
            $process.Refresh()
            if ( $process.HasExited ) {
                break
            }
        }

        if ( ! $process.HasExited -and $process.MainWindowHandle -ne [IntPtr]::Zero ) {
            Add-Type -AssemblyName System.Windows.Forms
            if ( ! ( 'SwitchReleaseVerify.User32' -as [type] ) ) {
                Add-Type -Namespace SwitchReleaseVerify -Name User32 -MemberDefinition @'
[DllImport("user32.dll")]
public static extern bool SetForegroundWindow(IntPtr hWnd);

[DllImport("user32.dll")]
public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
'@
            }
            $process.Refresh()
            if ( ! $process.HasExited ) {
                Write-Host "OBS is still open; sending normal Alt+F4 close to the main window"
                [SwitchReleaseVerify.User32]::ShowWindow($process.MainWindowHandle, 9) | Out-Null
                [SwitchReleaseVerify.User32]::SetForegroundWindow($process.MainWindowHandle) | Out-Null
                Start-Sleep -Milliseconds 300
                [System.Windows.Forms.SendKeys]::SendWait('%{F4}')
            }
            $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
            while ( (Get-Date) -lt $deadline ) {
                Start-Sleep -Milliseconds 500
                $process.Refresh()
                if ( $process.HasExited ) {
                    break
                }
            }
        }

        if ( ! $process.HasExited ) {
            throw "OBS did not close after normal close requests within ${TimeoutSeconds}s. Leaving it running instead of force quitting."
        }
    }
}

function Invoke-InstalledMotionModelDownload([string] $PluginDataDir, [string] $Url) {
    $downloader = Join-Path $PluginDataDir 'scripts\download-motion-model.ps1'
    $manifest = Join-Path $PluginDataDir 'models\manifest.json'
    Require-File $downloader 'Installed Motion model downloader'
    Require-File $manifest 'Installed Motion model manifest'

    $downloadArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $downloader, '-PluginDataDir', $PluginDataDir)
    if ( ! [string]::IsNullOrWhiteSpace($Url) ) {
        $downloadArgs += @('-Url', $Url)
    }
    & powershell.exe @downloadArgs
    if ( $LASTEXITCODE -ne 0 ) {
        throw "Motion model downloader failed with exit code ${LASTEXITCODE}"
    }

    $manifestData = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    $model = $manifestData.models[0]
    $modelPath = Join-Path (Join-Path $PluginDataDir 'models') $model.file
    Require-File $modelPath 'Installed Motion AI model'

    $actualHash = (Get-FileHash -LiteralPath $modelPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $expectedHash = $model.sha256.ToLowerInvariant()
    if ( $actualHash -ne $expectedHash ) {
        throw "Installed Motion AI model checksum mismatch. expected=${expectedHash} actual=${actualHash}"
    }
    Write-Host "Verified installed Motion AI model: ${modelPath}"
}

Add-KnownToolPath 'C:\Program Files\CMake\bin'
Add-KnownToolPath 'C:\Program Files\Go\bin'
Add-KnownToolPath 'C:\Program Files (x86)\Inno Setup 6'
Add-KnownToolPath 'C:\Program Files\Inno Setup 6'
Add-KnownToolPath (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6')

$ProjectRoot = Resolve-Path -Path "$PSScriptRoot/.."
$BuildSpec = Get-Content -Path "${ProjectRoot}/buildspec.json" -Raw | ConvertFrom-Json
$ProductName = $BuildSpec.name
$ProductVersion = $BuildSpec.version
$ObsExe = Resolve-Path -Path $ObsPath -ErrorAction Stop
$ObsBinDir = Split-Path -Path $ObsExe.Path
$ObsRoot = Resolve-Path -Path (Join-Path $ObsBinDir '..\..')

Require-File $ObsExe.Path 'OBS executable'
foreach ( $runtimeDll in @('obs.dll', 'obs-frontend-api.dll', 'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Network.dll') ) {
    Require-File (Join-Path $ObsBinDir $runtimeDll) "OBS runtime dependency ${runtimeDll}"
}
Require-Command cmake 'CMake'
Require-Command go 'Go toolchain'
Require-Command iscc 'Inno Setup compiler'

Write-Host "Building ${ProductName} ${ProductVersion} for Windows ${Target} (${Configuration})"
& "${ProjectRoot}/.github/scripts/Build-Windows.ps1" -Target $Target -Configuration $Configuration

Write-Host "Packaging ${ProductName} ${ProductVersion}"
& "${ProjectRoot}/.github/scripts/Package-Windows.ps1" -Target $Target -Configuration $Configuration

$Installer = Join-Path $ProjectRoot "release/${ProductName}-${ProductVersion}-windows-${Target}-Installer.exe"
$Zip = Join-Path $ProjectRoot "release/${ProductName}-${ProductVersion}-windows-${Target}.zip"
$PackageRoot = Join-Path $ProjectRoot 'release/Package'

Require-File $Installer 'Installer'
Require-File $Zip 'Portable package zip'
Require-File (Join-Path $PackageRoot 'obs-plugins/64bit/switch.dll') 'Packaged plugin binary'
Require-File (Join-Path $PackageRoot 'obs-plugins/64bit/onnxruntime.dll') 'Packaged ONNX Runtime DirectML runtime'
Require-File (Join-Path $PackageRoot 'data/obs-plugins/switch/locale/en-US.ini') 'Packaged English locale'
Require-File (Join-Path $PackageRoot 'data/obs-plugins/switch/models/manifest.json') 'Packaged model manifest'
Require-File (Join-Path $PackageRoot 'data/obs-plugins/switch/scripts/download-motion-model.ps1') 'Packaged Motion model downloader'
Require-File (Join-Path $PackageRoot 'data/obs-plugins/switch/remote/switch-remote-helper.exe') 'Packaged remote helper'
Require-File (Join-Path $PackageRoot 'data/obs-plugins/switch/remote/web/index.html') 'Packaged remote web UI'

if ( ! $SkipInstall ) {
    Write-Host "Installing packaged plugin into ${ObsRoot}"
    Close-ObsGracefully -Processes (Get-Process obs64 -ErrorAction SilentlyContinue)
    if ( ! [string]::IsNullOrWhiteSpace($MotionModelUrl) ) {
        $env:SWITCH_MOTION_MODEL_URL = $MotionModelUrl
    }
    $installArgs = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/DIR=`"${ObsRoot}`"")
    $process = Start-Process -FilePath $Installer -ArgumentList $installArgs -PassThru -Wait
    if ( $process.ExitCode -ne 0 ) {
        throw "Installer failed with exit code $($process.ExitCode)"
    }
    Invoke-InstalledMotionModelDownload (Join-Path $ObsRoot 'data\obs-plugins\switch') $MotionModelUrl
}

if ( ! $SkipLaunch ) {
    Write-Host "Launching installed OBS to verify Switch loads"
    $logDir = Join-Path $env:APPDATA 'obs-studio/logs'
    New-Item -ItemType Directory -Force -Path $logDir | Out-Null
    $obs = Start-Process -FilePath $ObsExe.Path -WorkingDirectory (Split-Path -Path $ObsExe.Path) -PassThru
    try {
        $deadline = (Get-Date).AddSeconds(45)
        $matchedLog = $null
        while ( (Get-Date) -lt $deadline ) {
            Start-Sleep -Seconds 2
            $latestLog = Get-ChildItem -Path $logDir -Filter '*.txt' -ErrorAction SilentlyContinue |
                Sort-Object LastWriteTime -Descending |
                Select-Object -First 1
            if ( $latestLog -and ( Select-String -Path $latestLog.FullName -Pattern '\[Switch\] loaded version' -Quiet ) ) {
                $matchedLog = $latestLog.FullName
                break
            }
        }
        if ( ! $matchedLog ) {
            throw 'OBS launched, but the latest logs did not show Switch loading.'
        }
        Write-Host "Verified Switch load in ${matchedLog}"
    } finally {
        if ( ! $obs.HasExited ) {
            Close-ObsGracefully -Processes @($obs) -TimeoutSeconds 60
        }
    }
}

Write-Host "Windows packaged release verification complete."
