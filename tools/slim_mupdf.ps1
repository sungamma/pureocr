<#
.SYNOPSIS
    从官方预编译 libmupdf.lib 里剔除内嵌 CJK 字体，生成 libmupdf-slim.lib。

.DESCRIPTION
    MuPDF 把每个内置字体单独编译成一个 .obj，字体数据以
        _binary_<name>  /  _binary_<name>_size
    两个符号导出。官方库里的三个 CJK 字体（思源宋体 TTC + 两个 Droid 回退字体）
    合计约 32 MB，全部被链接进 exe 的 .data 段，占了 38 MB 里的大头。

    本脚本用 lib.exe /REMOVE 把这三个 obj 从归档里摘掉，输出 libmupdf-slim.lib，
    再由 src/pdf/mupdf_font_stubs.cpp 提供同名零长度桩符号，运行时改用系统字体
    （msyh/simsun）做 CJK 回退。原始 libmupdf.lib 不做任何改动，可反复执行。

.PARAMETER MupdfDir
    MuPDF 目录（含 lib\libmupdf.lib），默认 ..\third_party\mupdf-1.25.0

.PARAMETER Remove
    要剔除的归档成员（相对归档根），默认三个 CJK 字体。

.PARAMETER Force
    即使 libmupdf-slim.lib 比源库新也强制重建。

.EXAMPLE
    pwsh -NoProfile -File tools\slim_mupdf.ps1

.EXAMPLE
    pwsh -NoProfile -File tools\slim_mupdf.ps1 -Force
#>
[CmdletBinding()]
param(
    [string]$MupdfDir,
    [string[]]$Remove = @(
        'x64\Release\libresources\SourceHanSerif-Regular_ttc.obj'
        'x64\Release\libresources\DroidSansFallbackFull_ttf.obj'
        'x64\Release\libresources\DroidSansFallback_ttf.obj'
    ),
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }

function Write-Step([string]$Message) { Write-Host ''; Write-Host "==> $Message" -ForegroundColor Cyan }
function Write-Detail([string]$Message) { Write-Host "    $Message" }

if ([string]::IsNullOrWhiteSpace($MupdfDir)) { $MupdfDir = Join-Path $PSScriptRoot '..\third_party\mupdf-1.25.0' }
$MupdfDir = (Resolve-Path -LiteralPath $MupdfDir).Path
$srcLib = Join-Path $MupdfDir 'lib\libmupdf.lib'
$dstLib = Join-Path $MupdfDir 'lib\libmupdf-slim.lib'

if (-not (Test-Path -LiteralPath $srcLib)) {
    throw "找不到 $srcLib（MuPDF 预编译库）"
}

# ---------------- 定位 lib.exe ----------------
function Get-VSInstallPath {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { return $null }
    $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if (-not $path) { return $null }
    return ($path | Select-Object -First 1).Trim()
}

$libExe = $null
$vsInstall = Get-VSInstallPath
if ($vsInstall) {
    $libExe = Get-ChildItem -Path (Join-Path $vsInstall 'VC\Tools\MSVC') -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'bin\Hostx64\x64\lib.exe' } |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
}
if (-not $libExe) {
    $cmd = Get-Command lib.exe -ErrorAction SilentlyContinue
    if ($cmd) { $libExe = $cmd.Source }
}
if (-not $libExe) { throw '找不到 lib.exe，请安装 Visual Studio 的 C++ 生成工具' }

$srcTime = (Get-Item -LiteralPath $srcLib).LastWriteTime
if (-not $Force -and (Test-Path -LiteralPath $dstLib) -and
    (Get-Item -LiteralPath $dstLib).LastWriteTime -ge $srcTime) {
    Write-Step '已是最新'
    Write-Detail "libmupdf-slim.lib 比源库新，跳过（-Force 可强制重建）"
    return
}

Write-Step '生成瘦身版 MuPDF 静态库'
Write-Detail "源库   : $srcLib"
Write-Detail "目标   : $dstLib"
Write-Detail "lib.exe: $libExe"

# lib.exe /REMOVE 要求配 /OUT，输入库本身不被修改；这里仍然在临时目录里操作，避免就地写坏
$tmpDir = Join-Path ([IO.Path]::GetTempPath()) ('pureocr-slim-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
$workLib = Join-Path $tmpDir 'libmupdf-work.lib'
$outLib = Join-Path $tmpDir 'libmupdf-slim.lib'
try {
    Write-Detail '复制源库到临时目录 ...'
    Copy-Item -LiteralPath $srcLib -Destination $workLib

    $libArgs = @('/NOLOGO', "/OUT:$outLib")
    foreach ($member in $Remove) { $libArgs += "/REMOVE:$member" }
    $libArgs += $workLib

    Write-Detail ("剔除成员：" + ($Remove -join ', '))
    & $libExe @libArgs
    if ($LASTEXITCODE -ne 0) { throw "lib.exe 失败，退出码 $LASTEXITCODE" }
    if (-not (Test-Path -LiteralPath $outLib)) { throw 'lib.exe 没有生成输出文件' }

    # 校验成员确实被剔除
    $listing = & $libExe /NOLOGO /LIST $outLib
    $left = @()
    foreach ($member in $Remove) {
        $leaf = Split-Path -Leaf $member
        $hit = $listing | Where-Object { $_ -like "*$leaf" }
        if ($hit) { $left += $leaf }
    }
    if ($left.Count -gt 0) { throw ("以下成员仍在归档中：" + ($left -join ', ')) }

    Copy-Item -LiteralPath $outLib -Destination $dstLib -Force
}
finally {
    Remove-Item -LiteralPath $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
}

$before = (Get-Item -LiteralPath $srcLib).Length
$after = (Get-Item -LiteralPath $dstLib).Length
Write-Step '完成'
Write-Detail ("原库 : {0,8:N1} MB" -f ($before / 1MB))
Write-Detail ("瘦身 : {0,8:N1} MB  (省 {1:N1} MB)" -f ($after / 1MB), (($before - $after) / 1MB))
Write-Detail 'CMake 会自动优先使用 libmupdf-slim.lib（-DOCR_MUPDF_SLIM=OFF 可退回原库）'