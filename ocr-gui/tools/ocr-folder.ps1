<#
.SYNOPSIS
    批量 OCR：把 tobeocr 文件夹里的 PDF 全部转成可搜索 PDF（输出名 <原名>_OCR.pdf）。

.DESCRIPTION
    固定参数（可用参数覆盖）：2 线程 / 300 dpi / 置信度 0.55 / 同行碎片合并 0.8（x 行高）。
    * 默认输入目录：<本脚本目录>\..\tobeocr（不存在就报错，不会偷偷建空目录）
    * 默认输出：与源文件同目录，文件名加 _OCR 后缀；用 -OutDir 可统一放到别的目录
    * 自动跳过：文件名已以 _OCR 结尾的、目标已存在且没加 -Overwrite 的
    * 每个文件的日志：<输入目录>\ocr-logs\<原名>.log

.EXAMPLE
    pwsh -NoProfile -File tools\ocr-folder.ps1

.EXAMPLE
    pwsh -NoProfile -File tools\ocr-folder.ps1 -Folder D:\扫描\待处理 -Threads 4 -Overwrite

.EXAMPLE
    pwsh -NoProfile -File tools\ocr-folder.ps1 -Exe ..\dist\ocrgui-20261103\ocrgui.exe -Pages 1-20
#>
[CmdletBinding()]
param(
    [string]$Folder,
    [string]$Exe,
    [string]$OutDir,
    [int]$Threads = 2,
    [double]$Dpi = 300,
    [double]$MinScore = 0.55,
    [double]$MergeGap = 0.8,
    [string]$ModelDir,
    [string]$Pages,
    [switch]$Mask,
    [switch]$Recurse,
    [switch]$Overwrite,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }

function Write-Step([string]$m) { Write-Host ''; Write-Host "==> $m" -ForegroundColor Cyan }
function Write-Ok([string]$m)   { Write-Host "    $m" -ForegroundColor Green }
function Write-Warn2([string]$m){ Write-Host "    $m" -ForegroundColor Yellow }
function Write-Bad([string]$m)  { Write-Host "    $m" -ForegroundColor Red }

$srcDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

