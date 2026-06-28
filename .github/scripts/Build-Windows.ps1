[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "A 64-bit system is required to build the project."
}

if ( $PSVersionTable.PSVersion -lt '5.1.0' ) {
    Write-Warning 'The Switch Windows build script requires PowerShell 5.1 or newer.'
    exit 2
}

function Add-KnownToolPath([string] $Path) {
    if ( ( Test-Path -LiteralPath $Path ) -and
         ( $env:Path -split ';' | Where-Object { $_ -ieq $Path } ).Count -eq 0 ) {
        $env:Path = "${Path};${env:Path}"
    }
}

Add-KnownToolPath 'C:\Program Files\CMake\bin'
Add-KnownToolPath 'C:\Program Files\Go\bin'

$CmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ( ! $CmakeCommand ) {
    throw 'CMake was not found. Install CMake before building Switch.'
}

function Build {
    trap {
        Pop-Location -Stack BuildTemp -ErrorAction 'SilentlyContinue'
        Write-Error $_
        Log-Group
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach($Utility in $UtilityFunctions) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    Push-Location -Stack BuildTemp
    Ensure-Location $ProjectRoot

    $BuildSpec = Get-Content -Path "${ProjectRoot}/buildspec.json" -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name

    $CmakeArgs = @('--preset', "windows-ci-${Target}")
    $CmakeBuildArgs = @('--build')
    $CmakeInstallArgs = @()

    if ( $DebugPreference -eq 'Continue' ) {
        $CmakeArgs += ('--debug-output')
        $CmakeBuildArgs += ('--verbose')
        $CmakeInstallArgs += ('--verbose')
    }

    $CmakeBuildArgs += @(
        '--preset', "windows-${Target}"
        '--config', $Configuration
        '--parallel'
        '--', '/consoleLoggerParameters:Summary', '/noLogo'
    )

    $CmakeInstallArgs += @(
        '--install', "build_${Target}"
        '--prefix', "${ProjectRoot}/release/${Configuration}"
        '--config', $Configuration
    )

    Log-Group "Configuring ${ProductName}..."
    Invoke-External $CmakeCommand.Source @CmakeArgs

    Log-Group "Building ${ProductName}..."
    Invoke-External $CmakeCommand.Source @CmakeBuildArgs

    Log-Group "Installing ${ProductName}..."
    Invoke-External $CmakeCommand.Source @CmakeInstallArgs

    Pop-Location -Stack BuildTemp
    Log-Group
}

Build
