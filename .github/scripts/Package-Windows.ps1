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
    throw "Packaging script requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '5.1.0' ) {
    Write-Warning 'The Switch Windows packaging script requires PowerShell 5.1 or newer.'
    exit 2
}

function Add-KnownToolPath([string] $Path) {
    if ( ( Test-Path -LiteralPath $Path ) -and
         ( $env:Path -split ';' | Where-Object { $_ -ieq $Path } ).Count -eq 0 ) {
        $env:Path = "${Path};${env:Path}"
    }
}

Add-KnownToolPath 'C:\Program Files (x86)\Inno Setup 6'
Add-KnownToolPath 'C:\Program Files\Inno Setup 6'
Add-KnownToolPath (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6')

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpecFile = "${ProjectRoot}/buildspec.json"

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path ${BuildSpecFile} -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version

    $OutputName = "${ProductName}-${ProductVersion}-windows-${Target}"

    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip"
        )
    }

    Remove-Item @RemoveArgs

    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.exe"
        )
    }

    Remove-Item @RemoveArgs

    $IsccFile = "${ProjectRoot}/build_${Target}/installer-Windows.generated.iss"

    if ( ! ( Test-Path -Path $IsccFile ) ) {
        throw 'InnoSetup install script not found. Run the build script or the CMake build and install procedures first.'
    }

    $IsccCommand = Get-Command iscc -ErrorAction SilentlyContinue
    if ( ! $IsccCommand ) {
        throw 'Inno Setup compiler (iscc) was not found. Install Inno Setup and rerun packaging.'
    }

    Remove-Item -Path "${ProjectRoot}/release/Package" -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path "${ProjectRoot}/release/Package" | Out-Null
    New-Item -ItemType Directory -Force -Path "${ProjectRoot}/release/Package/obs-plugins" | Out-Null
    New-Item -ItemType Directory -Force -Path "${ProjectRoot}/release/Package/data/obs-plugins/${ProductName}" | Out-Null

    Copy-Item -Path "${ProjectRoot}/release/${Configuration}/${ProductName}/bin/*" -Destination "${ProjectRoot}/release/Package/obs-plugins" -Recurse
    Copy-Item -Path "${ProjectRoot}/release/${Configuration}/${ProductName}/data/*" -Destination "${ProjectRoot}/release/Package/data/obs-plugins/${ProductName}" -Recurse
    Copy-Item "${IsccFile}" -Destination "${ProjectRoot}/release"
    Copy-Item "${ProjectRoot}/media/icon.ico" -Destination "${ProjectRoot}/release"

    Log-Information 'Creating InnoSetup installer...'
    Push-Location -Stack BuildTemp
    Ensure-Location -Path "${ProjectRoot}/release"
    Invoke-External $IsccCommand.Source ${IsccFile} /O. /F"${OutputName}-Installer"
    Pop-Location -Stack BuildTemp

    Log-Group "Archiving ${ProductName}..."
    $CompressArgs = @{
        Path = (Get-ChildItem -Path "${ProjectRoot}/release/${Configuration}" -Exclude "${OutputName}*.*")
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${OutputName}-programdata.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs

    $CompressArgs = @{
        Path = (Get-ChildItem -Path "${ProjectRoot}/release/Package" -Exclude "${OutputName}*.*")
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${OutputName}.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs

    Log-Group
}

Package
