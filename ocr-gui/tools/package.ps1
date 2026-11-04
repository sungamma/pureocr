<#
.SYNOPSIS
  把 ocrgui 打包成可直接双击运行的目录（并压缩为 zip）。

.DESCRIPTION
  产物目录结构：
    ocrgui.exe           主程序（GUI + CLI）
    onnxruntime.dll
    models\*.onnx        PP-OCR 模型
    使用说明.txt
    run-cli.ps1          命令行示例

.EXAMPLE
  pwsh -NoProfile -File ocr-gui/tools/package.ps1
#>
[CmdletBinding()]
param(
    [string]$BuildDir = (Join-Path $env:USERPROFILE 'build\ocrgui\Release'),
    [string]$OutDir   = (Join-Path $PSScriptRoot '..\dist'),
    [string]$Version,
    [ValidateSet('none', 'basic', 'full')]
    [string]$Models = 'none',
    [switch]$NoZip
)

$ErrorActionPreference = 'Stop'

$repoRoot  = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$modelsDir = (Resolve-Path (Join-Path $PSScriptRoot '..\models')).Path
$exe       = Join-Path $BuildDir 'ocrgui.exe'
$ortDll    = Join-Path $BuildDir 'onnxruntime.dll'

if (-not (Test-Path -LiteralPath $exe)) {
    throw "找不到 $exe，请先构建：cmake --build <build> --config Release"
}
if (-not (Test-Path -LiteralPath $modelsDir)) {
    throw "找不到模型目录 $modelsDir，请先运行：ocrgui.exe --fetch-models"
}

$stamp   = if ($Version) { $Version } else { Get-Date -Format 'yyyyMMdd' }
$pkgName = "ocrgui-$stamp"
$pkgDir  = Join-Path $OutDir $pkgName
if (Test-Path -LiteralPath $pkgDir) {
    try {
        Remove-Item -LiteralPath $pkgDir -Recurse -Force -ErrorAction Stop
    } catch {
        # 旧目录被杀毒 / 索引 / Dropbox 按住时改成改名：发布不能因为一个删不掉的旧目录而失败
        $oldName = "$pkgName.old-" + (Get-Date -Format "HHmmss")
        Rename-Item -LiteralPath $pkgDir -NewName $oldName -ErrorAction Stop
        Write-Host "旧发布目录被占用，已改名为 $oldName"
        Remove-Item -LiteralPath (Join-Path $OutDir $oldName) -Recurse -Force -ErrorAction SilentlyContinue
    }
}
New-Item -ItemType Directory -Path $pkgDir -Force | Out-Null

Copy-Item -LiteralPath $exe -Destination $pkgDir
if (Test-Path -LiteralPath $ortDll) { Copy-Item -LiteralPath $ortDll -Destination $pkgDir }
# ---- 模型：默认只带字典和清单，onnx 由首次运行时的下载向导 / --fetch-models 获取 ----
$modelsOut = Join-Path $pkgDir 'models'
New-Item -ItemType Directory -Path $modelsOut -Force | Out-Null
foreach ($name in @('ppocrv5_dict.txt', 'registry.ini', 'README.md')) {
    $src = Join-Path $modelsDir $name
    if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination $modelsOut }
}
$modelNames = @()
if ($Models -eq 'basic') {
    $modelNames = @('ch_PP-OCRv5_det_infer.onnx', 'ch_PP-OCRv5_rec_infer.onnx', 'ch_ppocr_mobile_v2.0_cls_infer.onnx')
} elseif ($Models -eq 'full') {
    $modelNames = @(Get-ChildItem -LiteralPath $modelsDir -Filter '*.onnx' -File -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
}
foreach ($name in $modelNames) {
    $src = Join-Path $modelsDir $name
    if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination $modelsOut }
}
$onnxCn = @(Get-ChildItem -LiteralPath $modelsOut -Filter '*.onnx' -File -ErrorAction SilentlyContinue).Count
Write-Host "模型打包：$Models（$onnxCn 个 onnx）"

$readme = 'PDF OCR 工具（C++ 版）
=====================

双击 ocrgui.exe 打开界面（左边是参数，右边常驻页面预览）：
  1) 点「选择 PDF 文件…」；输出默认落在同目录「原名_OCR.pdf」，可用「另存为…」改
     要整批处理：点「选择文件夹…」（或把文件夹拖进窗口），可勾「含子目录」「覆盖已有结果」
  2) 「页范围」留空 = 全部；也可以写 1-10,15（框下面会显示共几页、这次处理几页）
  3) 「线程数」默认 2（机器快可调 4），「渲染 DPI」默认 300，最低置信度默认 0.55
  4) 「识别模型」可选：
       v5   移动版，默认，快且准（10 页 / 4 线程约 44 秒）
       v5s  服务器版，最准但慢 4~7 倍（10 页约 290 秒），适合定稿
       v4   上一代，置信度略高但偶有重复字
       v3   最快、准确率最低
       自动探测 = 按 v5 → v4 → v3 顺序找可用模型
       （模型文件缺失时下拉里会标 [缺模型]，并提示缺哪个文件）
  5) 点「开始识别」，进度条和日志实时刷新；完成后可「打开目录 / 导出 CSV / 回灌 CSV」
  6) 右侧预览：竖版页面按高度铺满，识别框绿=正常、红=低置信度，鼠标悬停看识别文字；
     上方可切页码、调预览 DPI、勾选「只看低置信度」
  7) 改完 CSV 后点「回灌 CSV」，按 页码 + 行号 覆盖文字（配合缓存秒出）
  8) 程序会记住上次的参数；也可以把 PDF 直接拖进窗口

