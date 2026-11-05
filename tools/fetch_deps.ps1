<#
.SYNOPSIS
    下载并准备 ocr-gui 的第三方依赖（构建前跑一次，产物都不入库）。

.DESCRIPTION
    第三方库体积太大且不适合进 Git，仓库里只放本脚本，依赖按需下载：

      ONNX Runtime 1.20.1 (win-x64)  MIT        -> third_party\onnxruntime-win-x64-1.20.1
         官方 zip，校验 SHA256；解压时跳过 300 MB 的 onnxruntime.pdb

      MuPDF 1.25.0                   AGPL-3.0  -> third_party\mupdf-1.25.0
         官方源码包，用 VS 的 msbuild 编 libmupdf.lib（约 550 MB，十几分钟），
         编完自动调用 tools\slim_mupdf.ps1 生成 libmupdf-slim.lib（剔除内嵌 CJK 字体）

    GLFW / Dear ImGui 由 CMake 的 FetchContent 在配置阶段拉取，这里不处理。

.PARAMETER Root
    依赖根目录，默认 ..\third_party（即仓库根目录下的 third_party）

.PARAMETER Force
    已存在的依赖也重新下载 / 重新编译

.PARAMETER SkipMupdf
    只准备 ONNX Runtime（不需要 PDF 功能，或想自己提供 MuPDF 时）

.PARAMETER SkipSlim
    不生成 libmupdf-slim.lib（用完整库，exe 会大 32 MB）

.PARAMETER DownloadDir
    下载缓存目录，默认 %TEMP%\ocrgui-deps（已下完的包会复用）

.EXAMPLE
    pwsh -NoProfile -File tools\fetch_deps.ps1

.EXAMPLE
    pwsh -NoProfile -File tools\fetch_deps.ps1 -Force -SkipMupdf
#>
[CmdletBinding()]
param(
    [string]$Root,
    [switch]$Force,
    [switch]$SkipMupdf,
    [switch]$SkipSlim,
    [string]$DownloadDir = (Join-Path $env:TEMP 'ocrgui-deps')
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }

function Write-Step([string]$Message) { Write-Host ''; Write-Host "==> $Message" -ForegroundColor Cyan }
function Write-Detail([string]$Message) { Write-Host "    $Message" }
function Write-WarnMsg([string]$Message) { Write-Host "    [警告] $Message" -ForegroundColor Yellow }

if ([string]::IsNullOrWhiteSpace($Root)) { $Root = Join-Path $PSScriptRoot '..\third_party' }
$Root = [IO.Path]::GetFullPath($Root)
New-Item -ItemType Directory -Path $Root -Force | Out-Null
New-Item -ItemType Directory -Path $DownloadDir -Force | Out-Null

$OrtVersion   = '1.20.1'
$OrtUrl       = "https://github.com/microsoft/onnxruntime/releases/download/v$OrtVersion/onnxruntime-win-x64-$OrtVersion.zip"
$OrtSha256    = '78D447051E48BD2E1E778BBA378BEC4ECE11191C9E538CF7B2C4A4565E8F5581'

$MupdfVersion = '1.25.0'
$MupdfUrl     = "https://mupdf.com/downloads/archive/mupdf-$MupdfVersion-source.tar.gz"
$MupdfSha256  = '5493DE830AB506B1C3BC8F5D670AA1A9F4ADA9A959A4CC36CB7B4A3C12812074'

Write-Step '依赖准备'
Write-Detail "目标目录 : $Root"
Write-Detail "下载缓存 : $DownloadDir"

