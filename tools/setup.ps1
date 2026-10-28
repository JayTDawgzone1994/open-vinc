param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Options
)

# Windows PowerShell 5.1 is the bootstrap dependency supplied by Windows.
# Functions are also dot-sourceable for offline policy/failure-path tests.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:SetupDirectory = $PSScriptRoot
$script:Repository = Split-Path -Parent $PSScriptRoot
$script:CheckOnly = $false
$script:WithUi = $true
$script:WithPython = $true
$script:Temporary = $null
$script:Python = $null
$script:DeveloperCommand = $null

function Show-Usage {
    Write-Host @'
Usage: tools\setup.bat [--check] [--no-ui] [--no-python]
Install/check dependencies, restore pinned submodules, and run npm ci.
No CMake configure, project build, tests, or browser downloads are performed.
--check       Check only; no installs, downloads, or dependency restoration.
--no-ui       Skip Node/npm and frontend packages.
--no-python   Skip Python SDK interpreter/development requirements.
VINC_SETUP_PYTHON selects a specific CPython executable.
'@
}

function Read-Options {
    param([string[]]$Arguments)
    foreach ($option in $Arguments) {
        switch ($option) {
            '--check' { $script:CheckOnly = $true }
            '--no-ui' { $script:WithUi = $false }
            '--no-python' { $script:WithPython = $false }
            '--help' {
                Show-Usage
                return $false
            }
            '-h' {
                Show-Usage
                return $false
            }
            default { throw "Unknown option: $option (use --help)" }
        }
    }
    return $true
}

function Invoke-Checked {
    param([string]$File, [string[]]$Arguments)
    # Windows PowerShell treats native stderr as ErrorRecords. Exit status,
    # rather than a harmless npm/compiler warning, determines command failure.
    $ErrorActionPreference = 'Continue'
    & $File @Arguments | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed ($LASTEXITCODE): $File $($Arguments -join ' ')"
    }
}

function Update-ToolPath {
    # Installers can update persisted PATH without changing this process.
    $paths = @(
        $env:Path,
        [Environment]::GetEnvironmentVariable('Path', 'Machine'),
        [Environment]::GetEnvironmentVariable('Path', 'User'),
        "$env:LOCALAPPDATA\Microsoft\WinGet\Links",
        "$env:ProgramFiles\CMake\bin",
        "$env:ProgramFiles\nodejs",
        "$env:ProgramFiles\Git\cmd"
    )
    $env:Path = ($paths | Where-Object { $_ } | Select-Object -Unique) -join ';'
}

function Install-Package {
    param([string]$Id, [string]$Override = '')
    if ($script:CheckOnly) {
        throw "Missing/incompatible dependency: $Id; rerun without --check"
    }

    if (-not (Get-Command winget.exe -ErrorAction SilentlyContinue)) {
        throw 'WinGet is unavailable. Install/update Microsoft App Installer, then rerun setup.'
    }

    $arguments = @('install', '--id', $Id, '--exact', '--source', 'winget',
        '--accept-source-agreements', '--accept-package-agreements', '--disable-interactivity')
    if ($Override) {
        $arguments += @('--override', $Override)
    }
    Invoke-Checked 'winget.exe' $arguments
    Update-ToolPath
}

function Test-ToolVersion {
    param([string]$Tool, [version]$Minimum)
    if (-not (Get-Command $Tool -ErrorAction SilentlyContinue)) {
        return $false
    }

    $output = & $Tool --version 2>&1
    if ($LASTEXITCODE -ne 0) {
        return $false
    }

    $match = [regex]::Match(($output -join "`n"), '\d+\.\d+\.\d+')
    return $match.Success -and ([version]$match.Value -ge $Minimum)
}

function Initialize-Tools {
    if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) {
        Install-Package 'Git.Git'
    }
    if (-not (Test-ToolVersion 'cmake.exe' ([version]'3.20.0'))) {
        Install-Package 'Kitware.CMake'
    }
    if (-not (Get-Command ninja.exe -ErrorAction SilentlyContinue)) {
        Install-Package 'Ninja-build.Ninja'
    }
    if (-not (Test-ToolVersion 'cmake.exe' ([version]'3.20.0'))) {
        throw 'CMake 3.20+ remains unavailable; check PATH or installer restart requirements.'
    }

    Invoke-Checked 'git.exe' @('--version')
    Invoke-Checked 'cmake.exe' @('--version')
    Invoke-Checked 'ninja.exe' @('--version')
}