命令行（PowerShell）：
  .\run-cli.ps1 -Pdf "D:\某文件.pdf"
  .\run-cli.ps1 -Pdf "D:\某文件.pdf" -Pages 1-10 -Threads 2
  .\run-cli.ps1 -Pdf "D:\某文件.pdf" -Preset v5s
  .\run-cli.ps1 -Pdf "D:\某文件.pdf" -Csv 复核.csv -ReviewScore 0.75
  .\run-cli.ps1 -Pdf "D:\某文件.pdf" -FixCsv 复核_已改.csv -Resume
  .\run-cli.ps1 -Dir "D:\待处理" -Recurse            # 整目录（含子目录）批量
  .\run-cli.ps1 -Dir "D:\待处理" -OutDir "D:\输出"   # 结果统一放另一个目录

说明：
- 输出 PDF 保留原始页面（渲染层不动），只叠加不可见的 OCR 文字层，可直接框选复制
- 默认不动原文字体（最稳）；要盖住原文字才用「用 OCR 文本替换原文」或 --mask
- 批量：每个 PDF 输出「原名_OCR.pdf」；以 _OCR.pdf 结尾的文件、以及已经存在
  「原名_OCR.pdf」的原文件都会自动跳过，可以放心反复跑
- 模型都在 models\ 下；移动版与服务器版共用 ppocrv5_dict.txt
- 断点续跑：同目录生成 *_ocrcache.tsv；整篇跑完自动删除（中途挂掉才会留着），
  改 DPI / 模型 / 合并阈值会自动失效重建
- 只支持 Windows 10/11 x64，不需要显卡（ONNX Runtime CPU）'
Set-Content -LiteralPath (Join-Path $pkgDir '使用说明.txt') -Value $readme -Encoding UTF8

$runCli = 'param(
    [string]$Pdf,
    [string]$Dir,
    [string]$OutDir,
    [switch]$Recurse,
    [switch]$Overwrite,
    [string]$OutPdf,
    [string]$Pages,
    [int]$Threads = 2,
    [int]$Dpi = 300,
    [double]$MinScore = 0.55,
    [string]$Preset = "v5",
    [string]$Csv,
    [double]$ReviewScore = 0.75,
    [string]$FixCsv,
    [double]$MergeGap = 0.8,
    [switch]$Resume,
    [switch]$NoMask
)
$exe = Join-Path $PSScriptRoot "ocrgui.exe"
$modelDir = Join-Path $PSScriptRoot "models"
if (-not $Pdf -and -not $Dir) { throw "请用 -Pdf <文件> 或 -Dir <目录> 指定输入" }
$batch = [bool]$Dir
if ($batch) {
    $argList = @("--dir", $Dir, "--model-dir", $modelDir, "--threads", $Threads,
                 "--dpi", $Dpi, "--min-score", $MinScore, "--merge-gap", $MergeGap)
    if ($OutDir) { $argList += @("--out-dir", $OutDir) }
    if ($Recurse) { $argList += "--recurse" }
    if ($Overwrite) { $argList += "--overwrite" }
} else {
    $argList = @("--pdf", $Pdf, "--model-dir", $modelDir, "--threads", $Threads,
                 "--dpi", $Dpi, "--min-score", $MinScore, "--merge-gap", $MergeGap)
}
if ($Preset) { $argList += @("--preset", $Preset) }
if ($OutPdf -and -not $batch) { $argList += @("--out-pdf", $OutPdf) }
if ($Pages) { $argList += @("--pages", $Pages) }
if ($Csv) { $argList += @("--csv", $Csv, "--review-score", $ReviewScore) }
if ($FixCsv) { $argList += @("--fix-csv", $FixCsv) }
if ($Resume) { $argList += "--resume" }
if ($NoMask) { $argList += "--no-mask" }
Start-Process -FilePath $exe -ArgumentList $argList -Wait -WindowStyle Hidden'
Set-Content -LiteralPath (Join-Path $pkgDir 'run-cli.ps1') -Value $runCli -Encoding UTF8

$commit = ''
try { $commit = (& git -C $repoRoot rev-parse --short HEAD 2>$null) } catch { }
$buildLines = @(
    "ocrgui 构建信息",
    "================",
    "构建时间 : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')",
    "提交     : $commit",
    "模型     : $Models（none = 首次运行在线下载）",
    "运行环境 : Windows 10/11 x64（纯 CPU，ONNX Runtime，不需要显卡）"
)
Set-Content -LiteralPath (Join-Path $pkgDir 'BUILD.txt') -Value $buildLines -Encoding utf8

$files = Get-ChildItem -LiteralPath $pkgDir -Recurse -File
$sizeMb = [math]::Round(($files | Measure-Object Length -Sum).Sum / 1MB, 1)
Write-Host "打包完成：$pkgDir（$($files.Count) 个文件，$sizeMb MB）"

if (-not $NoZip) {
    $zip = Join-Path $OutDir "$pkgName.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -Path $pkgDir -DestinationPath $zip
    Write-Host "压缩包：$zip"
}
