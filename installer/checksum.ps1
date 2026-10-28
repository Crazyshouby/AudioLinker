# Writes <installer>.sha256 (SHA-256, binary marker, Unix line ending so that
# `sha256sum -c` accepts it on any system). Called by build.ps1, and again by
# the CI after the installer has been signed: the hash must be the one of the
# file that is published.
param(
    [Parameter(Mandatory = $true)][string]$Setup
)
$ErrorActionPreference = 'Stop'
$Setup = (Resolve-Path $Setup).Path
$hash = (Get-FileHash $Setup -Algorithm SHA256).Hash.ToLower()
[IO.File]::WriteAllText("$Setup.sha256", "$hash *$(Split-Path $Setup -Leaf)`n")
Write-Host "SHA-256 : $hash"
