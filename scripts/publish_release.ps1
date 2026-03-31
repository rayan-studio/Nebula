param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [Parameter(Mandatory = $true)]
    [string]$NoteVersion,

    [Parameter(Mandatory = $true)]
    [string]$PortablePath,

    [Parameter(Mandatory = $true)]
    [string]$SetupPath,

    [string]$Cookie = $env:ASTRACODE_ADMIN_COOKIE,
    [string]$Token = $env:ASTRACODE_ADMIN_TOKEN,
    [string]$ApiBase = "https://api.astracode.dev",
    [string]$UploadBase = "https://upload.astracode.dev"
)

$ErrorActionPreference = "Stop"

function Get-VersionFromArtifactPath {
    param([string]$PathValue)

    $normalized = ($PathValue -replace '\\', '/')

    if ($normalized -match '(^|/)Nebula-(?<v>[^/]+)-portable(/|$)') {
        return $Matches['v']
    }

    if ($normalized -match '(^|/)Nebula-(?<v>[^/]+)-setup\.exe$') {
        return $Matches['v']
    }

    return $null
}

function Resolve-RequiredFile {
    param([string]$PathValue, [string]$Label)
    $resolved = Resolve-Path -LiteralPath $PathValue -ErrorAction SilentlyContinue
    if (-not $resolved) {
        throw "$Label file not found: $PathValue"
    }
    return $resolved.Path
}

$authValue = $Cookie
if ([string]::IsNullOrWhiteSpace($authValue)) {
    $authValue = $Token
}

if ([string]::IsNullOrWhiteSpace($authValue)) {
    throw "Missing auth. Pass -Cookie or -Token, or set ASTRACODE_ADMIN_COOKIE / ASTRACODE_ADMIN_TOKEN in your environment."
}

$portableVersion = Get-VersionFromArtifactPath -PathValue $PortablePath
$setupVersion = Get-VersionFromArtifactPath -PathValue $SetupPath

if ($portableVersion -and $portableVersion -ne $Version) {
    throw "Version mismatch: -Version '$Version' but PortablePath points to version '$portableVersion'."
}
if ($setupVersion -and $setupVersion -ne $Version) {
    throw "Version mismatch: -Version '$Version' but SetupPath points to version '$setupVersion'."
}
if ($portableVersion -and $setupVersion -and $portableVersion -ne $setupVersion) {
    throw "Artifacts mismatch: portable version '$portableVersion' differs from setup version '$setupVersion'."
}

$portable = Resolve-RequiredFile -PathValue $PortablePath -Label "Portable"
$setup = Resolve-RequiredFile -PathValue $SetupPath -Label "Setup"
$uploadUrl = "$($UploadBase.TrimEnd('/'))/upload"

Write-Host "Uploading version $Version to $uploadUrl" -ForegroundColor Cyan

$curlArgs = @(
    "-sS",
    "-X", "POST",
    $uploadUrl,
    "-F", "cookie=$authValue",
    "-F", "version=$Version",
    "-F", "note_version=$NoteVersion",
    "-F", "file_portable=@$portable",
    "-F", "file_setup=@$setup"
)

$response = & curl.exe @curlArgs
if ($LASTEXITCODE -ne 0) {
    throw "Upload request failed (curl exit code: $LASTEXITCODE)."
}

try {
    $json = $response | ConvertFrom-Json
} catch {
    Write-Host "Server response was not JSON:" -ForegroundColor Yellow
    Write-Host $response
    throw
}

if ($json.message) {
    Write-Host $json.message -ForegroundColor Green
}

if ($json.version) {
    Write-Host "Uploaded version: $($json.version)" -ForegroundColor Green
}

if ($json.files) {
    if ($json.files.portable) {
        Write-Host "Portable URL: $($ApiBase.TrimEnd('/'))$($json.files.portable)"
    }
    if ($json.files.setup) {
        Write-Host "Setup URL: $($ApiBase.TrimEnd('/'))$($json.files.setup)"
    }
}
