# Ceratek lathe touchscreen firmware: one script for every step (Windows 10/11).
# Run it through lathe.bat from a Command Prompt in this folder:
#
#   lathe setup               install PlatformIO and download FluidNC v4.1.1 into X:\ctk (once, needs internet)
#   lathe build [wifi]        build the firmware (no Wi-Fi by default; "wifi" keeps the web page)
#   lathe flash [wifi]        build and write firmware + config_screen.yaml to the board
#   lathe config [wifi]       write only the config (after editing config\config_screen.yaml)
#   lathe monitor             open the serial console (type $SS, $Heap ...; Ctrl+C to leave)
#   lathe restore             put back stock FluidNC v4.1.1 (Wi-Fi) with config_stock.yaml
#   lathe test                run the PC tests (only if g++ is installed; optional)
#
#   lathe flash -Port COM5    picks the serial port when there is more than one

param(
    [string]$Command = "",
    [string]$Option = "",
    [string]$Port = ""
)

$ErrorActionPreference = "Stop"
$Here    = Split-Path -Parent $MyInvocation.MyCommand.Path

# Build files (PlatformIO, the FluidNC source, the ESP32 compiler, about 2 GB) go in a short
# folder at the root of the same drive, e.g. J:\ctk, wherever this project folder is. FluidNC's
# build makes paths about 150 characters deep, and Windows tools fail past 260 characters.
# Set CERATEK_WORK to use another short folder.
if ($env:CERATEK_WORK) {
    $Work = $env:CERATEK_WORK
} else {
    $Work = (Split-Path -Qualifier $Here) + "\ctk"
}
$FluidNC = Join-Path $Work "FluidNC"
$Venv    = Join-Path $Work "venv"
$Pio     = Join-Path $Venv "Scripts\pio.exe"
$Tag     = "v4.1.1"

# Keep PlatformIO's tools inside this folder. The default place is under the Windows user
# folder, and a user name with Chinese letters or spaces breaks the ESP32 compiler.
$env:PLATFORMIO_CORE_DIR = Join-Path $Work "platformio"

# FluidNC's build runs "python git-version.py". Put this folder's own Python first, so that
# "python" is not the Microsoft Store shortcut Windows 10 has in its place.
$env:PATH = (Join-Path $Venv "Scripts") + ";" + $env:PATH

function Say([string]$Message) {
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Die([string]$Message) {
    Write-Host ""
    Write-Host "ERROR: $Message" -ForegroundColor Red
    exit 1
}

function Check-Path {
    if ($Work -match '[^\x00-\x7F]' -or $Work -match ' ' -or $Work.Length -gt 30) {
        Die "The build folder must be short, with no spaces or non-English letters: $Work`nChoose another one, for example:  set CERATEK_WORK=C:\ctk"
    }
}

function Env-Name([string]$Opt) {
    if ($Opt -eq "") { return "ceratek" }
    if ($Opt -eq "wifi") { return "ceratek_wifi" }
    Die "unknown option '$Opt' (use nothing, or 'wifi')"
}

# Adds the serial port to a PlatformIO command when -Port was given.
function With-Port([string[]]$PioArgs) {
    if ($Port -ne "") { return ($PioArgs + @("--upload-port", $Port)) }
    return $PioArgs
}

function Run-Pio([string[]]$PioArgs) {
    Push-Location $FluidNC
    try {
        & $Pio @PioArgs
        if ($LASTEXITCODE -ne 0) { Die "PlatformIO stopped with an error (see the lines above)." }
    } finally {
        Pop-Location
    }
}

function Need-Setup {
    if (-not (Test-Path $Pio) -or -not (Test-Path (Join-Path $FluidNC ".git"))) {
        Die "run 'lathe setup' first (build folder: $Work)"
    }
}

# Copy the screen module into the FluidNC checkout, and the chosen config into its file-system image.
function Prepare([string]$Config) {
    & robocopy (Join-Path $Here "fluidnc") $FluidNC /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { Die "copying the screen module into FluidNC failed" }
    Copy-Item -Force $Config (Join-Path $FluidNC "FluidNC\data\config.yaml")
}

function Find-Python {
    foreach ($candidate in @("py", "python", "python3")) {
        if (-not (Get-Command $candidate -ErrorAction SilentlyContinue)) { continue }
        $pre = @()
        if ($candidate -eq "py") { $pre = @("-3") }
        try {
            $version = & $candidate @pre --version 2>&1
        } catch {
            continue
        }
        if ($LASTEXITCODE -eq 0 -and "$version" -match "Python 3\.(\d+)") {
            if ([int]$Matches[1] -ge 8) {
                return @{ Exe = $candidate; Pre = $pre }
            }
        }
    }
    return $null
}

function Cmd-Setup {
    Check-Path
    $py = Find-Python
    if ($null -eq $py) { Die "Python 3 was not found. Install it from https://www.python.org/downloads/ and tick 'Add python.exe to PATH'." }
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Die "Git was not found. Install it from https://git-scm.com/download/win (all default options)." }
    New-Item -ItemType Directory -Force -Path $Work | Out-Null
    Say "Build files go in $Work (about 2 GB; delete this folder when the project is over)"
    if (-not (Test-Path $Pio)) {
        Say "Installing PlatformIO into $Venv"
        $pyExe = $py.Exe
        $pyPre = $py.Pre
        & $pyExe @pyPre -m venv $Venv
        if ($LASTEXITCODE -ne 0) { Die "could not create the Python environment" }
        $vpy = Join-Path $Venv "Scripts\python.exe"
        & $vpy -m pip install --upgrade pip
        & $vpy -m pip install "platformio>=6.1,<7"
        if ($LASTEXITCODE -ne 0) { Die "installing PlatformIO failed (is the internet connected?)" }
    }
    if (-not (Test-Path (Join-Path $FluidNC ".git"))) {
        Say "Downloading FluidNC $Tag"
        & git -c core.autocrlf=false clone --depth 1 --branch $Tag https://github.com/bdring/FluidNC $FluidNC
        if ($LASTEXITCODE -ne 0) { Die "downloading FluidNC failed (is the internet connected?)" }
    }
    Say "Setup done. Next: lathe build"
}