function Get-Package([string]$Url, [string]$Sha256) {
    $name = Split-Path $Url -Leaf
    $file = Join-Path $DownloadDir $name
    if ((Test-Path -LiteralPath $file) -and -not $Force) {
        $h = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
        if ($h -eq $Sha256) {
            Write-Detail "$name 已在缓存中，SHA256 校验通过"
            return $file
        }
        Write-WarnMsg "$name 校验值不符，重新下载"
    }
    Write-Detail "下载 $Url"
    $tmp = "$file.part"
    if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Force }
    Invoke-WebRequest -Uri $Url -OutFile $tmp -UseBasicParsing -TimeoutSec 1800
    $got = (Get-FileHash -LiteralPath $tmp -Algorithm SHA256).Hash
    if ($got -ne $Sha256) {
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
        throw "$name 的 SHA256 不符：期望 $Sha256，实际 $got"
    }
    Move-Item -LiteralPath $tmp -Destination $file -Force
    Write-Detail "$name 下载完成并校验通过"
    return $file
}

function Expand-ZipSelective([string]$ZipPath, [string]$Destination, [string[]]$ExcludePattern = @()) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $count = 0
        foreach ($e in $zip.Entries) {
            if ($e.Name -eq '') { continue }
            $skip = $false
            foreach ($p in $ExcludePattern) { if ($e.FullName -like $p) { $skip = $true; break } }
            if ($skip) { continue }
            $target = Join-Path $Destination ($e.FullName -replace '/', '\')
            New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
            [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $target, $true)
            $count++
        }
        Write-Detail "解压 $count 个文件"
    } finally {
        $zip.Dispose()
    }
}

# ---------------- 1) ONNX Runtime ----------------
$ortDir = Join-Path $Root "onnxruntime-win-x64-$OrtVersion"
$ortLib = Join-Path $ortDir 'lib\onnxruntime.lib'
if ((Test-Path -LiteralPath $ortLib) -and -not $Force) {
    Write-Step 'ONNX Runtime'
    Write-Detail "已存在，跳过：$ortDir"
} else {
    Write-Step "ONNX Runtime $OrtVersion"
    $zip = Get-Package $OrtUrl $OrtSha256
    if (Test-Path -LiteralPath $ortDir) { Remove-Item -LiteralPath $ortDir -Recurse -Force }
    $stage = Join-Path $DownloadDir 'ort-stage'
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
    Write-Detail '解压中...'
    Expand-ZipSelective $zip $stage @('*.pdb')
    Move-Item -LiteralPath (Join-Path $stage "onnxruntime-win-x64-$OrtVersion") -Destination $ortDir
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    if (-not (Test-Path -LiteralPath $ortLib)) { throw "解压后仍找不到 $ortLib" }
    Write-Detail "完成：$ortDir"
}

