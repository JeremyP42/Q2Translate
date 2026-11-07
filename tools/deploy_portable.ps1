param(
    [string]$Configuration = "Release",
    # Empty means "take the version from CMakeLists.txt and bump the patch number".
    [string]$Version = "",
    [string]$WorkDir = "work",
    [string]$ArchiveDir = "build",
    [switch]$SkipBuild,
    [switch]$NoBump
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$build = Join-Path $root $WorkDir
# The project version lives in CMakeLists.txt. Every package build raises the
# patch number unless -NoBump is passed or an explicit -Version is given.
$cmakeFile = Join-Path $root "CMakeLists.txt"
$cmakeText = Get-Content -LiteralPath $cmakeFile -Raw
if ($cmakeText -notmatch 'project\(Q2Translate VERSION ([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "Cannot read the project version from $cmakeFile"
}
$currentVersion = $Matches[1]
if ($Version -eq "") {
    if ($NoBump -or $SkipBuild) {
        $Version = $currentVersion
    } else {
        $parts = $currentVersion.Split('.')
        $parts[2] = [string]([int]$parts[2] + 1)
        $Version = $parts -join '.'
        $cmakeText = $cmakeText -replace 'project\(Q2Translate VERSION [0-9]+\.[0-9]+\.[0-9]+',
                                         "project(Q2Translate VERSION $Version"
        Set-Content -LiteralPath $cmakeFile -Value $cmakeText -NoNewline -Encoding utf8
        Write-Output "Version bumped: $currentVersion -> $Version"
    }
}
Write-Output "Building version $Version"

$archiveDir = Join-Path $root $ArchiveDir
$package = Join-Path $build "package"
$qtBin = Join-Path $root ".toolchain\qt\bin"
$mingwBin = Join-Path $root ".toolchain\mingw\Tools\mingw1310_64\bin"
$cmake = Join-Path $root ".toolchain\cmake\bin\cmake.exe"
$ninja = Join-Path $root ".toolchain\ninja\ninja.exe"
$windeployqt = Join-Path $qtBin "windeployqt.exe"

$env:PATH = "$qtBin;$mingwBin;$(Join-Path $root '.toolchain\ninja');$env:PATH"

if (-not $SkipBuild) {
    if (-not (Test-Path $cmake)) {
        throw "CMake not found at $cmake. Run tools\download_toolchain.ps1 first."
    }
    if (-not (Test-Path (Join-Path $build "CMakeCache.txt"))) {
        & $cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=$Configuration `
            -DCMAKE_PREFIX_PATH=(Join-Path $root ".toolchain\qt") `
            -DCMAKE_CXX_COMPILER=(Join-Path $mingwBin "g++.exe") `
            -DCMAKE_MAKE_PROGRAM=$ninja
        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    }
    & $cmake --build $build
    if ($LASTEXITCODE -ne 0) { throw "Build failed" }
}

if (-not (Test-Path $windeployqt)) {
    throw "windeployqt not found at $windeployqt"
}

if (Test-Path $package) {
    Remove-Item $package -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $package | Out-Null

$exe = Join-Path $build "Q2Translate.exe"
if (-not (Test-Path $exe)) { throw "Executable not found: $exe" }
Copy-Item $exe $package
$pdfium = Join-Path $root "third_party\pdfium\bin\pdfium.dll"
if (Test-Path $pdfium) { Copy-Item $pdfium $package }
foreach ($name in @("libnuspell.dll", "cygicuuc76.dll", "cygicudata76.dll")) {
    $dependency = Join-Path $build $name
    if (Test-Path $dependency) { Copy-Item $dependency $package }
}
$pluginDir = Join-Path $package "plugins"
New-Item -ItemType Directory -Force -Path $pluginDir | Out-Null
$webpDecoder = Join-Path $root "plugins\libwebpdecoder.dll"
if (Test-Path $webpDecoder) { Copy-Item $webpDecoder $pluginDir }

$licenseDir = Join-Path $package "THIRD_PARTY_LICENSES"
New-Item -ItemType Directory -Force -Path $licenseDir | Out-Null
$pdfiumLicense = Join-Path $root "third_party\pdfium\LICENSE"
if (Test-Path $pdfiumLicense) { Copy-Item $pdfiumLicense (Join-Path $licenseDir "pdfium.txt") }
$yamlLicense = Join-Path $root "third_party\yaml-cpp\LICENSE"
if (Test-Path $yamlLicense) { Copy-Item $yamlLicense (Join-Path $licenseDir "yaml-cpp.txt") }
$nuspellCopying = Join-Path $root "third_party\nuspell\COPYING"
if (Test-Path $nuspellCopying) { Copy-Item $nuspellCopying (Join-Path $licenseDir "nuspell-COPYING.txt") }
$nuspellLesser = Join-Path $root "third_party\nuspell\COPYING.LESSER"
if (Test-Path $nuspellLesser) { Copy-Item $nuspellLesser (Join-Path $licenseDir "nuspell-COPYING.LESSER.txt") }
$icuLicense = Join-Path $root "third_party\icu\LICENSE"
if (Test-Path $icuLicense) { Copy-Item $icuLicense (Join-Path $licenseDir "icu.txt") }
$qtLgpl = Join-Path $root "third_party\qt\LICENSE.LGPL-3.0.txt"
if (Test-Path $qtLgpl) { Copy-Item $qtLgpl (Join-Path $licenseDir "qt-LICENSE.LGPL-3.0.txt") }
$qtGpl = Join-Path $root "third_party\qt\LICENSE.GPL-3.0.txt"
if (Test-Path $qtGpl) { Copy-Item $qtGpl (Join-Path $licenseDir "qt-LICENSE.GPL-3.0.txt") }
$tesseractLicense = Join-Path $root "third_party\tesseract\LICENSE"
if (Test-Path $tesseractLicense) { Copy-Item $tesseractLicense (Join-Path $licenseDir "tesseract-Apache-2.0.txt") }
$mingwNotice = Join-Path $root "third_party\mingw-runtime\NOTICE.md"
if (Test-Path $mingwNotice) { Copy-Item $mingwNotice (Join-Path $licenseDir "mingw-runtime-NOTICE.md") }
$notices = Join-Path $root "THIRD_PARTY_NOTICES.md"
if (Test-Path $notices) { Copy-Item $notices (Join-Path $licenseDir "THIRD_PARTY_NOTICES.md") }
$pdfiumLicenses = Join-Path $root "third_party\pdfium\licenses"
if (Test-Path $pdfiumLicenses) { Copy-Item $pdfiumLicenses (Join-Path $licenseDir "pdfium") -Recurse -Force }
Copy-Item (Join-Path $root "settings-default.json") $package
Set-Content -Path (Join-Path $package "portable.txt") -Value "" -NoNewline

& $windeployqt --release --compiler-runtime --no-translations (Join-Path $package "Q2Translate.exe")
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

foreach ($name in @("services", "Themes", "Locales")) {
    $source = Join-Path $root "data\$name"
    if (Test-Path $source) {
        Copy-Item $source (Join-Path $package $name) -Recurse -Force
    }
}

$dataDir = Join-Path $package "Data"
if (Test-Path $dataDir) { Remove-Item $dataDir -Recurse -Force }
Get-ChildItem $package -Recurse -Filter "settings.json" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match "\\services\\" } |
    Remove-Item -Force

if (Test-Path (Join-Path $package "qml")) { Remove-Item (Join-Path $package "qml") -Recurse -Force }
if (Test-Path (Join-Path $package "qmltooling")) { Remove-Item (Join-Path $package "qmltooling") -Recurse -Force }

$pluginsDir = Join-Path $package "plugins"
New-Item -ItemType Directory -Force -Path $pluginsDir | Out-Null
foreach ($name in @("platforms", "tls", "styles", "imageformats", "iconengines", "networkinformation", "generic")) {
    $source = Join-Path $package $name
    if (Test-Path $source) {
        Move-Item $source (Join-Path $pluginsDir $name) -Force
    }
}
@"
[Paths]
Plugins = plugins
"@ | Set-Content -Path (Join-Path $package "qt.conf") -Encoding ascii
foreach ($name in @("Qt6OpenGL.dll", "Qt6Quick.dll", "Qt6QmlMeta.dll", "Qt6QmlModels.dll",
                    "Qt6QmlWorkerScript.dll", "D3Dcompiler_47.dll", "dxcompiler.dll", "dxil.dll")) {
    $path = Join-Path $package $name
    if (Test-Path $path) { Remove-Item $path -Force }
}


# ---------------------------------------------------------------------------
# Two distributions of the same version:
#   full  - everything embedded (current behaviour);
#   small - without Tesseract, PaddleOCR and Nuspell engines/models.
# libnuspell.dll and the ICU DLLs stay in the small build because the
# executable imports them at startup; only the connector directories are cut.
# ---------------------------------------------------------------------------
$smallPackage = Join-Path $build "package-small"
if (Test-Path $smallPackage) { Remove-Item $smallPackage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $smallPackage | Out-Null
Copy-Item (Join-Path $package "*") $smallPackage -Recurse -Force

foreach ($relative in @(
    "services\ocr\tesseract",
    "services\ocr\paddleocr",
    "services\spelling\nuspell"
)) {
    $path = Join-Path $smallPackage $relative
    if (Test-Path $path) { Remove-Item $path -Recurse -Force }
}

# The small build has no Tesseract/PaddleOCR: default OCR uses the built-in
# Windows engine until an engine package is installed.
$smallSettings = Join-Path $smallPackage "settings-default.json"
if (Test-Path $smallSettings) {
    $smallDefaults = Get-Content -LiteralPath $smallSettings -Raw | ConvertFrom-Json
    if ($smallDefaults.Ocr) {
        $smallDefaults.Ocr.ProviderScreen = "windows_ocr"
        $smallDefaults.Ocr.ProviderImages = "windows_ocr"
        $smallDefaults.Ocr.ProviderPdf = "windows_ocr"
        $smallDefaults.Ocr.ProviderDjvu = "windows_ocr"
    }
    if ($smallDefaults.ServiceProviders) {
        $smallDefaults.ServiceProviders.spell = "hunspell"
    }
    $smallDefaults | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath $smallSettings -Encoding utf8
}

function New-PortableArchive {
    param([string]$SourcePackage, [string]$ZipPath)
    if (Test-Path $ZipPath) { Remove-Item $ZipPath -Force }
    $whitelistNames = @(
        "Q2Translate.exe",
        "settings-default.json",
        "portable.txt",
        "qt.conf",
        "plugins", "services",
        "Themes", "Locales", "THIRD_PARTY_LICENSES"
    )
    $items = @()
    foreach ($name in $whitelistNames) {
        $path = Join-Path $SourcePackage $name
        if (Test-Path $path) { $items += $path }
    }
    $items += Get-ChildItem $SourcePackage -File -Filter "*.dll" | Select-Object -ExpandProperty FullName
    $items = $items | Select-Object -Unique
    if ($items.Count -eq 0) { throw "Nothing to package in $SourcePackage" }
    Compress-Archive -Path $items -DestinationPath $ZipPath -CompressionLevel Fastest
}

if (-not (Test-Path $archiveDir)) { New-Item -ItemType Directory -Force -Path $archiveDir | Out-Null }
$fullZip = Join-Path $archiveDir "Q2TranslatePortable_full_v$Version.zip"
$smallZip = Join-Path $archiveDir "Q2TranslatePortable_small_v$Version.zip"
New-PortableArchive -SourcePackage $package -ZipPath $fullZip
New-PortableArchive -SourcePackage $smallPackage -ZipPath $smallZip
Write-Output "Package: $package"
Write-Output "Small package: $smallPackage"
Write-Output "Archive (full): $fullZip"
Write-Output "Archive (small): $smallZip"
