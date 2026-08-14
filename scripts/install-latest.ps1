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

# Clear every legacy UxPlay process before replacing the install. The app uses
# one global mutex, so an old developer-path process can otherwise intercept a
# Start Menu launch and bring its stale UI back to the foreground.
$running = Get-Process -Name "uxplay-studio" -ErrorAction SilentlyContinue |
    Where-Object { $_.Id -ne $PID }
foreach ($process in @($running)) {
    $processPath = ""
    try { $processPath = $process.Path } catch {}
    $isCurrentInstall = [StringComparer]::OrdinalIgnoreCase.Equals(
        $processPath,
        $resolvedInstallRoot + "\uxplay-studio.exe"
    )
    $process.CloseMainWindow() | Out-Null
    if ($process.WaitForExit(10000)) { continue }
    if ($isCurrentInstall) {
        throw "UxPlay Studio is still running. Stop recording and close it before installing the latest build."
    }
    # A legacy developer build has no reliable recording-state contract. It is
    # explicitly outside the managed install, so force-close only that stale
    # process to release the single-instance mutex and leave one clean app.
    Stop-Process -Id $process.Id -Force -ErrorAction Stop
    $process.WaitForExit(5000) | Out-Null
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
