<#
.SYNOPSIS
    ocr-gui 一键发布脚本：编译 Release 并打包成可直接分发的 zip。

.DESCRIPTION
    流程：
      1. 定位 CMake（PATH 里没有就用 Visual Studio 自带的）
      2. 检查 third_party（ONNX Runtime / MuPDF）与 models 目录
      3. CMake 配置（默认 Visual Studio 17 2022 + x64）
      4. 编译 Release
      5. 调用 tools\package.ps1 组装发布目录
      6. 压缩 zip 并生成 SHA256 校验文件

    构建目录默认在仓库内（<ocr-gui>\build），打包完成后会自动删掉这个临时目录，
    下次发布就是全新配置；想留着增量编译用 -KeepBuild。

.PARAMETER Version
    发布版本号，默认当天日期 yyyyMMdd。例：-Version 1.0.0 -> ocrgui-1.0.0.zip

.PARAMETER BuildDir
    CMake 构建目录，默认 <ocr-gui>\build；发布完成后会被删除（-KeepBuild 可保留）。

.PARAMETER KeepBuild
    保留构建目录（默认发布成功后删除，保持工作区干净）。

.PARAMETER OutDir
    发布产物目录，默认 <ocr-gui>\dist。

.EXAMPLE
    pwsh -NoProfile -File tools\release.ps1 -Clean

.EXAMPLE
    pwsh -NoProfile -File tools\release.ps1 -Version 1.0.0 -Clean

.EXAMPLE
    pwsh -NoProfile -File tools\release.ps1 -SkipBuild     # 只重新打包现有构建
#>
[CmdletBinding()]
param(
    [string]$Version,
    [string]$BuildDir = (Join-Path $PSScriptRoot '..\build'),
    [string]$OutDir,
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug')]
    [string]$Config = 'Release',
    [string]$Generator,
    [int]$Jobs = [Environment]::ProcessorCount,
    [switch]$Clean,
    [switch]$SkipBuild,
    [switch]$SkipPackage,
    [switch]$NoZip,
    [switch]$AllowNoMupdf,
    [switch]$KeepBuild,
    [switch]$Open
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }

function Write-Step([string]$Message) { Write-Host ''; Write-Host "==> $Message" -ForegroundColor Cyan }
function Write-Detail([string]$Message) { Write-Host "    $Message" }
function Write-WarnMsg([string]$Message) { Write-Host "    [警告] $Message" -ForegroundColor Yellow }

# ---------------- 路径 ----------------
$scriptDir  = $PSScriptRoot
$srcDir     = (Resolve-Path (Join-Path $scriptDir '..')).Path
$repoRoot   = (Resolve-Path (Join-Path $srcDir '..')).Path
$thirdParty = Join-Path $repoRoot 'third_party'
$modelsDir  = Join-Path $srcDir 'models'
if ([string]::IsNullOrWhiteSpace($OutDir)) { $OutDir = Join-Path $srcDir 'dist' }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)   # 去掉 .. 之类的相对片段，便于显示和删除
$OutDir   = [IO.Path]::GetFullPath($OutDir)

Write-Step '工程信息'
Write-Detail "源码目录 : $srcDir"
Write-Detail "构建目录 : $BuildDir"
Write-Detail "输出目录 : $OutDir"
Write-Detail "配置     : $Config / x64"

# ---------------- 工具链 ----------------
function Get-VSInstallPath {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { return $null }
    $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if (-not $path) { return $null }
    return ($path | Select-Object -First 1).Trim()
}

$vsInstall = Get-VSInstallPath

function Resolve-CMakeExe {
    $cmd = Get-Command cmake -CommandType Application -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = @(
        (Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe'),
        (Join-Path $env:LOCALAPPDATA 'Programs\CMake\bin\cmake.exe')
    )
    if ($vsInstall) {
        $candidates += (Join-Path $vsInstall 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')
    }
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c) { return $c }
    }
    throw '找不到 cmake.exe：请安装 CMake 并加入 PATH，或安装 Visual Studio 的 C++ 工作负载。'
}

$cmakeExe = Resolve-CMakeExe

