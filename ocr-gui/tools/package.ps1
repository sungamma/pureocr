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
    [string]$BuildDir = 'C:\Users\xwtal\build\ocrgui\Release',
    [string]$OutDir   = (Join-Path $PSScriptRoot '..\dist'),
    [switch]$NoZip
)

$ErrorActionPreference = 'Stop'

$repoRoot  = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$modelsDir = Join-Path $repoRoot 'ocr-gui\models'
$exe       = Join-Path $BuildDir 'ocrgui.exe'
$ortDll    = Join-Path $BuildDir 'onnxruntime.dll'

if (-not (Test-Path -LiteralPath $exe)) {
    throw "找不到 $exe，请先构建：cmake --build <build> --config Release"
}
if (-not (Test-Path -LiteralPath $modelsDir)) {
    throw "找不到模型目录 $modelsDir，请先运行：python ocr-gui/tools/fetch_models.py"
}

$stamp   = Get-Date -Format 'yyyyMMdd'
$pkgName = "ocrgui-$stamp"
$pkgDir  = Join-Path $OutDir $pkgName
if (Test-Path -LiteralPath $pkgDir) { Remove-Item -LiteralPath $pkgDir -Recurse -Force }
New-Item -ItemType Directory -Path $pkgDir -Force | Out-Null

Copy-Item -LiteralPath $exe -Destination $pkgDir
if (Test-Path -LiteralPath $ortDll) { Copy-Item -LiteralPath $ortDll -Destination $pkgDir }
Copy-Item -LiteralPath $modelsDir -Destination (Join-Path $pkgDir 'models') -Recurse

$readme = '
PDF OCR 工具（C++ 版）
=====================

双击 ocrgui.exe 打开界面：
  1) 「打开 PDF...」选择文件（输出会自动填成「原名_可复制文字.pdf」，可点「另存为...」修改）
  2) 需要时填「页范围」（如 1-10,15），留空表示全部
  3) 调整线程数（默认 4，机器卡就调到 2~3）、DPI（默认 300）、置信度阈值（默认 0.50）
  4) 点「开始识别」，进度条与日志实时刷新；完成后可「打开输出目录」
  5) 「预览」页签可看识别框是否贴合原文（绿框正常、红框低置信度，悬停显示识别文字）

命令行用法（PowerShell）：
  .\run-cli.ps1 -Pdf "D:\某文件.pdf"
  .\run-cli.ps1 -Pdf "D:\某文件.pdf" -Pages 1-10 -Threads 2
  .\run-cli.ps1 -Pdf "D:\某文件.pdf" -NoMask

说明：
- 输出文件保留原始页面渲染（逐像素不变），只叠加不可见的 OCR 文字层，可直接框选复制。
- 默认把原有（可能已损坏的）文字层屏蔽成零宽字符，保证复制出来的是 OCR 结果。
- 模型：PP-OCRv3（det/cls/rec），识别质量与 Python 版（rapidocr）一致。
'
Set-Content -LiteralPath (Join-Path $pkgDir '使用说明.txt') -Value $readme -Encoding UTF8

$runCli = '
param(
    [Parameter(Mandatory = $true)][string]$Pdf,
    [string]$OutPdf,
    [string]$Pages,
    [int]$Threads = 4,
    [int]$Dpi = 300,
    [double]$MinScore = 0.5,
    [switch]$NoMask
)
$exe = Join-Path $PSScriptRoot "ocrgui.exe"
$modelDir = Join-Path $PSScriptRoot "models"
$argList = @("--pdf", $Pdf, "--model-dir", $modelDir, "--threads", $Threads, "--dpi", $Dpi, "--min-score", $MinScore)
if ($OutPdf) { $argList += @("--out-pdf", $OutPdf) }
if ($Pages)  { $argList += @("--pages", $Pages) }
if ($NoMask) { $argList += "--no-mask" }
Start-Process -FilePath $exe -ArgumentList $argList -Wait -WindowStyle Hidden
'
Set-Content -LiteralPath (Join-Path $pkgDir 'run-cli.ps1') -Value $runCli -Encoding UTF8

$files = Get-ChildItem -LiteralPath $pkgDir -Recurse -File
$sizeMb = [math]::Round(($files | Measure-Object Length -Sum).Sum / 1MB, 1)
Write-Host "打包完成：$pkgDir（$($files.Count) 个文件，$sizeMb MB）"

if (-not $NoZip) {
    $zip = Join-Path $OutDir "$pkgName.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -Path (Join-Path $pkgDir '*') -DestinationPath $zip
    Write-Host "压缩包：$zip"
}
