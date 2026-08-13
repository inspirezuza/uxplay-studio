<# Builds and validates the recursive PE dependency closure of a Windows bundle.
   Reads imports with objdump and copies missing DLLs from the selected MSYS2 environment. #>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$StageDir,

    [string]$MsysRoot = "C:\msys64",

    [string]$EnvironmentName = "ucrt64",

    [switch]$ValidateOnly,

    [string]$ManifestPath
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Get-RelativePath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BasePath,

        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $absoluteBase = [IO.Path]::GetFullPath($BasePath)
    $separator = [string][IO.Path]::DirectorySeparatorChar
    if (-not $absoluteBase.EndsWith($separator)) {
        $absoluteBase += [IO.Path]::DirectorySeparatorChar
    }

    $baseUri = [Uri]$absoluteBase
    $pathUri = [Uri][IO.Path]::GetFullPath($Path)
    return [Uri]::UnescapeDataString(
        $baseUri.MakeRelativeUri($pathUri).ToString()
    ).Replace("/", $separator)
}

function Get-StableFileHash {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    for ($attempt = 1; $attempt -le 3; $attempt++) {
        $stream = $null
        $sha256 = $null
        try {
            $stream = [IO.File]::Open(
                $Path,
                [IO.FileMode]::Open,
                [IO.FileAccess]::Read,
                [IO.FileShare]::ReadWrite
            )
            $sha256 = [Security.Cryptography.SHA256]::Create()
            $bytes = $sha256.ComputeHash($stream)
            return ([BitConverter]::ToString($bytes)).Replace("-", "").ToLowerInvariant()
        } catch {
            if ($attempt -eq 3 -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
                throw "Could not hash bundle file '$Path': $($_.Exception.Message)"
            }
        } finally {
            if ($sha256) { $sha256.Dispose() }
            if ($stream) { $stream.Dispose() }
        }
        Start-Sleep -Milliseconds (100 * $attempt)
    }
    throw "Could not hash bundle file '$Path'"
}

function Invoke-ObjdumpWithRetry {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BinaryPath,

        [Parameter(Mandatory = $true)]
        [string]$ObjdumpPath,

        [Parameter(Mandatory = $true)]
        [string]$RuntimeBin,

        [hashtable]$RuntimeAliases = @{}
    )

    for ($attempt = 1; $attempt -le 10; $attempt++) {
        if (-not (Test-Path -LiteralPath $BinaryPath -PathType Leaf)) {
            $fallbackName = [IO.Path]::GetFileName($BinaryPath)
            $fallbackPath = Join-Path $RuntimeBin $fallbackName
            if (
                -not (Test-Path -LiteralPath $fallbackPath) -and
                $RuntimeAliases.ContainsKey($fallbackName)
            ) {
                $fallbackPath = Join-Path $RuntimeBin $RuntimeAliases[$fallbackName]
            }

            if (Test-Path -LiteralPath $fallbackPath -PathType Leaf) {
                Copy-BundleDependencyWithRetry -SourcePath $fallbackPath `
                    -DestinationPath $BinaryPath
            }

            if ($attempt -eq 10) {
                throw "Queued bundle binary disappeared before dependency inspection: $BinaryPath"
            }
            Start-Sleep -Milliseconds (100 * $attempt)
            continue
        }

        $output = & $ObjdumpPath -p $BinaryPath 2>&1
        if ($LASTEXITCODE -eq 0) {
            return $output
        }

        if ($attempt -eq 10) {
            throw "objdump failed for $BinaryPath`n$($output -join [Environment]::NewLine)"
        }
        Start-Sleep -Milliseconds (100 * $attempt)
    }

    throw "objdump did not produce a stable result for $BinaryPath"
}

function Copy-BundleDependencyWithRetry {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourcePath,

        [Parameter(Mandatory = $true)]
        [string]$DestinationPath
    )

    for ($attempt = 1; $attempt -le 10; $attempt++) {
        try {
            Copy-Item -LiteralPath $SourcePath -Destination $DestinationPath -ErrorAction Stop
            return
        } catch {
            if ($attempt -eq 10) {
                throw
            }
            Start-Sleep -Milliseconds (100 * $attempt)
        }
    }

    throw "Could not copy bundle dependency '$SourcePath' to '$DestinationPath'"
}

$stage = (Resolve-Path -LiteralPath $StageDir).Path
$runtimeBin = Join-Path $MsysRoot "$EnvironmentName\bin"
$objdump = Join-Path $runtimeBin "objdump.exe"
$runtimeAliases = @{
    # Some CLANGARM64 packages ship lib-prefixed files whose PE import names
    # omit that prefix.
    "dovi.dll" = "libdovi.dll"
    "rav1e.dll" = "librav1e.dll"
}

