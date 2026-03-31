param(
    [Parameter(Mandatory = $true)]
    [string]$Version,

    [string]$NoteVersion,

    [string]$Cookie = $env:ASTRACODE_ADMIN_COOKIE,
    [string]$Token = $env:ASTRACODE_ADMIN_TOKEN,
    [string]$BuildDir = "build",
    [string]$Config = "Release",
    [string]$PythonExe = "python",
    [string]$ApiBase = "https://api.astracode.dev",
    [string]$UploadBase = "https://upload.astracode.dev",
    [string]$EnvFile = ".env",
    [switch]$IncludeRuntime,
    [switch]$IncludePdb,
    [switch]$OnlyUpload,
    [switch]$RememberCookie,
    [switch]$SkipConfigure,
    [switch]$SkipBuild,
    [switch]$SkipPackage,
    [switch]$SkipUpload
)

$ErrorActionPreference = "Stop"

function Get-DotEnvValue {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$Key
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return $null
    }

    $lines = Get-Content -LiteralPath $Path -ErrorAction SilentlyContinue
    foreach ($line in $lines) {
        $trimmed = $line.Trim()
        if ([string]::IsNullOrWhiteSpace($trimmed)) { continue }
        if ($trimmed.StartsWith("#")) { continue }

        if ($trimmed -match '^\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.*)\s*$') {
            $name = $Matches[1]
            $value = $Matches[2].Trim()
            if ($name -ne $Key) { continue }

            if (($value.StartsWith('"') -and $value.EndsWith('"')) -or
                ($value.StartsWith("'") -and $value.EndsWith("'"))) {
                if ($value.Length -ge 2) {
                    $value = $value.Substring(1, $value.Length - 2)
                }
            }
            return $value
        }
    }
    return $null
}

function Resolve-ReleaseAuth {
    param(
        [string]$CookieValue,
        [string]$TokenValue,
        [string]$EnvFilePath
    )

    if (-not [string]::IsNullOrWhiteSpace($CookieValue)) {
        return $CookieValue
    }

    if (-not [string]::IsNullOrWhiteSpace($TokenValue)) {
        return $TokenValue
    }

    if (-not [string]::IsNullOrWhiteSpace($env:ASTRACODE_ADMIN_COOKIE)) {
        return $env:ASTRACODE_ADMIN_COOKIE
    }

    if (-not [string]::IsNullOrWhiteSpace($env:ASTRACODE_ADMIN_TOKEN)) {
        return $env:ASTRACODE_ADMIN_TOKEN
    }

    $fromEnvFile = Get-DotEnvValue -Path $EnvFilePath -Key "ASTRACODE_ADMIN_COOKIE"
    if (-not [string]::IsNullOrWhiteSpace($fromEnvFile)) {
        return $fromEnvFile
    }

    $fromTokenEnvFile = Get-DotEnvValue -Path $EnvFilePath -Key "ASTRACODE_ADMIN_TOKEN"
    if (-not [string]::IsNullOrWhiteSpace($fromTokenEnvFile)) {
        return $fromTokenEnvFile
    }

    $fromEnvLocal = Get-DotEnvValue -Path ".env.local" -Key "ASTRACODE_ADMIN_COOKIE"
    if (-not [string]::IsNullOrWhiteSpace($fromEnvLocal)) {
        return $fromEnvLocal
    }

    $fromTokenEnvLocal = Get-DotEnvValue -Path ".env.local" -Key "ASTRACODE_ADMIN_TOKEN"
    if (-not [string]::IsNullOrWhiteSpace($fromTokenEnvLocal)) {
        return $fromTokenEnvLocal
    }

    return $null
}

function Invoke-External {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Label,
        [Parameter(Mandatory = $true)]
        [scriptblock]$Command
    )

    Write-Host "==> $Label" -ForegroundColor Cyan
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed (exit code: $LASTEXITCODE)."
    }
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Resolve-Path (Join-Path $scriptDir "..")

Push-Location $projectRoot
try {
    if ([string]::IsNullOrWhiteSpace($NoteVersion)) {
        $NoteVersion = "Release $Version"
    }

    if ($OnlyUpload) {
        $SkipConfigure = $true
        $SkipBuild = $true
        $SkipPackage = $true
    }

    if (-not $SkipConfigure) {
        Invoke-External -Label "Configure CMake ($Version)" -Command {
            & cmake "-S" "." "-B" $BuildDir "-DNEBULA_APP_VERSION=$Version"
        }
    }

    if (-not $SkipBuild) {
        Invoke-External -Label "Build ($Config)" -Command {
            & cmake "--build" $BuildDir "--config" $Config
        }
    }

    if (-not $SkipPackage) {
        $packageArgs = @(
            "scripts/package.py",
            "--version", $Version,
            "--build-dir", $BuildDir,
            "--config", $Config
        )
        if ($IncludeRuntime) { $packageArgs += "--include-runtime" }
        if ($IncludePdb) { $packageArgs += "--include-pdb" }

        Invoke-External -Label "Package artifacts" -Command {
            & $PythonExe @packageArgs
        }
    }

    $portablePath = Join-Path "dist" "Nebula-$Version-portable\Nebula.exe"
    $setupPath = Join-Path "dist" "Nebula-$Version-setup.exe"

    if (-not (Test-Path -LiteralPath $portablePath)) {
        throw "Portable artifact not found: $portablePath"
    }
    if (-not (Test-Path -LiteralPath $setupPath)) {
        throw "Setup artifact not found: $setupPath"
    }

    if (-not $SkipUpload) {
        $resolvedAuth = Resolve-ReleaseAuth -CookieValue $Cookie -TokenValue $Token -EnvFilePath $EnvFile
        if ([string]::IsNullOrWhiteSpace($resolvedAuth)) {
            $resolvedAuth = Read-Host "ASTRACODE_ADMIN_COOKIE / ASTRACODE_ADMIN_TOKEN"
        }
        if ([string]::IsNullOrWhiteSpace($resolvedAuth)) {
            throw "Missing auth. Pass -Cookie or -Token, set ASTRACODE_ADMIN_COOKIE/ASTRACODE_ADMIN_TOKEN, or put one in .env/.env.local."
        }

        if ($RememberCookie) {
            [Environment]::SetEnvironmentVariable("ASTRACODE_ADMIN_COOKIE", $resolvedAuth, "User")
            Write-Host "Auth saved in user environment variable ASTRACODE_ADMIN_COOKIE." -ForegroundColor Green
        }

        Write-Host "==> Upload release" -ForegroundColor Cyan
        & (Join-Path $scriptDir "publish_release.ps1") `
            -Version $Version `
            -NoteVersion $NoteVersion `
            -PortablePath $portablePath `
            -SetupPath $setupPath `
            -Cookie $resolvedAuth `
            -ApiBase $ApiBase `
            -UploadBase $UploadBase

        if ($LASTEXITCODE -ne 0) {
            throw "Upload release failed (exit code: $LASTEXITCODE)."
        }
    }
    else {
        Write-Host "==> Skip upload (artifacts ready in dist/)" -ForegroundColor Yellow
    }

    Write-Host "Release pipeline completed for version $Version" -ForegroundColor Green
}
finally {
    Pop-Location
}