function Find-DeveloperCommand {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path -LiteralPath $vswhere)) {
        return $null
    }

    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $installation) {
        return $null
    }

    $command = Join-Path ($installation | Select-Object -First 1) 'Common7\Tools\VsDevCmd.bat'
    if (Test-Path -LiteralPath $command) {
        return $command
    }
    return $null
}

function Import-DeveloperEnvironment {
    # The environment is local to setup. Never persist a selected compiler globally.
    $script:DeveloperCommand = Find-DeveloperCommand
    if (-not $script:DeveloperCommand) {
        Install-Package 'Microsoft.VisualStudio.2022.BuildTools' '--wait --passive --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'
        $script:DeveloperCommand = Find-DeveloperCommand
    }
    if (-not $script:DeveloperCommand) {
        throw 'MSVC C++ tools were not found; complete Visual Studio installation and rerun setup.'
    }

    $command = '"{0}" -no_logo -arch=x64 -host_arch=x64 >nul && set' -f $script:DeveloperCommand
    $environment = & $env:ComSpec /d /c $command
    if ($LASTEXITCODE -ne 0) {
        throw 'Unable to initialize the x64 MSVC/Windows SDK environment.'
    }

    foreach ($line in $environment) {
        if ($line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }
    foreach ($tool in @('cl.exe', 'link.exe', 'rc.exe', 'nmake.exe')) {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
            throw "MSVC/Windows SDK component is missing: $tool; modify the C++ workload and rerun."
        }
    }

    $source = Join-Path $script:SetupDirectory 'setup\toolchain.cpp'
    $object = Join-Path $script:Temporary 'toolchain.obj'
    $executable = Join-Path $script:Temporary 'toolchain.exe'
    Invoke-Checked 'cl.exe' @('/nologo', '/std:c++latest', '/EHsc', '/MD', '/utf-8', $source,
        "/Fo$object", "/Fe$executable")
    Write-Host 'MSVC/Windows SDK/C++23/64-bit/atomic/threads probe: PASS'
}

function Find-Python {
    $ErrorActionPreference = 'Continue'
    if ($env:VINC_SETUP_PYTHON) {
        return @($env:VINC_SETUP_PYTHON)
    }

    $candidates = @()
    if (Get-Command py.exe -ErrorAction SilentlyContinue) {
        $path = & py.exe -3 -c 'import sys; print(sys.executable)' 2>$null
        if ($LASTEXITCODE -eq 0 -and $path) {
            $candidates += $path
        }
    }
    $candidates += @(Get-Command python.exe -All -ErrorAction SilentlyContinue |
        Where-Object { $_.Source -notlike '*\WindowsApps\*' } |
        ForEach-Object { $_.Source })
    # WinGet CPython defaults to a per-user installation, with PATH changes
    # potentially taking effect only in a later terminal.
    $installed = "$env:LOCALAPPDATA\Programs\Python\Python313\python.exe"
    if (Test-Path -LiteralPath $installed) {
        $candidates += $installed
    }
    return @($candidates | Select-Object -Unique)
}

function Select-Python {
    $ErrorActionPreference = 'Continue'
    $probe = Join-Path $script:SetupDirectory 'setup\check_python.py'
    foreach ($candidate in @(Find-Python)) {
        if (-not (Get-Command $candidate -CommandType Application -ErrorAction SilentlyContinue)) {
            Write-Host "Python executable is unavailable: $candidate"
            continue
        }
        & $candidate $probe | Out-Host
        if ($LASTEXITCODE -eq 0) {
            return $candidate
        }
    }
    return $null
}

function Initialize-Python {
    if (-not $script:WithPython) {
        return
    }

    $script:Python = Select-Python
    if (-not $script:Python) {
        if ($env:VINC_SETUP_PYTHON) {
            throw 'Explicit VINC_SETUP_PYTHON is incompatible; install its matching development files.'
        }
        Install-Package 'Python.Python.3.13'
        $script:Python = Select-Python
    }
    if (-not $script:Python) {
        throw 'Compatible 64-bit Windows CPython and development files are unavailable.'
    }
}

function Test-Node {
    if (-not (Get-Command node.exe -ErrorAction SilentlyContinue)) {
        return $false
    }

    & node.exe -e "const [a,b]=process.versions.node.split('.').map(Number); process.exit((a===20&&b>=19)||(a===22&&b>=12)||a>22 ? 0 : 1)"
    return $LASTEXITCODE -eq 0
}

