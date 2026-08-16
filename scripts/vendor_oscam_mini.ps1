$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Dst = Join-Path $Root "third_party\oscam-mini"
$Tmp = Join-Path $env:TEMP ("oscam-vendor-" + [guid]::NewGuid().ToString("N"))
try {
    git clone --depth 1 https://github.com/gfto/oscam.git $Tmp
    if (Test-Path $Dst) { Remove-Item -Recurse -Force $Dst }
    New-Item -ItemType Directory -Force -Path $Dst | Out-Null
    Get-ChildItem -Force $Tmp | Where-Object { $_.Name -ne ".git" } | Copy-Item -Destination $Dst -Recurse -Force
    "https://github.com/gfto/oscam.git" | Set-Content -Encoding ascii (Join-Path $Dst "UPSTREAM_URL")
    (git -C $Tmp rev-parse HEAD) | Set-Content -Encoding ascii (Join-Path $Dst "UPSTREAM_COMMIT")
    Write-Host "Vendored OSCam source into $Dst"
}
finally {
    if (Test-Path $Tmp) { Remove-Item -Recurse -Force $Tmp }
}
