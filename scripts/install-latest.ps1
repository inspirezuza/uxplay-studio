<#
   Installs the verified staged bundle into the per-user application directory
   and rewrites both Windows entry points to that exact executable. The install
   directory is intentionally fixed to the user's LocalAppData scope so the
   package can be refreshed without an elevation prompt.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$StageDir,

    [string]$InstallRoot = $(if ($env:LOCALAPPDATA) {
        Join-Path $env:LOCALAPPDATA "UxPlay Studio"
    } else {
        Join-Path (Join-Path $env:USERPROFILE "AppData\Local") "UxPlay Studio"
    })
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Remove-PathWithRetry {
    param(
        [Parameter(Mandatory = $true)]
        [string]$LiteralPath,

        [int]$Attempts = 10,

        [int]$DelayMilliseconds = 400
    )

    for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
        try {
            Remove-Item -LiteralPath $LiteralPath -Recurse -Force -ErrorAction Stop
            return
        }
        catch {
            if ($attempt -eq $Attempts) { throw }
            Start-Sleep -Milliseconds $DelayMilliseconds
        }
    }
}

$stage = (Resolve-Path -LiteralPath $StageDir).Path
$stageExe = Join-Path $stage "uxplay-studio.exe"
if (-not (Test-Path -LiteralPath $stageExe -PathType Leaf)) {
    throw "The staged bundle does not contain uxplay-studio.exe: $stage"
}

$localAppData = $env:LOCALAPPDATA
if (-not $localAppData) {
    $localAppData = Join-Path $env:USERPROFILE "AppData\Local"
}
$expectedInstallRoot = [IO.Path]::GetFullPath((Join-Path $localAppData "UxPlay Studio"))
$resolvedInstallRoot = [IO.Path]::GetFullPath($InstallRoot)
if (-not [StringComparer]::OrdinalIgnoreCase.Equals($resolvedInstallRoot, $expectedInstallRoot)) {
    throw "Refusing to install outside the per-user UxPlay Studio directory: $resolvedInstallRoot"
}

# Never overwrite a running executable. A recording session must be closed by
# the user before a new package can replace the installed runtime.
$running = Get-Process -Name "uxplay-studio" -ErrorAction SilentlyContinue |
    Where-Object {
        try { [StringComparer]::OrdinalIgnoreCase.Equals($_.Path, $resolvedInstallRoot + "\uxplay-studio.exe") }
        catch { $false }
    }
foreach ($process in @($running)) {
    $process.CloseMainWindow() | Out-Null
    if (-not $process.WaitForExit(10000)) {
        throw "UxPlay Studio is still running. Close it before installing the latest build."
    }
}

if (Test-Path -LiteralPath $resolvedInstallRoot) {
    Remove-PathWithRetry -LiteralPath $resolvedInstallRoot
}
New-Item -ItemType Directory -Force -Path $resolvedInstallRoot | Out-Null
Copy-Item -Path (Join-Path $stage "*") -Destination $resolvedInstallRoot -Recurse -Force

$desktop = [Environment]::GetFolderPath("Desktop")
$startMenu = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs"
New-Item -ItemType Directory -Force -Path $startMenu | Out-Null
$shortcutPaths = @(
    (Join-Path $desktop "UxPlay Studio.lnk"),
    (Join-Path $startMenu "UxPlay Studio.lnk")
)

# Remove stale UxPlay Studio links from the two user entry-point folders. This
# is deliberately limited to links whose target is the UxPlay executable.
foreach ($folder in @($desktop, $startMenu)) {
    if (-not (Test-Path -LiteralPath $folder)) { continue }
    foreach ($link in Get-ChildItem -LiteralPath $folder -Filter "*.lnk" -File -ErrorAction SilentlyContinue) {
        try {
            $old = (New-Object -ComObject WScript.Shell).CreateShortcut($link.FullName)
            $target = $old.TargetPath
            if ($target -and
                [IO.Path]::GetFileName($target) -ieq "uxplay-studio.exe" -and
                $shortcutPaths -notcontains $link.FullName) {
                Remove-Item -LiteralPath $link.FullName -Force
            }
        }
        catch {
            # A malformed unrelated shortcut is not an installation blocker.
        }
    }
}

$shell = New-Object -ComObject WScript.Shell
foreach ($shortcutPath in $shortcutPaths) {
    $shortcut = $shell.CreateShortcut($shortcutPath)
    $shortcut.TargetPath = Join-Path $resolvedInstallRoot "uxplay-studio.exe"
    $shortcut.WorkingDirectory = $resolvedInstallRoot
    $shortcut.IconLocation = "$(Join-Path $resolvedInstallRoot 'resources\icon.ico'),0"
    $shortcut.Description = "UxPlay Studio - latest local install"
    $shortcut.Save()
}

Write-Host "Installed latest UxPlay Studio to $resolvedInstallRoot"
Write-Host "Desktop shortcut: $($shortcutPaths[0])"
Write-Host "Start Menu shortcut: $($shortcutPaths[1])"