function Initialize-Node {
    if (-not $script:WithUi) {
        return
    }
    if (-not (Test-Node) -or -not (Get-Command npm.cmd -ErrorAction SilentlyContinue)) {
        Install-Package 'OpenJS.NodeJS.LTS'
    }
    if (-not (Test-Node)) {
        throw 'Node remains incompatible; check PATH or open a new terminal after installation.'
    }

    Invoke-Checked 'node.exe' @('--version')
    Invoke-Checked 'npm.cmd' @('--version')
}

function Initialize-Submodules {
    Invoke-Checked 'git.exe' @('-C', $script:Repository, 'rev-parse', '--is-inside-work-tree')
    $status = & git.exe -C $script:Repository submodule status --recursive
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not inspect pinned submodules.'
    }
    if ($status | Where-Object { $_ -match '^U' }) {
        throw 'Resolve submodule merge conflicts before setup.'
    }

    # Avoid embedded double quotes: Windows PowerShell 5.1 reparses them at
    # native argv boundaries before Git passes the expression to its shell.
    $cleanCheck = 'changes=$(git status --porcelain --untracked-files=normal) || exit 1; if test ${#changes} -ne 0; then echo Dirty submodule: $displaypath >&2; exit 1; fi'
    Invoke-Checked 'git.exe' @('-C', $script:Repository, 'submodule', 'foreach', '--quiet', '--recursive', $cleanCheck)
    if ($script:CheckOnly) {
        if ($status | Where-Object { $_ -match '^[-+]' }) {
            throw 'Submodules are missing or differ from pinned revisions.'
        }
    }
    else {
        Invoke-Checked 'git.exe' @('-C', $script:Repository, 'submodule', 'update', '--init', '--recursive')
    }
    Write-Host 'Pinned submodules: PASS'
}

function Initialize-Frontend {
    if (-not $script:WithUi) {
        return
    }

    $web = Join-Path $script:Repository 'src\vinc\ui\web'
    if ($script:CheckOnly) {
        $ErrorActionPreference = 'Continue'
        & npm.cmd --prefix $web ls --include=dev --all | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw 'Frontend dependencies are missing/incompatible; rerun setup without --check.'
        }
        Write-Host 'Frontend dependency tree: PASS'
    }
    else {
        Invoke-Checked 'npm.cmd' @('--prefix', $web, 'ci', '--include=dev', '--no-audit', '--no-fund')
    }
    Invoke-Checked 'node.exe' @((Join-Path $script:SetupDirectory 'setup\check_frontend.mjs'), $web)
}

function Invoke-Setup {
    param([string[]]$Arguments)
    try {
        if (-not (Read-Options $Arguments)) {
            return 0
        }
        if (-not [Environment]::Is64BitProcess -or $env:PROCESSOR_ARCHITECTURE -ne 'AMD64') {
            throw 'This setup entry supports native x64 Windows. Use 64-bit PowerShell on x64 Windows.'
        }

        $script:Temporary = Join-Path ([IO.Path]::GetTempPath()) ('open-vinc-setup-' + [guid]::NewGuid().ToString('N'))
        [void][IO.Directory]::CreateDirectory($script:Temporary)
        Write-Host "Open-VINC dependency setup (Windows x64; check-only=$script:CheckOnly)"
        Initialize-Tools
        Import-DeveloperEnvironment
        Initialize-Python
        Initialize-Node
        Initialize-Submodules
        Initialize-Frontend
        Write-Host 'Dependency checks passed. No project was configured or built.'
        Write-Host 'For your own builds, open an x64 Visual Studio Developer terminal, or run:'
        Write-Host ('call "{0}" -arch=x64 -host_arch=x64' -f $script:DeveloperCommand)
        if ($script:Python) {
            Write-Host "For Python SDK builds, select Python_EXECUTABLE=$script:Python"
        }
        return 0
    }
    catch {
        Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
        return 1
    }
    finally {
        if ($script:Temporary) {
            # Delete only our known probe artifacts; never recursively clean a
            # caller-supplied directory or any existing project build tree.
            foreach ($name in @('toolchain.obj', 'toolchain.exe')) {
                $file = Join-Path $script:Temporary $name
                if (Test-Path -LiteralPath $file) {
                    Remove-Item -LiteralPath $file
                }
            }
            [IO.Directory]::Delete($script:Temporary, $false)
        }
    }
}

if ($MyInvocation.InvocationName -ne '.') {
    exit (Invoke-Setup $Options)
}