if (-not (Test-Path -LiteralPath $objdump)) {
    throw "objdump.exe not found at $objdump"
}

$queue = [System.Collections.Generic.Queue[string]]::new()
$visited = [System.Collections.Generic.HashSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase
)
$unresolved = [System.Collections.Generic.SortedSet[string]]::new(
    [System.StringComparer]::OrdinalIgnoreCase
)
$copied = 0

Get-ChildItem -LiteralPath $stage -Recurse -File |
    Where-Object { $_.Extension -in @(".exe", ".dll") } |
    ForEach-Object { $queue.Enqueue($_.FullName) }

if ($queue.Count -eq 0) {
    throw "No PE binaries found under $stage"
}

$windowsDir = $env:WINDIR
if (-not $windowsDir) {
    $windowsDir = "C:\Windows"
}
$systemDirectories = @(
    (Join-Path $windowsDir "System32"),
    $windowsDir
)

while ($queue.Count -gt 0) {
    $binary = $queue.Dequeue()
    if (-not $visited.Add($binary)) {
        continue
    }

    $output = Invoke-ObjdumpWithRetry -BinaryPath $binary -ObjdumpPath $objdump `
        -RuntimeBin $runtimeBin -RuntimeAliases $runtimeAliases

    $imports = foreach ($line in $output) {
        if ($line -match "DLL Name:\s*(.+)$") {
            $Matches[1].Trim()
        }
    }

    foreach ($import in ($imports | Sort-Object -Unique)) {
        $localCandidates = @(
            (Join-Path (Split-Path -Parent $binary) $import),
            (Join-Path $stage $import)
        )

        $localDependency = $localCandidates |
            Where-Object { Test-Path -LiteralPath $_ } |
            Select-Object -First 1

        if ($localDependency) {
            $queue.Enqueue((Resolve-Path -LiteralPath $localDependency).Path)
            continue
        }

        $runtimeDependency = Join-Path $runtimeBin $import
        if (
            -not (Test-Path -LiteralPath $runtimeDependency) -and
            $runtimeAliases.ContainsKey($import)
        ) {
            $runtimeDependency = Join-Path $runtimeBin $runtimeAliases[$import]
        }

        if (Test-Path -LiteralPath $runtimeDependency) {
            if ($ValidateOnly) {
                $relativeBinary = Get-RelativePath -BasePath $stage -Path $binary
                [void]$unresolved.Add(
                    "$import required by $relativeBinary (available only in $runtimeBin)"
                )
                continue
            }

            $destination = Join-Path $stage $import
            if (-not (Test-Path -LiteralPath $destination)) {
                Copy-BundleDependencyWithRetry -SourcePath $runtimeDependency `
                    -DestinationPath $destination
                $copied++
            }
            $queue.Enqueue((Resolve-Path -LiteralPath $destination).Path)
            continue
        }

        $isWindowsDependency = $false
        foreach ($directory in $systemDirectories) {
            if (Test-Path -LiteralPath (Join-Path $directory $import)) {
                $isWindowsDependency = $true
                break
            }
        }

        if (
            $isWindowsDependency -or
            $import -match "^(api-ms-win-|ext-ms-win-)"
        ) {
            continue
        }

        $relativeBinary = Get-RelativePath -BasePath $stage -Path $binary
        [void]$unresolved.Add("$import required by $relativeBinary")
    }
}

if ($unresolved.Count -gt 0) {
    $details = $unresolved -join [Environment]::NewLine
    throw "Unresolved non-system runtime dependencies:`n$details"
}

if (-not $ValidateOnly -and $ManifestPath) {
    $manifestDirectory = Split-Path -Parent $ManifestPath
    if ($manifestDirectory) {
        New-Item -ItemType Directory -Force -Path $manifestDirectory | Out-Null
    }

    $files = Get-ChildItem -LiteralPath $stage -Recurse -File |
        Sort-Object FullName |
        ForEach-Object {
            $relativePath = Get-RelativePath -BasePath $stage -Path $_.FullName
            [ordered]@{
                path = $relativePath.Replace("\", "/")
                bytes = $_.Length
                sha256 = Get-StableFileHash -Path $_.FullName
            }
        }

    $files | ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath $ManifestPath -Encoding utf8
}

Write-Host (
    "Runtime dependency check passed: {0} binaries inspected, {1} DLLs copied." -f
    $visited.Count,
    $copied
)
