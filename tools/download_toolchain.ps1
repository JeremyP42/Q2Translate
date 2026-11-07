param(
    [string]$BaseUrl = "https://mirrors.tuna.tsinghua.edu.cn/qt/online/qtsdkrepository/windows_x86/desktop",
    [int]$Connections = 12,
    [int64]$ChunkBytes = 4194304,
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$toolchainDir = Join-Path $root ".toolchain"
$downloadDir = Join-Path $toolchainDir "downloads"
$markerDir = Join-Path $toolchainDir ".extracted"
New-Item -ItemType Directory -Force -Path $downloadDir, $markerDir | Out-Null

$items = @(
    @{
        Name = "qtbase.7z"
        Url = "$BaseUrl/qt6_683/qt6_683/qt.qt6.683.win64_mingw/6.8.3-0-202503201308qtbase-Windows-Windows_10_22H2-Mingw-Windows-Windows_10_22H2-X86_64.7z"
        Target = "qt"
        Check = "qt\bin\qmake.exe"
    },
    @{
        Name = "qtsvg.7z"
        Url = "$BaseUrl/qt6_683/qt6_683/qt.qt6.683.win64_mingw/6.8.3-0-202503201308qtsvg-Windows-Windows_10_22H2-Mingw-Windows-Windows_10_22H2-X86_64.7z"
        Target = "qt"
        Check = "qt\bin\Qt6Svg.dll"
    },
    @{
        Name = "qttools.7z"
        Url = "$BaseUrl/qt6_683/qt6_683/qt.qt6.683.win64_mingw/6.8.3-0-202503201308qttools-Windows-Windows_10_22H2-Mingw-Windows-Windows_10_22H2-X86_64.7z"
        Target = "qt"
        Check = "qt\bin\lrelease.exe"
    },
    @{
        Name = "qtdeclarative.7z"
        Url = "$BaseUrl/qt6_683/qt6_683/qt.qt6.683.win64_mingw/6.8.3-0-202503201308qtdeclarative-Windows-Windows_10_22H2-Mingw-Windows-Windows_10_22H2-X86_64.7z"
        Target = "qt"
        Check = "qt\bin\Qt6Qml.dll"
    },
    @{
        Name = "mingw_runtime.7z"
        Url = "$BaseUrl/qt6_683/qt6_683/qt.qt6.683.win64_mingw/6.8.3-0-202503201308MinGW-w64-x86_64-13.1.0-release-posix-seh-msvcrt-rt_v11-rev1-runtime.7z"
        Target = "qt\bin"
        Check = "qt\bin\libgcc_s_seh-1.dll"
    },
    @{
        Name = "mingw1310.7z"
        Url = "$BaseUrl/tools_mingw1310/qt.tools.win64_mingw1310/13.1.0-202407240918mingw1310.7z"
        Target = "mingw"
        Check = "mingw\Tools\mingw1310_64\bin\g++.exe"
    },
    @{
        Name = "cmake.7z"
        Url = "$BaseUrl/tools_cmake/qt.tools.cmake/3.30.5-202502251255windows64_x64_cmake_tools.7z"
        Target = "cmake"
        Check = "cmake\bin\cmake.exe"
    },
    @{
        Name = "ninja.7z"
        Url = "$BaseUrl/tools_ninja/qt.tools.ninja/1.12.1-202502251256ninja_win64.7z"
        Target = "ninja"
        Check = "ninja\ninja.exe"
    }
)

function Get-RemoteSize {
    param([string]$Url)
    $headers = curl.exe -sIL --max-time 60 $Url
    $match = ($headers | Select-String -Pattern "^Content-Length:\s*(\d+)" | Select-Object -Last 1).Matches
    if (-not $match) {
        throw "No Content-Length for $Url"
    }
    return [int64]$match.Groups[1].Value
}

function Download-Parallel {
    param(
        [string]$Url,
        [string]$OutFile,
        [int]$ParallelConnections,
        [int64]$TargetChunkBytes
    )

    $total = Get-RemoteSize $Url
    if ((Test-Path $OutFile) -and (Get-Item $OutFile).Length -eq $total) {
        Write-Output ("OK {0} ({1:N1} MB)" -f $OutFile, ($total / 1MB))
        return
    }
    # Windows PowerShell 5.1 has no ForEach-Object -Parallel. Fall back to a
    # resumable single-connection download; PowerShell 7+ uses parallel chunks.
    if ($PSVersionTable.PSVersion.Major -lt 7) {
        if ((Test-Path $OutFile) -and (Get-Item $OutFile).Length -gt $total) {
            Remove-Item $OutFile -Force
        }
        Write-Output ("Downloading {0} ({1:N1} MB, single connection)" -f $OutFile, ($total / 1MB))
        for ($attempt = 1; $attempt -le 3; $attempt++) {
            & curl.exe -sL --retry 3 --retry-delay 2 -C - --max-time 3600 -o $OutFile $Url
            $length = if (Test-Path $OutFile) { (Get-Item $OutFile).Length } else { 0 }
            if ($length -eq $total) { break }
            if ($attempt -eq 3) {
                throw "Size mismatch for ${OutFile}: $length != $total"
            }
            Write-Output ("Incomplete download ({0:N1} of {1:N1} MB), resuming..." -f ($length / 1MB), ($total / 1MB))
        }
        Write-Output ("DONE {0} ({1:N1} MB)" -f $OutFile, ($total / 1MB))
        return
    }

    if (Test-Path $OutFile) {
        Remove-Item $OutFile -Force
    }

    $partDir = Join-Path $env:TEMP ("qtpart_" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force -Path $partDir | Out-Null

    $count = [math]::Ceiling($total / [double]$TargetChunkBytes)
    if ($count -lt 1) { $count = 1 }
    if ($count -gt 40) { $count = 40 }
    $chunk = [int64][math]::Ceiling($total / [double]$count)

    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    $results = 0..($count - 1) | ForEach-Object -Parallel {
        $index = $_
        $start = [int64]$index * [int64]$using:chunk
        $end = [math]::Min([int64]$using:total - 1, $start + [int64]$using:chunk - 1)
        $destination = Join-Path $using:partDir ("part{0:D3}.bin" -f $index)
        $info = curl.exe -sL -r "$start-$end" -o $destination --retry 3 --retry-delay 2 --max-time 300 -w "%{http_code} %{size_download}" $using:Url
        [pscustomobject]@{
            Index = $index
            Info = $info
            Destination = $destination
        }
    } -ThrottleLimit $ParallelConnections
    $stopwatch.Stop()

    $bad = $results | Where-Object { -not ($_.Info -match "^(206|200)\s") }
    if ($bad) {
        Remove-Item $partDir -Recurse -Force
        throw "Chunk download failed: $($bad.Info -join '; ')"
    }

    $stream = [IO.File]::Create($OutFile)
    try {
        for ($index = 0; $index -lt $count; $index++) {
            $part = Join-Path $partDir ("part{0:D3}.bin" -f $index)
            $input = [IO.File]::OpenRead($part)
            try {
                $input.CopyTo($stream)
            } finally {
                $input.Dispose()
            }
        }
    } finally {
        $stream.Dispose()
    }
    Remove-Item $partDir -Recurse -Force

    $length = (Get-Item $OutFile).Length
    Write-Output ("DONE {0} {1:N1} MB in {2:N1}s ({3:N0} KB/s)" -f `
        $OutFile, ($length / 1MB), $stopwatch.Elapsed.TotalSeconds, ($length / 1KB / $stopwatch.Elapsed.TotalSeconds))
    if ($length -ne $total) {
        throw "Size mismatch for ${OutFile}: $length != $total"
    }
}

function Get-Extractor {
    $sevenZip = $null
    foreach ($candidate in @(
        (Get-Command 7z.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source),
        "$env:ProgramFiles\7-Zip\7z.exe",
        "${env:ProgramFiles(x86)}\7-Zip\7z.exe"
    )) {
        if ($candidate -and (Test-Path $candidate)) {
            $sevenZip = $candidate
            break
        }
    }
    if ($sevenZip) {
        return [pscustomobject]@{ Path = $sevenZip; Kind = "7z" }
    }

    $tar = Get-Command tar.exe -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
    if ($tar) {
        return [pscustomobject]@{ Path = $tar; Kind = "tar" }
    }

    throw "Neither 7z.exe nor tar.exe was found. Install 7-Zip or use Windows 10 1803+ with the built-in tar.exe."
}

function Expand-Package {
    param(
        [string]$Archive,
        [string]$Target,
        [pscustomobject]$Extractor
    )
    New-Item -ItemType Directory -Force -Path $Target | Out-Null
    if ($Extractor.Kind -eq "7z") {
        & $Extractor.Path x -y -bso0 -bsp0 "-o$Target" $Archive
    } else {
        & $Extractor.Path -xf $Archive -C $Target
    }
    if ($LASTEXITCODE -ne 0) {
        throw "Extraction failed (exit $LASTEXITCODE): $Archive"
    }
}

if (-not (Get-Command curl.exe -ErrorAction SilentlyContinue)) {
    throw "curl.exe was not found. Windows 10 1803+ includes it; otherwise install curl or use a newer Windows."
}

$extractor = Get-Extractor

foreach ($item in $items) {
    $archive = Join-Path $downloadDir $item.Name
    $target = Join-Path $toolchainDir $item.Target
    $checkPath = Join-Path $toolchainDir $item.Check
    $marker = Join-Path $markerDir ($item.Name + ".ok")

    if ($Force) {
        Remove-Item $archive, $marker -Force -ErrorAction SilentlyContinue
    }

    if (-not $Force -and (Test-Path $checkPath) -and (Test-Path $marker)) {
        $markerSize = (Get-Content $marker -Raw).Trim()
        if (-not (Test-Path $archive) -or $markerSize -eq [string](Get-Item $archive).Length) {
            Write-Output ("OK {0}" -f $item.Name)
            continue
        }
    }

    Write-Output ("=== {0} ===" -f $item.Name)
    Download-Parallel -Url $item.Url -OutFile $archive `
        -ParallelConnections $Connections -TargetChunkBytes $ChunkBytes

    Write-Output ("Extracting {0} -> {1}" -f $item.Name, $target)
    Expand-Package -Archive $archive -Target $target -Extractor $extractor
    Set-Content -Path $marker -Value (Get-Item $archive).Length -NoNewline
}

$missing = @()
foreach ($item in $items) {
    if (-not (Test-Path (Join-Path $toolchainDir $item.Check))) {
        $missing += $item.Check
    }
}
if ($missing.Count -gt 0) {
    throw "Toolchain is incomplete. Missing: $($missing -join ', ')"
}

$qtBin = Join-Path $toolchainDir "qt\bin"
$mingwBin = Join-Path $toolchainDir "mingw\Tools\mingw1310_64\bin"
$cmakeExe = Join-Path $toolchainDir "cmake\bin\cmake.exe"
$ninjaExe = Join-Path $toolchainDir "ninja\ninja.exe"

$qtVersion = (& (Join-Path $qtBin "qmake.exe") -query QT_VERSION 2>$null)
$gccVersion = (& (Join-Path $mingwBin "g++.exe") --version 2>$null | Select-Object -First 1)
$cmakeVersion = (& $cmakeExe --version 2>$null | Select-Object -First 1)
$ninjaVersion = (& $ninjaExe --version 2>$null | Select-Object -First 1)

Write-Output ""
Write-Output "Toolchain ready in $toolchainDir"
Write-Output "  Qt:    $qtVersion"
Write-Output "  GCC:   $gccVersion"
Write-Output "  CMake: $cmakeVersion"
Write-Output "  Ninja: $ninjaVersion"
Write-Output ""
Write-Output "Archives are kept in $downloadDir (safe to delete)."