if (-not $Generator) {
    if ($vsInstall) {
        $vswhere  = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
        $lineVer  = (& $vswhere -latest -products * -property catalog_productLineVersion 2>$null | Select-Object -First 1)
        switch ("$lineVer".Trim()) {
            '2022'  { $Generator = 'Visual Studio 17 2022' }
            '2019'  { $Generator = 'Visual Studio 16 2019' }
            default { $Generator = 'Visual Studio 17 2022' }
        }
    } elseif (Get-Command ninja -CommandType Application -ErrorAction SilentlyContinue) {
        $Generator = 'Ninja'
    } else {
        throw '没有找到 Visual Studio（MSVC）或 Ninja，无法编译 C++ 工程。'
    }
}

Write-Step '工具链'
Write-Detail "cmake     : $cmakeExe"
Write-Detail "generator : $Generator"
if ($vsInstall) { Write-Detail "Visual Studio : $vsInstall" }

# ---------------- 依赖检查 ----------------
function Find-DependencyDir([string]$Pattern) {
    if (-not (Test-Path -LiteralPath $thirdParty)) { return $null }
    $d = Get-ChildItem -LiteralPath $thirdParty -Directory -Filter $Pattern -ErrorAction SilentlyContinue |
         Sort-Object Name -Descending | Select-Object -First 1
    if ($d) { return $d.FullName }
    return $null
}

Write-Step '依赖检查'

$ortDir = Find-DependencyDir 'onnxruntime-win-x64-*'
if (-not $ortDir -or -not (Test-Path -LiteralPath (Join-Path $ortDir 'lib\onnxruntime.lib'))) {
    throw "找不到 ONNX Runtime：请把 onnxruntime-win-x64-* 解压到 $thirdParty（需要 lib\onnxruntime.lib 和 lib\onnxruntime.dll）。"
}
Write-Detail "ONNX Runtime : $ortDir"

$mupdfDir = Find-DependencyDir 'mupdf-*'
if (-not $mupdfDir -or -not (Test-Path -LiteralPath (Join-Path $mupdfDir 'lib\libmupdf.lib'))) {
    if (-not $AllowNoMupdf) {
        throw "找不到 MuPDF：请把 mupdf-* 解压到 $thirdParty（需要 lib\libmupdf.lib）。确实要出不含 PDF 功能的包，加 -AllowNoMupdf。"
    }
    Write-WarnMsg 'MuPDF 缺失：本次构建将不含 PDF 渲染 / 叠字功能'
    $mupdfDir = $null
} else {
    Write-Detail "MuPDF        : $mupdfDir"
}

if (-not $SkipPackage) {
    $onnx = @()
    if (Test-Path -LiteralPath $modelsDir) {
        $onnx = @(Get-ChildItem -LiteralPath $modelsDir -Filter '*.onnx' -File -ErrorAction SilentlyContinue)
    }
    if ($onnx.Count -eq 0) {
        throw "模型目录 $modelsDir 里没有 .onnx 文件：先运行 python tools\fetch_models.py"
    }
    $modelMb = [math]::Round((($onnx | Measure-Object Length -Sum).Sum / 1MB), 1)
    Write-Detail "模型         : $($onnx.Count) 个 onnx（$modelMb MB）"
}

