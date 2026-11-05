<#
.SYNOPSIS
    Runs ESP-IDF v6.1 commands with the toolchain environment created by the
    ESP-IDF Installation Manager (EIM).

.DESCRIPTION
    The EIM install keeps its environment in an activation script instead of the
    system PATH, which is why "idf.py" is not a global command and why
    export.ps1 can pick up the wrong python.exe (the Windows Store stub).

    This wrapper loads exactly the environment the ESP-IDF VS Code extension
    uses, then runs idf.py with the project's own virtual environment. It works
    from any terminal and is what .vscode/tasks.json calls.

.EXAMPLE
    .\idf.ps1 build
    .\idf.ps1 -p COM5 flash monitor
    .\idf.ps1 fullclean
#>
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $IdfArgs
)

$ErrorActionPreference = 'Stop'

$activation = if ($env:ESP_IDF_ACTIVATION_SCRIPT) {
    $env:ESP_IDF_ACTIVATION_SCRIPT
} else {
    'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1'
}

if (-not (Test-Path -LiteralPath $activation)) {
    throw "ESP-IDF activation script not found: $activation"
}

# The activation script supports -e, which prints KEY=VALUE pairs instead of
# mutating the current shell. That is the same mechanism the ESP-IDF extension
# uses, and it avoids the slow python-env activation and tab-completion setup.
$powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$originalPath = $env:PATH

$envLines = & $powershell -ExecutionPolicy Bypass -NoProfile -File $activation -e
foreach ($line in $envLines) {
    $separator = $line.IndexOf('=')
    if ($separator -le 0) { continue }

    $name = $line.Substring(0, $separator)
    if ($name -notmatch '^[A-Za-z_][A-Za-z0-9_]*$') { continue }

    Set-Item -Path "env:$name" -Value $line.Substring($separator + 1)
}

# The exported PATH only contains the toolchain directories, so keep the system
# entries (git, powershell, etc.) after them.
if ($env:PATH) {
    $env:PATH = "$($env:PATH);$originalPath"
}

$python = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
$idfPy  = Join-Path $env:IDF_PATH 'tools\idf.py'

if (-not (Test-Path -LiteralPath $python)) {
    throw "ESP-IDF python not found: $python (check IDF_PYTHON_ENV_PATH)"
}
if (-not (Test-Path -LiteralPath $idfPy)) {
    throw "idf.py not found: $idfPy (check IDF_PATH)"
}

# idf.py warns loudly when the console is not UTF-8 capable; this silences it.
if (-not $env:PYTHONUTF8) { $env:PYTHONUTF8 = '1' }

# Run in the ESP-IDF project that contains the current directory (or in this
# project when the shell is somewhere unrelated such as C:\.platformio).
$projectDir = $null
$probe = (Get-Location).Path
for ($depth = 0; $depth -lt 6 -and $probe; $depth++) {
    if (Test-Path -LiteralPath (Join-Path $probe 'CMakeLists.txt')) {
        $projectDir = $probe
        break
    }
    $parent = Split-Path -Parent $probe
    if (-not $parent -or $parent -eq $probe) { break }
    $probe = $parent
}
if (-not $projectDir) { $projectDir = $PSScriptRoot }

Push-Location -LiteralPath $projectDir
try {
    if (-not $IdfArgs -or $IdfArgs.Count -eq 0) {
        $IdfArgs = @('--help')
    }
    & $python $idfPy @IdfArgs
    $exitCode = $LASTEXITCODE
} finally {
    Pop-Location
}

exit $exitCode