# ---------------- 2) MuPDF ----------------
if ($SkipMupdf) {
    Write-Step 'MuPDF'
    Write-Detail '按参数跳过（构建时 PDF 功能会被关掉）'
} else {
    $mupdfDir = Join-Path $Root "mupdf-$MupdfVersion"
    $mupdfLib = Join-Path $mupdfDir 'lib\libmupdf.lib'
    if ((Test-Path -LiteralPath $mupdfLib) -and -not $Force) {
        Write-Step 'MuPDF'
        Write-Detail "已存在，跳过：$mupdfDir"
    } else {
        Write-Step "MuPDF $MupdfVersion（源码编译，首次约 10~20 分钟）"
        $tar = Get-Package $MupdfUrl $MupdfSha256

        $stage = Join-Path $DownloadDir 'mupdf-stage'
        if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
        New-Item -ItemType Directory -Path $stage -Force | Out-Null
        Write-Detail '解压源码...'
        & tar -xzf $tar -C $stage
        if ($LASTEXITCODE -ne 0) { throw 'tar 解压失败（Windows 10 1803 以后自带 bsdtar）' }
        $src = Join-Path $stage "mupdf-$MupdfVersion-source"
        if (-not (Test-Path -LiteralPath $src)) { throw "解压后找不到 $src" }

        $sln = Join-Path $src 'platform\win32\mupdf.sln'
        if (-not (Test-Path -LiteralPath $sln)) { throw "找不到 $sln" }

        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        if (-not (Test-Path -LiteralPath $vswhere)) { throw '找不到 vswhere.exe：需要安装 Visual Studio 2022 + 桌面 C++ 工作负载' }
        $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if (-not $vsPath) { throw '找不到带 C++ 工具的 Visual Studio' }
        $vsPath = ($vsPath | Select-Object -First 1).Trim()
        $msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
        if (-not (Test-Path -LiteralPath $msbuild)) { throw "找不到 $msbuild" }

        Write-Detail 'msbuild libmupdf（Release|x64），这一步最慢...'
        & $msbuild $sln /t:libmupdf /p:Configuration=Release /p:Platform=x64 /m /nologo /v:minimal
        if ($LASTEXITCODE -ne 0) { throw "msbuild 失败（退出码 $LASTEXITCODE）" }

        $built = Get-ChildItem -LiteralPath $src -Filter 'libmupdf.lib' -Recurse -File -ErrorAction SilentlyContinue |
                 Sort-Object Length -Descending | Select-Object -First 1
        if (-not $built) { throw '编译结束但没找到 libmupdf.lib' }
        Write-Detail "编译产物：$($built.FullName)（$([math]::Round($built.Length / 1MB, 1)) MB）"

        if (Test-Path -LiteralPath $mupdfDir) { Remove-Item -LiteralPath $mupdfDir -Recurse -Force }
        New-Item -ItemType Directory -Path (Join-Path $mupdfDir 'lib') -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $src 'include') -Destination $mupdfDir -Recurse
        Copy-Item -LiteralPath $built.FullName -Destination $mupdfLib
        $copying = Join-Path $src 'COPYING.txt'
        if (Test-Path -LiteralPath $copying) { Copy-Item -LiteralPath $copying -Destination (Join-Path $mupdfDir 'LICENSE') }

        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
        Write-Detail "完成：$mupdfDir"
    }

    $slimLib = Join-Path $mupdfDir 'lib\libmupdf-slim.lib'
    if ($SkipSlim) {
        Write-Detail '按参数跳过 libmupdf-slim.lib（exe 会大 32 MB）'
    } elseif ((Test-Path -LiteralPath $slimLib) -and -not $Force) {
        Write-Detail 'libmupdf-slim.lib 已存在，跳过'
    } else {
        Write-Step '生成 libmupdf-slim.lib（剔除内嵌 CJK 字体）'
        & (Join-Path $PSScriptRoot 'slim_mupdf.ps1') -MupdfDir $mupdfDir
        if (-not (Test-Path -LiteralPath $slimLib)) { throw 'slim_mupdf.ps1 没有生成 libmupdf-slim.lib' }
    }
}

# ---------------- 汇总 ----------------
Write-Step '检查结果'
$ok = $true
$checks = @(
    @{ Name = "ONNX Runtime $OrtVersion"; Path = (Join-Path $Root "onnxruntime-win-x64-$OrtVersion\lib\onnxruntime.lib") },
    @{ Name = 'ONNX Runtime DLL';         Path = (Join-Path $Root "onnxruntime-win-x64-$OrtVersion\lib\onnxruntime.dll") },
    @{ Name = 'MuPDF 头文件';             Path = (Join-Path $Root "mupdf-$MupdfVersion\include\mupdf\fitz.h") },
    @{ Name = 'MuPDF 库';                 Path = (Join-Path $Root "mupdf-$MupdfVersion\lib\libmupdf.lib") }
)
if ($SkipMupdf) { $checks = @($checks | Where-Object { $_.Name -notlike 'MuPDF*' }) }
foreach ($item in $checks) {
    if (Test-Path -LiteralPath $item.Path) { Write-Detail "[OK]   $($item.Name)" }
    else { Write-Host "    [缺失] $($item.Name)：$($item.Path)" -ForegroundColor Red; $ok = $false }
}
if (-not $SkipMupdf -and -not (Test-Path -LiteralPath (Join-Path $Root "mupdf-$MupdfVersion\lib\libmupdf-slim.lib"))) {
    Write-Detail '[提示] 没有 libmupdf-slim.lib，构建会退回完整的 libmupdf.lib'
}
if (-not $ok) { throw '依赖不完整' }

Write-Detail ''
Write-Detail '下一步：pwsh -NoProfile -File tools\release.ps1 -Clean -Version <版本号>'