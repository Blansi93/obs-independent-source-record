#requires -Version 5.1
<#
.SYNOPSIS
    Builds the Record Source plugin for OBS Studio 33.x on Windows.

.DESCRIPTION
    The script downloads the OBS sources and the pre-built Qt 6 package if they
    are not already present in .deps, generates import libraries from the
    installed OBS DLLs, configures CMake and builds the plugin bundle:

        build/stage/obs-record-source/obs-record-source.dll
        build/stage/obs-record-source/data/manifest.json

    Use -Install to copy the bundle to
    C:\ProgramData\obs-studio\plugins\obs-record-source.

.PARAMETER BuildDir
    CMake build directory. Defaults to <repo>/build.

.PARAMETER Config
    CMake configuration: Release (default) or Debug.

.PARAMETER Install
    Copy the staged bundle into the OBS third-party plugin directory.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Install
#>
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\build"),
    [ValidateSet("Release", "Debug", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "Release",
    [switch]$Install
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$DepsDir = Join-Path $RepoRoot ".deps"
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)

$ObsVersion = "33.0.1"
$DepsVersion = "2026-10-23"
$CmakeVersion = "3.31.8"

$ObsSourceDir = $env:OBS_SOURCE_DIR
$QtDir = $env:OBS_QT_DIR
$ObsInstallDir = $env:OBS_INSTALL_DIR

if (-not $ObsInstallDir) {
    $ObsInstallDir = Join-Path $env:ProgramFiles "obs-studio"
}

function Write-Step([string]$Text) {
    Write-Host ""
    Write-Host "==> $Text" -ForegroundColor Cyan
}

function Download-File([string]$Url, [string]$Destination) {
    if (Test-Path $Destination) {
        Write-Host "    Usando $Destination"
        return
    }

    Write-Host "    Descargando $Url"
    $directory = Split-Path -Parent $Destination
    if (-not (Test-Path $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }

    $oldProgress = $ProgressPreference
    $ProgressPreference = "SilentlyContinue"
    try {
        Invoke-WebRequest -Uri $Url -OutFile $Destination -UseBasicParsing
    } finally {
        $ProgressPreference = $oldProgress
    }
}

function Expand-Zip([string]$Zip, [string]$Destination) {
    if (Test-Path $Destination) {
        Remove-Item $Destination -Recurse -Force
    }
    Expand-Archive -Path $Zip -DestinationPath $Destination -Force
}

function Find-CMake {
    $command = Get-Command cmake -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $local = Get-ChildItem (Join-Path $DepsDir "cmake") -Recurse -Filter "cmake.exe" -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "bin\\cmake\.exe$" } |
        Select-Object -First 1
    if ($local) {
        return $local.FullName
    }

    $zip = Join-Path $DepsDir "cmake-$CmakeVersion-windows-x86_64.zip"
    Download-File "https://github.com/Kitware/CMake/releases/download/v$CmakeVersion/cmake-$CmakeVersion-windows-x86_64.zip" $zip
    Expand-Zip $zip (Join-Path $DepsDir "cmake")

    $local = Get-ChildItem (Join-Path $DepsDir "cmake") -Recurse -Filter "cmake.exe" |
        Where-Object { $_.FullName -match "bin\\cmake\.exe$" } |
        Select-Object -First 1
    if (-not $local) {
        throw "No se pudo localizar cmake.exe después de extraer el archivo."
    }
    return $local.FullName
}

function Import-VcEnvironment([string]$VcVarsAll) {
    Write-Host "    Cargando el entorno de Visual C++"
    $output = cmd /c "`"$VcVarsAll`" >nul 2>&1 && set"
    foreach ($line in $output) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
        }
    }
}

function Find-VcTools {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    $installations = @()

    if (Test-Path $vswhere) {
        $json = & $vswhere -all -products * -format json
        if ($LASTEXITCODE -eq 0 -and $json) {
            $installations = @($json | ConvertFrom-Json | ForEach-Object { $_.installationPath })
        }
    }

    # Fallback for Build Tools installations whose component metadata is not
    # registered with vswhere.
    foreach ($candidate in @(
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2022\BuildTools"),
        (Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\2022\BuildTools"),
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\2022\Community"),
        (Join-Path ${env:ProgramFiles} "Microsoft Visual Studio\2022\Community")
    )) {
        if ((Test-Path $candidate) -and ($installations -notcontains $candidate)) {
            $installations += $candidate
        }
    }

    $installation = $null
    $versionDir = $null
    foreach ($candidate in $installations) {
        $msvcRoot = Join-Path $candidate "VC\Tools\MSVC"
        if (-not (Test-Path $msvcRoot)) {
            continue
        }
        $found = Get-ChildItem $msvcRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
        if ($found) {
            $installation = $candidate
            $versionDir = $found
            break
        }
    }

    if (-not $installation -or -not $versionDir) {
        throw "No se encontró una instalación de Visual Studio con las herramientas de C++."
    }

    $hostDir = Join-Path $versionDir.FullName "bin\Hostx64\x64"
    if (-not (Test-Path (Join-Path $hostDir "dumpbin.exe"))) {
        throw "No se encontró dumpbin.exe en $hostDir."
    }
    if (-not (Test-Path (Join-Path $hostDir "lib.exe"))) {
        throw "No se encontró lib.exe en $hostDir."
    }

    return @{
        Installation = $installation
        HostDir = $hostDir
        VcVarsAll = Join-Path $installation "VC\Auxiliary\Build\vcvars64.bat"
    }
}

function New-ImportLibrary {
    param(
        [string]$DllPath,
        [string]$DefPath,
        [string]$LibPath,
        [string]$Dumpbin,
        [string]$LibTool
    )

    Write-Host "    Generando $(Split-Path -Leaf $LibPath) desde $(Split-Path -Leaf $DllPath)"

    $exports = & $Dumpbin /nologo /exports $DllPath
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin falló para $DllPath."
    }

    $names = New-Object System.Collections.Generic.List[string]
    foreach ($line in $exports) {
        if ($line -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+([^\s]+)') {
            $name = $Matches[1]
            if ($name -notmatch '^\[NONAME\]' -and $name -ne "Name") {
                $names.Add($name)
            }
        }
    }

    if ($names.Count -eq 0) {
        throw "No se encontraron exportaciones en $DllPath."
    }

    $defContent = "LIBRARY $([System.IO.Path]::GetFileName($DllPath))`r`nEXPORTS`r`n"
    foreach ($name in ($names | Sort-Object -Unique)) {
        $defContent += "    $name`r`n"
    }
    Set-Content -Path $DefPath -Value $defContent -Encoding ASCII

    & $LibTool /nologo /def:$DefPath /out:$LibPath /machine:x64
    if ($LASTEXITCODE -ne 0) {
        throw "lib.exe falló al generar $LibPath."
    }
}

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
if (-not (Test-Path $DepsDir)) {
    New-Item -ItemType Directory -Path $DepsDir -Force | Out-Null
}

if (-not $ObsSourceDir) {
    $ObsSourceDir = Join-Path $DepsDir "obs-studio-$ObsVersion"
    if (-not (Test-Path (Join-Path $ObsSourceDir "libobs\obs.h"))) {
        Write-Step "Preparando las fuentes de OBS $ObsVersion"
        $zip = Join-Path $DepsDir "obs-studio-$ObsVersion.zip"
        Download-File "https://github.com/obsproject/obs-studio/archive/refs/tags/$ObsVersion.zip" $zip
        $tempExtract = Join-Path $DepsDir "obs-studio-extract"
        Expand-Zip $zip $tempExtract
        $inner = Join-Path $tempExtract "obs-studio-$ObsVersion"
        if (-not (Test-Path $inner)) {
            throw "El archivo de OBS no contiene la carpeta esperada."
        }
        if (Test-Path $ObsSourceDir) {
            Remove-Item $ObsSourceDir -Recurse -Force
        }
        Move-Item $inner $ObsSourceDir
        Remove-Item $tempExtract -Recurse -Force
    }
}

if (-not $QtDir) {
    $QtDir = Join-Path $DepsDir "qt6"
    if (-not (Test-Path (Join-Path $QtDir "lib\cmake\Qt6\Qt6Config.cmake"))) {
        Write-Step "Preparando Qt 6 ($DepsVersion)"
        $zip = Join-Path $DepsDir "windows-deps-qt6-$DepsVersion-x64.zip"
        Download-File "https://github.com/obsproject/obs-deps/releases/download/$DepsVersion/windows-deps-qt6-$DepsVersion-x64.zip" $zip
        Expand-Zip $zip $QtDir
    }
}

# ---------------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------------
Write-Step "Localizando Visual Studio y CMake"
$cmake = Find-CMake
$vc = Find-VcTools
Write-Host "    CMake:  $cmake"
Write-Host "    MSVC:   $($vc.HostDir)"
Write-Host "    OBS:    $ObsInstallDir"

$obsDll = Join-Path $ObsInstallDir "bin\64bit\obs.dll"
$frontendDll = Join-Path $ObsInstallDir "bin\64bit\obs-frontend-api.dll"
if (-not (Test-Path $obsDll) -or -not (Test-Path $frontendDll)) {
    throw "No se encontraron obs.dll y obs-frontend-api.dll en $ObsInstallDir\bin\64bit."
}

# ---------------------------------------------------------------------------
# Import libraries generated from the installed OBS DLLs
# ---------------------------------------------------------------------------
Write-Step "Generando bibliotecas de importación de OBS"
$importLibDir = Join-Path $BuildDir "importlibs"
if (-not (Test-Path $importLibDir)) {
    New-Item -ItemType Directory -Path $importLibDir -Force | Out-Null
}

$obsLib = Join-Path $importLibDir "obs.lib"
$frontendLib = Join-Path $importLibDir "obs-frontend-api.lib"
if (-not (Test-Path $obsLib)) {
    New-ImportLibrary -DllPath $obsDll -DefPath (Join-Path $importLibDir "obs.def") -LibPath $obsLib -Dumpbin (Join-Path $vc.HostDir "dumpbin.exe") -LibTool (Join-Path $vc.HostDir "lib.exe")
}
if (-not (Test-Path $frontendLib)) {
    New-ImportLibrary -DllPath $frontendDll -DefPath (Join-Path $importLibDir "obs-frontend-api.def") -LibPath $frontendLib -Dumpbin (Join-Path $vc.HostDir "dumpbin.exe") -LibTool (Join-Path $vc.HostDir "lib.exe")
}

# ---------------------------------------------------------------------------
# CMake configure and build
# ---------------------------------------------------------------------------
Write-Step "Configurando CMake"
$generator = $env:CMAKE_GENERATOR
if (-not $generator) {
    $generator = "NMake Makefiles"
}

if ($generator -notlike "Visual Studio*") {
    Import-VcEnvironment $vc.VcVarsAll
}

# If the build directory was configured with another generator, start clean so
# CMake does not reject the change.
$cacheFile = Join-Path $BuildDir "CMakeCache.txt"
if (Test-Path $cacheFile) {
    $cachedGenerator = Select-String -Path $cacheFile -Pattern '^CMAKE_GENERATOR:INTERNAL=(.*)$' |
        Select-Object -First 1
    if ($cachedGenerator -and $cachedGenerator.Matches[0].Groups[1].Value -ne $generator) {
        Write-Host "    El generador cambió; limpiando la caché de CMake"
        Remove-Item $cacheFile -Force
        Remove-Item (Join-Path $BuildDir "CMakeFiles") -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$configureArgs = @(
    "-S", $RepoRoot,
    "-B", $BuildDir,
    "-G", $generator,
    "-DOBS_SOURCE_DIR=$ObsSourceDir",
    "-DOBS_QT_DIR=$QtDir",
    "-DOBS_INSTALL_DIR=$ObsInstallDir",
    "-DOBS_IMPORT_LIB_DIR=$importLibDir"
)
if ($generator -notlike "Visual Studio*") {
    $configureArgs += "-DCMAKE_BUILD_TYPE=$Config"
} else {
    $configureArgs += @("-A", "x64")
}

& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) {
    throw "La configuración de CMake falló."
}

Write-Step "Compilando ($Config)"
if ($generator -like "Visual Studio*") {
    & $cmake --build $BuildDir --config $Config --parallel
} else {
    & $cmake --build $BuildDir
}
if ($LASTEXITCODE -ne 0) {
    throw "La compilación falló."
}

$stageDir = Join-Path $BuildDir "stage\obs-record-source"
if (-not (Test-Path (Join-Path $stageDir "obs-record-source.dll"))) {
    throw "No se encontró el plugin compilado en $stageDir."
}

Write-Step "Compilación completada"
Write-Host "    Plugin: $stageDir\obs-record-source.dll"
Write-Host "    Datos:  $stageDir\data\manifest.json"

if ($Install) {
    $target = Join-Path $env:ProgramData "obs-studio\plugins\obs-record-source"
    Write-Step "Instalando en $target"
    if (Test-Path $target) {
        Remove-Item $target -Recurse -Force
    }
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    Copy-Item (Join-Path $stageDir "obs-record-source.dll") $target -Force
    Copy-Item (Join-Path $stageDir "data") (Join-Path $target "data") -Recurse -Force
    Write-Host "    Instalado. Reinicia OBS para cargar el plugin."
}