function Cmd-Build([string]$Opt) {
    Check-Path
    Need-Setup
    $envName = Env-Name $Opt
    Prepare (Join-Path $Here "config\config_screen.yaml")
    Say "Building $envName (the first build downloads the ESP32 tools and takes 10-20 minutes)"
    Run-Pio @("run", "-e", $envName)
    $out = Join-Path $Here "output"
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    Copy-Item -Force (Join-Path $FluidNC ".pio\build\$envName\firmware.bin") (Join-Path $out "firmware-$envName.bin")
    Say "Built: output\firmware-$envName.bin"
}

function Cmd-Flash([string]$Opt) {
    Check-Path
    Need-Setup
    $envName = Env-Name $Opt
    Prepare (Join-Path $Here "config\config_screen.yaml")
    Say "Writing the firmware ($envName). Keep the 12 V supply and USB connected."
    Run-Pio (With-Port @("run", "-e", $envName, "-t", "upload"))
    Say "Writing the file system with config_screen.yaml"
    Run-Pio (With-Port @("run", "-e", $envName, "-t", "uploadfs"))
    Say "Done. Unplug the 12 V supply for 5 seconds and plug it back in, then follow the guide."
}

function Cmd-Config([string]$Opt) {
    Check-Path
    Need-Setup
    $envName = Env-Name $Opt
    Prepare (Join-Path $Here "config\config_screen.yaml")
    Say "Writing only the file system with config_screen.yaml"
    Run-Pio (With-Port @("run", "-e", $envName, "-t", "uploadfs"))
    Say "Done. Restart the controller (unplug 12 V for 5 seconds)."
}

function Cmd-Monitor {
    Need-Setup
    $monitorArgs = @("device", "monitor", "-b", "115200", "--eol", "LF", "--echo")
    if ($Port -ne "") { $monitorArgs += @("-p", $Port) }
    & $Pio @monitorArgs
}

function Cmd-Restore {
    Check-Path
    Need-Setup
    Prepare (Join-Path $Here "config\config_stock.yaml")
    Say "Building and writing stock FluidNC $Tag (Wi-Fi), the firmware tested on 2026-11-14"
    Run-Pio (With-Port @("run", "-e", "stock_wifi", "-t", "upload"))
    Run-Pio (With-Port @("run", "-e", "stock_wifi", "-t", "uploadfs"))
    Say "Stock FluidNC is back with config_stock.yaml. The screen will stay dark, as before."
}

function Cmd-Test {
    if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
        Say "g++ is not installed, so the PC tests are skipped. They are optional: they passed (40 tests) when the package was made."
        return
    }
    $tests = Join-Path $Here "host-tests"
    $lathe = Join-Path $Here "fluidnc\FluidNC\lathe"
    $core  = Get-ChildItem (Join-Path $lathe "core\*.cpp") | ForEach-Object { $_.FullName }
    & g++ -std=c++17 -O1 "-I$tests" "-I$lathe" (Join-Path $tests "test_main.cpp") @core -o (Join-Path $tests "lathe_tests.exe")
    if ($LASTEXITCODE -ne 0) { Die "the tests did not compile" }
    & (Join-Path $tests "lathe_tests.exe") (Join-Path $tests "vectors.txt")
}

switch ($Command) {
    "setup"   { Cmd-Setup }
    "build"   { Cmd-Build $Option }
    "flash"   { Cmd-Flash $Option }
    "config"  { Cmd-Config $Option }
    "monitor" { Cmd-Monitor }
    "restore" { Cmd-Restore }
    "test"    { Cmd-Test }
    default   { Get-Content $MyInvocation.MyCommand.Path -TotalCount 13 | Select-Object -Skip 1 | ForEach-Object { $_ -replace '^# ?', '' } }
}
