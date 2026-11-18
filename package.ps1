param(
    [string]$Config = "Release"
)
# Packages SoundX: builds the app, refreshes dist\SoundX.exe, rebuilds the installer so it
# embeds the fresh payload and copies it to dist\SoundX-<version>-Setup.exe
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

# the version label lives in CMakeLists.txt so the app, the installer and this file name agree
$label = (Select-String -Path "$root\CMakeLists.txt" -Pattern 'set\(SOUNDX_VERSION_LABEL\s+"([^"]+)"\)' |
          Select-Object -First 1).Matches.Groups[1].Value
if ([string]::IsNullOrWhiteSpace($label)) { $label = "0.03Alpha" }

& "$root\build.ps1" -Config $Config
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Copy-Item "$root\build\SoundX_artefacts\$Config\SoundX.exe" "$root\dist\SoundX.exe" -Force

# the installer also ships the app-local C++ runtime (build.ps1 refreshes dist\runtime); shipping
# without it would leave other machines with "MSVCP140.dll was not found" on first start
$runtime = "$root\dist\runtime"
$neededRuntime = @("msvcp140.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll")
$missing = $neededRuntime | Where-Object { -not (Test-Path (Join-Path $runtime $_)) }
if ($missing) { Write-Error "the C++ runtime files are missing from dist\runtime: $($missing -join ', ')"; exit 1 }

# the installer embeds dist\SoundX.exe. The generated BinaryData*.cpp files inline the bytes but
# JUCE only regenerates them when the *file list* changes, so a new payload would silently keep
# the old bytes (the user then installs an old program, or a truncated one). Dropping the
# generated sources makes the next build re-read the payload.
Get-ChildItem "$root\build\juce_binarydata_SoundXPayload\JuceLibraryCode" -Filter "BinaryData*.cpp" -ErrorAction SilentlyContinue |
    Remove-Item -Force
& "$root\build.ps1" -Config $Config
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$setup = "$root\build\SoundXSetup_artefacts\$Config\SoundX Setup.exe"
if (-not (Test-Path $setup)) { Write-Error "installer was not built: $setup"; exit 1 }

$installerName = "SoundX-$label-Setup.exe"
Copy-Item $setup "$root\dist\$installerName" -Force

# end-to-end check: install into a scratch folder and make sure the installed program is byte
# identical to the freshly built one (a stale or truncated payload would surface as a crash or
# "not a valid Win32 application" on the user's machine)
$scratch = Join-Path ([System.IO.Path]::GetTempPath()) ("soundx-package-check-" + [guid]::NewGuid().ToString("N"))
try {
    $setupProc = Start-Process -FilePath "$root\dist\$installerName" -ArgumentList @("--silent", "`"$scratch`"") -Wait -PassThru
    if ($setupProc.ExitCode -ne 0) { Write-Error "the installer failed its silent check (exit $($setupProc.ExitCode))"; exit 1 }
    $rebuilt = "$root\build\SoundX_artefacts\$Config\SoundX.exe"
    $installed = Join-Path $scratch "SoundX.exe"
    if (-not (Test-Path $installed)) { Write-Error "the installer did not write SoundX.exe"; exit 1 }
    $want = (Get-FileHash $rebuilt -Algorithm SHA256).Hash
    $got = (Get-FileHash $installed -Algorithm SHA256).Hash
    if ($want -ne $got) { Write-Error "the installer embedded a stale payload (expected $want, got $got)"; exit 1 }
    foreach ($dll in $neededRuntime) {
        if (-not (Test-Path (Join-Path $scratch $dll))) { Write-Error "the installer did not write $dll"; exit 1 }
    }
    Write-Host "installer check: payload and runtime files verified in a silent install"
} finally {
    Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue
}

$app = Get-Item "$root\dist\SoundX.exe"
$inst = Get-Item "$root\dist\$installerName"
Write-Host ""
Write-Host "packaged ($label):"
Write-Host ("  app        {0}  ({1:N0} bytes)" -f $app.FullName, $app.Length)
Write-Host ("  installer  {0}  ({1:N0} bytes)" -f $inst.FullName, $inst.Length)