# ---------------- 清理保护 ----------------
function Test-SafeToRemove([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $true }
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $root = [IO.Path]::GetPathRoot($full).TrimEnd('\')
    if ($full.Length -le $root.Length + 1) { return $false }
    foreach ($p in @($srcDir, $repoRoot, $env:USERPROFILE, $env:APPDATA)) {
        if ([string]::IsNullOrWhiteSpace($p)) { continue }
        $p = $p.TrimEnd('\')
        if ($full -eq $p) { return $false }
        if ($p.StartsWith($full + '\', [StringComparison]::OrdinalIgnoreCase)) { return $false }
    }
    return $true
}

# 删除目录，遇到占用（杀毒 / 索引 / 残留 MSBuild）重试几次
function Remove-DirectoryWithRetry([string]$Path) {
    if (-not (Test-SafeToRemove $Path)) { return $false }
    for ($i = 0; $i -lt 5; ++$i) {
        try {
            Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
            return $true
        } catch {
            Start-Sleep -Milliseconds 800
        }
    }
    # 仍然删不掉（残留的 MSBuild 节点 / 杀毒软件按住文件）时，改成「改名」：
    # 同目录改名几乎总能成功，发布流程不会被卡住，垃圾目录可稍后手动清理。
    try {
        $trashName = (Split-Path -Leaf $Path) + ".trash-" + (Get-Date -Format "HHmmss")
        Rename-Item -LiteralPath $Path -NewName $trashName -ErrorAction Stop
        Write-WarnMsg "构建目录被占用，已改名为 $trashName（可稍后手动删除）"
        return $true
    } catch {
        return $false
    }
}

$started = Get-Date

# ---------------- 配置 + 编译 ----------------
if (-not $SkipBuild) {
    if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
        if (-not (Test-SafeToRemove $BuildDir)) { throw "拒绝清理 $BuildDir：路径过于宽泛，请明确指定构建目录。" }
        Write-Step '清理构建目录'
        Write-Detail $BuildDir
        if (-not (Remove-DirectoryWithRetry $BuildDir)) {
            throw "无法清理 $BuildDir：可能被杀毒软件 / 索引 / 残留 MSBuild 进程占用。"
        }
    }

    Write-Step 'CMake 配置'
    $cmakeArgs = @('-S', $srcDir, '-B', $BuildDir, '-G', $Generator)
    if ($Generator -like 'Visual Studio*') {
        $cmakeArgs += @('-A', 'x64')          # 多配置生成器：配置在 --build --config 里给
    } else {
        $cmakeArgs += "-DCMAKE_BUILD_TYPE=$Config"
    }
    $cmakeArgs += "-DORT_DIR=$ortDir"
    if ($mupdfDir) { $cmakeArgs += "-DMUPDF_DIR=$mupdfDir" }
    Write-Detail ((($cmakeArgs | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' '))
    $cfgOut = & $cmakeExe @cmakeArgs 2>&1
    $cfgExit = $LASTEXITCODE
    $cfgOut | ForEach-Object { Write-Host "    $_" }
    if ($cfgExit -ne 0 -and (($cfgOut | Out-String) -match 'C1090|PDB API')) {
        # 云同步（Dropbox / OneDrive）的过滤驱动会挡住 mspdbsrv 的跨进程 PDB 写入，
        # 报 "error C1090: PDB API 调用失败"。这时自动换到系统临时目录重新配置一次，
        # 发布流程照常走完，最后的临时目录也照样删掉。
        $fallbackBuild = Join-Path $env:TEMP 'ocrgui-build'
        Write-WarnMsg "编译器写 PDB 失败（C1090），通常是 Dropbox/OneDrive 同步过滤驱动挡住了。改用临时构建目录：$fallbackBuild"
        $fallbackBuild = [IO.Path]::GetFullPath($fallbackBuild)
        if (Test-Path -LiteralPath $fallbackBuild) { Remove-DirectoryWithRetry $fallbackBuild | Out-Null }
        $BuildDir = $fallbackBuild
        $cmakeArgs = @('-S', $srcDir, '-B', $BuildDir) + ($cmakeArgs | Select-Object -Skip 4)
        $cfgOut = & $cmakeExe @cmakeArgs 2>&1
        $cfgExit = $LASTEXITCODE
        $cfgOut | ForEach-Object { Write-Host "    $_" }
    }
    if ($cfgExit -ne 0) { throw "CMake 配置失败（exit $cfgExit）。首次配置需要联网拉取 GLFW / Dear ImGui。" }

    Write-Step "编译（$Config，并行 $Jobs 个任务）"
    # 关掉 MSBuild 节点复用：否则编译完会残留 MSBuild 进程占着构建目录，删不掉
    $oldNodeReuse = $env:MSBUILDDISABLENODEREUSE
    $env:MSBUILDDISABLENODEREUSE = '1'
    try {
        & $cmakeExe --build $BuildDir --config $Config --parallel $Jobs
    } finally {
        $env:MSBUILDDISABLENODEREUSE = $oldNodeReuse
    }
    if ($LASTEXITCODE -ne 0) { throw "编译失败（exit $LASTEXITCODE）。" }
}

# ---------------- 定位产物 ----------------
Write-Step '定位产物'
$exe = $null
foreach ($cand in @((Join-Path $BuildDir "$Config\ocrgui.exe"), (Join-Path $BuildDir 'ocrgui.exe'))) {
    if (Test-Path -LiteralPath $cand) { $exe = (Resolve-Path -LiteralPath $cand).Path; break }
}
if (-not $exe) {
    $found = Get-ChildItem -LiteralPath $BuildDir -Filter 'ocrgui.exe' -File -Recurse -ErrorAction SilentlyContinue |
             Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($found) { $exe = $found.FullName }
}
if (-not $exe) { throw "在 $BuildDir 下找不到 ocrgui.exe，请先编译（不要用 -SkipBuild）。" }

$exeDir = Split-Path -Parent $exe
$exeItem = Get-Item -LiteralPath $exe
Write-Detail "ocrgui.exe : $exe"
Write-Detail "大小       : $([math]::Round($exeItem.Length / 1MB, 1)) MB，时间 $($exeItem.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))"

if (-not (Test-Path -LiteralPath (Join-Path $exeDir 'onnxruntime.dll'))) {
    Write-WarnMsg 'exe 同目录缺少 onnxruntime.dll，已从 ONNX Runtime 目录补一份'
    Copy-Item -LiteralPath (Join-Path $ortDir 'lib\onnxruntime.dll') -Destination $exeDir -Force
}

# ---------------- 打包 ----------------
$stamp   = if ($Version) { $Version } else { Get-Date -Format 'yyyyMMdd' }
$pkgName = "ocrgui-$stamp"
$pkgDir  = Join-Path $OutDir $pkgName
$zip     = Join-Path $OutDir "$pkgName.zip"

if (-not $SkipPackage) {
    Write-Step '组装发布目录'
    $pkgArgs = @{ BuildDir = $exeDir; OutDir = $OutDir }
    if ($Version) { $pkgArgs['Version'] = $Version }
    if ($NoZip)   { $pkgArgs['NoZip'] = $true }
    & (Join-Path $scriptDir 'package.ps1') @pkgArgs
    if ($LASTEXITCODE -ne 0) { throw "打包失败（exit $LASTEXITCODE）。" }

    if (-not $NoZip -and (Test-Path -LiteralPath $zip)) {
        $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
        $sumFile = Join-Path $OutDir 'SHA256SUMS.txt'
        [IO.File]::WriteAllLines($sumFile, @("$hash  $pkgName.zip"), (New-Object Text.UTF8Encoding($false)))
        Write-Detail "sha256     : $hash"
    }
}

# ---------------- 清理临时构建目录 ----------------
if (-not $SkipBuild -and -not $KeepBuild -and (Test-Path -LiteralPath $BuildDir)) {
    Write-Step '清理临时构建目录'
    if (Remove-DirectoryWithRetry $BuildDir) {
        Write-Detail "已删除 $BuildDir"
    } else {
        Write-WarnMsg "未能删除 $BuildDir（可能被杀毒 / 索引占用），可稍后手动删"
    }
}

# ---------------- 汇总 ----------------
$elapsed = [int]((Get-Date) - $started).TotalSeconds
Write-Step '完成'
if (Test-Path -LiteralPath $pkgDir) {
    $files  = Get-ChildItem -LiteralPath $pkgDir -Recurse -File
    $sizeMb = [math]::Round((($files | Measure-Object Length -Sum).Sum / 1MB), 1)
    Write-Detail "发布目录 : $pkgDir（$($files.Count) 个文件，$sizeMb MB）"
}
if (Test-Path -LiteralPath $zip) {
    Write-Detail "压缩包   : $zip（$([math]::Round((Get-Item -LiteralPath $zip).Length / 1MB, 1)) MB）"
}
Write-Detail "耗时     : $elapsed 秒"
if ($Open) { Invoke-Item -LiteralPath $OutDir }