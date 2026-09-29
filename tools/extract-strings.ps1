# extract-strings.ps1 — 从二进制（V8 .jsc / DLL / EXE）中提取可打印 ASCII 字符串
# 用法:
#   .\extract-strings.ps1 -Path <文件> [-Min 6] [-Filter '正则']
# 说明: V8 字节码会把字符串常量保留在字符串表中，因此 .jsc 也能挖出命令名、
#       字段名、型号名等线索。

param(
    [Parameter(Mandatory = $true)][string]$Path,
    [int]$Min = 6,
    [string]$Filter,
    [switch]$Unique
)

if (-not (Test-Path -LiteralPath $Path)) { Write-Error "文件不存在: $Path"; exit 1 }

$bytes = [System.IO.File]::ReadAllBytes($Path)
$sb = [System.Text.StringBuilder]::new(256)
$results = [System.Collections.Generic.List[string]]::new()

foreach ($b in $bytes) {
    if ($b -ge 32 -and $b -lt 127) {
        [void]$sb.Append([char]$b)
    }
    else {
        if ($sb.Length -ge $Min) { $results.Add($sb.ToString()) }
        [void]$sb.Clear()
    }
}
if ($sb.Length -ge $Min) { $results.Add($sb.ToString()) }

if ($Filter) { $results = $results | Where-Object { $_ -match $Filter } }
if ($Unique) { $results = $results | Select-Object -Unique }

$results