# ---------------- 输入目录 ----------------
if ([string]::IsNullOrWhiteSpace($Folder)) { $Folder = Join-Path $srcDir 'tobeocr' }
if (-not (Test-Path -LiteralPath $Folder)) {
    throw "输入目录不存在：$Folder`n请把待处理的 PDF 放进去，或用 -Folder 指定别的目录。"
}
$Folder = (Resolve-Path -LiteralPath $Folder).Path
if ([string]::IsNullOrWhiteSpace($OutDir)) { $OutDir = $Folder }
if (-not (Test-Path -LiteralPath $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path

$logDir = Join-Path $Folder 'ocr-logs'
if (-not (Test-Path -LiteralPath $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }

# ---------------- 定位 exe ----------------
function Resolve-Exe([string]$hint) {
    if (-not [string]::IsNullOrWhiteSpace($hint)) {
        $p = if ([IO.Path]::IsPathRooted($hint)) { [IO.Path]::GetFullPath($hint) }
             else { [IO.Path]::GetFullPath((Join-Path (Get-Location) $hint)) }
        if (Test-Path -LiteralPath $p) { return (Resolve-Path -LiteralPath $p).Path }
        throw "找不到指定的 exe：$p"
    }
    $cands = @()
    $dist = Join-Path $srcDir 'dist'
    if (Test-Path -LiteralPath $dist) {
        $cands += Get-ChildItem -LiteralPath $dist -Filter 'ocrgui.exe' -Recurse -ErrorAction SilentlyContinue |
                  Sort-Object LastWriteTime -Descending
    }
    foreach ($c in @((Join-Path $srcDir 'build\Release\ocrgui.exe'), (Join-Path $srcDir 'build\ocrgui.exe'))) {
        if (Test-Path -LiteralPath $c) { $cands += Get-Item -LiteralPath $c }
    }
    if ($cands.Count -eq 0) { throw '找不到 ocrgui.exe：请先运行 tools\release.ps1 打包，或用 -Exe 指定路径。' }
    return $cands[0].FullName
}

$exePath = Resolve-Exe $Exe

# ---------------- 模型目录 ----------------
function Test-HasOnnx([string]$dir) {
    if ([string]::IsNullOrWhiteSpace($dir) -or -not (Test-Path -LiteralPath $dir)) { return $false }
    return [bool](Get-ChildItem -LiteralPath $dir -Filter '*.onnx' -ErrorAction SilentlyContinue | Select-Object -First 1)
}
if ([string]::IsNullOrWhiteSpace($ModelDir)) {
    $exeModels = Join-Path (Split-Path -Parent $exePath) 'models'
    if (-not (Test-HasOnnx $exeModels)) {
        $repoModels = Join-Path $srcDir 'models'
        if (Test-HasOnnx $repoModels) { $ModelDir = $repoModels }
    }
}

# ---------------- 收集文件 ----------------
$pdfs = Get-ChildItem -LiteralPath $Folder -Filter '*.pdf' -File -Recurse:$Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -notlike '*\ocr-logs\*' } |
        Where-Object { $_.BaseName -notmatch '_OCR$' } |
        Sort-Object FullName

Write-Step '批量 OCR'
Write-Host "    程序     : $exePath"
Write-Host "    输入目录 : $Folder"
Write-Host "    输出目录 : $OutDir"
if ($ModelDir) { Write-Host "    模型目录 : $ModelDir" } else { Write-Host "    模型目录 : 程序默认（exe 同级 models / 用户缓存）" }
Write-Host "    参数     : $Threads 线程 / $Dpi dpi / 置信度 $MinScore / 合并阈值 $MergeGap"
if ($Mask) { Write-Host "    模式     : 用 OCR 文本替换原文（--mask，仅整篇处理时生效）" }

if ($pdfs.Count -eq 0) {
    Write-Warn2 '没有找到 PDF（已跳过 *_OCR.pdf）。'
    exit 0
}
Write-Host "    待处理   : $($pdfs.Count) 个文件"

# ---------------- 逐个处理 ----------------
$swAll = [Diagnostics.Stopwatch]::StartNew()
$ok = @(); $skipped = @(); $failed = @()
$idx = 0

foreach ($f in $pdfs) {
    $idx++
    $base = $f.BaseName
    $outPdf = Join-Path $OutDir ($base + '_OCR.pdf')
    $logPath = Join-Path $logDir ($base + '.log')
    Write-Host ''
    Write-Host ("[{0}/{1}] {2}" -f $idx, $pdfs.Count, $f.Name) -ForegroundColor White

    if ((Test-Path -LiteralPath $outPdf) -and -not $Overwrite) {
        Write-Warn2 ("已存在，跳过：{0}（要重做加 -Overwrite）" -f (Split-Path -Leaf $outPdf))
        $skipped += $f.Name
        continue
    }
    if (Test-Path -LiteralPath $logPath) { Remove-Item -LiteralPath $logPath -Force -ErrorAction SilentlyContinue }

    $argList = @(
        '--pdf',      ('"' + $f.FullName + '"'),
        '--out-pdf',  ('"' + $outPdf + '"'),
        '--dpi',      $Dpi,
        '--threads',  $Threads,
        '--min-score',$MinScore,
        '--merge-gap',$MergeGap,
        '--log',      ('"' + $logPath + '"')
    )
    if ($ModelDir) { $argList += @('--model-dir', ('"' + $ModelDir + '"')) }
    if (-not [string]::IsNullOrWhiteSpace($Pages)) { $argList += @('--pages', $Pages) }
    if ($Mask) { $argList += '--mask' }

    if ($DryRun) {
        Write-Host ("    " + $exePath + ' ' + ($argList -join ' ')) -ForegroundColor DarkGray
        Write-Warn2 '仅预览（-DryRun），未执行'
        continue
    }

    $sw = [Diagnostics.Stopwatch]::StartNew()
    $code = -1
    try {
        $p = Start-Process -FilePath $exePath -ArgumentList $argList -Wait -PassThru -WindowStyle Hidden
        $code = $p.ExitCode
    } catch {
        Write-Bad ("启动失败：{0}" -f $_.Exception.Message)
        $failed += $f.Name
        continue
    }
    $sw.Stop()
    $secs = [math]::Round($sw.Elapsed.TotalSeconds, 1)

    if ($code -ne 0 -or -not (Test-Path -LiteralPath $outPdf)) {
        Write-Bad ("失败（退出码 {0}，{1} 秒）：{2}" -f $code, $secs, $f.Name)
        if (Test-Path -LiteralPath $logPath) {
            $tail = Get-Content -LiteralPath $logPath -Tail 6 -ErrorAction SilentlyContinue
            foreach ($t in $tail) { Write-Host ("      " + $t) -ForegroundColor DarkGray }
        }
        $failed += $f.Name
        continue
    }

    $info = ''
    if (Test-Path -LiteralPath $logPath) {
        $txt = Get-Content -LiteralPath $logPath -Raw -Encoding UTF8 -ErrorAction SilentlyContinue
        if ($txt -match '(?m)^共\s*(\d+)\s*页，写入\s*(\d+)\s*行') { $info += ("{0} 页 / {1} 行" -f $matches[1], $matches[2]) }
        if ($txt -match '(?m)^待复核：(\d+)\s*行') { $info += ("，待复核 {0} 行" -f $matches[1]) }
        if ($txt -match '输出自检：(\d+) 页内容完整') { $info += ("，自检 {0} 页 OK" -f $matches[1]) }
        elseif ($txt -match '注意：输出自检没能执行') { $info += '，自检未执行' }
    }
    $sz = [math]::Round((Get-Item -LiteralPath $outPdf).Length / 1MB, 2)
    Write-Ok ("完成 {0} 秒 -> {1}（{2} MB{3}）" -f $secs, (Split-Path -Leaf $outPdf), $sz, $(if ($info) { '，' + $info } else { '' }))
    $ok += $f.Name
}

$swAll.Stop()
$total = [math]::Round($swAll.Elapsed.TotalSeconds, 1)

Write-Step '汇总'
Write-Host ("    成功 {0} 个 / 跳过 {1} 个 / 失败 {2} 个，总耗时 {3} 秒" -f $ok.Count, $skipped.Count, $failed.Count, $total)
if ($skipped.Count -gt 0) { Write-Host ("    跳过：{0}" -f ($skipped -join '、')) -ForegroundColor Yellow }
if ($failed.Count -gt 0) {
    Write-Host ("    失败：{0}" -f ($failed -join '、')) -ForegroundColor Red
    Write-Host ("    失败详情看日志：{0}" -f $logDir) -ForegroundColor Red
    exit 1
}
Write-Host ("    输出目录：{0}" -f $OutDir) -ForegroundColor Green
exit 0