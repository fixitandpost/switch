[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $PluginDataDir,
    [string] $Manifest,
    [string] $Url = $env:SWITCH_MOTION_MODEL_URL,
    [switch] $Force
)

$ErrorActionPreference = 'Stop'

if ( [string]::IsNullOrWhiteSpace($Manifest) ) {
    $Manifest = Join-Path $PluginDataDir 'models\manifest.json'
}

if ( ! ( Test-Path -LiteralPath $Manifest ) ) {
    throw "Motion model manifest not found: ${Manifest}"
}

$manifestData = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
if ( ! $manifestData.models -or $manifestData.models.Count -lt 1 ) {
    throw "Motion model manifest has no model entries: ${Manifest}"
}

$model = $manifestData.models[0]
if ( [string]::IsNullOrWhiteSpace($model.file) -or [string]::IsNullOrWhiteSpace($model.sha256) ) {
    throw 'Motion model manifest is missing file or sha256.'
}

if ( [string]::IsNullOrWhiteSpace($Url) -and ! [string]::IsNullOrWhiteSpace($model.downloadUrl) ) {
    $Url = $model.downloadUrl
}

if ( [string]::IsNullOrWhiteSpace($Url) ) {
    throw 'Motion model download URL is required. Set SWITCH_MOTION_MODEL_URL or provide downloadUrl in the manifest.'
}

$modelDir = Join-Path $PluginDataDir 'models'
$destination = Join-Path $modelDir $model.file
$temporary = "${destination}.download"
New-Item -ItemType Directory -Force -Path $modelDir | Out-Null

if ( ( Test-Path -LiteralPath $destination ) -and ! $Force ) {
    $existingHash = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
    if ( $existingHash -eq $model.sha256.ToLowerInvariant() ) {
        Write-Host "Switch Motion model already installed: ${destination}"
        exit 0
    }
    throw "Existing Motion model checksum mismatch at ${destination}. Re-run with -Force to replace it."
}

Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
Write-Host "Downloading Switch Motion model to ${destination}"
Invoke-WebRequest -Uri $Url -OutFile $temporary -UseBasicParsing

$actualHash = (Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash.ToLowerInvariant()
$expectedHash = $model.sha256.ToLowerInvariant()
if ( $actualHash -ne $expectedHash ) {
    Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
    throw "Motion model checksum mismatch. expected=${expectedHash} actual=${actualHash}"
}

Move-Item -LiteralPath $temporary -Destination $destination -Force
Write-Host "Installed Switch Motion model: ${destination}"
