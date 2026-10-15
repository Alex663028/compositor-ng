# compositor-ng 构建入口（Windows PowerShell）
#   .\scripts\build.ps1                编译并运行测试
#   .\scripts\build.ps1 --no-run       只编译
#   $env:COMP_DRIVER="cc"; .\scripts\build.ps1   改用系统 cc/c++
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Rest
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$PythonBin = if ($env:PYTHON_BIN) { $env:PYTHON_BIN } else { "python" }
$Driver = if ($env:COMP_DRIVER) { $env:COMP_DRIVER } else { "zig" }

Push-Location $Root
try {
    & $PythonBin "scripts/build.py" "--driver" $Driver @Rest
    exit $LASTEXITCODE
}
finally {
    Pop-Location
}
