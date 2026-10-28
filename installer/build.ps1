# Builds the installer: installer\Output\AudioLinker-Setup-<version>.exe and its
# SHA-256. The version is read from CMakeLists.txt, the single place it lives.
#
#   .\installer\build.ps1                       # uses build\AudioLinker.exe
#   .\installer\build.ps1 -ExePath X.exe -Sign  # signs with signtool (see below)
#
# -Sign needs signtool.exe on the PATH and the environment variable
# AL_SIGN_ARGS, the arguments between "signtool sign" and the file (certificate,
# timestamp...), e.g. '/f cert.pfx /p secret /tr http://timestamp.digicert.com
# /td sha256 /fd sha256'. The exe itself must be signed beforehand.
param(
    [string]$ExePath = (Join-Path $PSScriptRoot '..\build\AudioLinker.exe'),
    [switch]$Sign
)
$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$cmake = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(AudioLinker VERSION (\d+\.\d+\.\d+)') { throw 'Version introuvable dans CMakeLists.txt' }
$version = $Matches[1]

$exe = (Resolve-Path $ExePath).Path

$iscc = @(
    (Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source,
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 introuvable (https://jrsoftware.org/isinfo.php)' }

$isccArgs = @('/Q', "/DAppVersion=$version", "/DExePath=$exe")
if ($Sign) {
    if (-not $env:AL_SIGN_ARGS) { throw 'AL_SIGN_ARGS est vide' }
    $isccArgs += '/DSignTool'
    # sign.cmd runs signtool with AL_SIGN_ARGS; $q is Inno's escape for a quote, $f the file.
    $isccArgs += "/Ssigntool=`$q$(Join-Path $PSScriptRoot 'sign.cmd')`$q `$f"
}
$isccArgs += (Join-Path $PSScriptRoot 'AudioLinker.iss')

Write-Host "Version $version, exe $exe"
& $iscc @isccArgs
if ($LASTEXITCODE -ne 0) { throw "ISCC a échoué ($LASTEXITCODE)" }

$setup = Join-Path $PSScriptRoot "Output\AudioLinker-Setup-$version.exe"
$hash = (Get-FileHash $setup -Algorithm SHA256).Hash.ToLower()
"$hash *AudioLinker-Setup-$version.exe" | Set-Content -Encoding ascii "$setup.sha256"
Write-Host "OK : $setup ($([math]::Round((Get-Item $setup).Length / 1KB)) Ko)"
Write-Host "SHA-256 : $hash"
