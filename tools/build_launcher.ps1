param([Parameter(ValueFromRemainingArguments = $true)][string[]]$BuildArguments)

# Windows' PowerShell selects Python before it is started. Profile fields are
# literal data, never dot-sourced or evaluated as commands.
$ErrorActionPreference = 'Stop'
$repository = Split-Path -Parent $PSScriptRoot
# Both current Python Manager and the legacy launcher can install on demand.
# Build only discovers installed interpreters; these affect this child process.
$env:PYTHON_MANAGER_AUTOMATIC_INSTALL = 'false'
$env:PYLAUNCHER_ALLOW_INSTALL = $null
$env:PYLAUNCHER_ALWAYS_INSTALL = $null

function Invoke-Python {
    param([string]$Executable, [string[]]$Arguments)
    # PowerShell 5.1's native argument binder drops embedded quotes. Encode CRT
    # arguments explicitly and inherit standard streams without piping output.
    $quoted = foreach ($argument in $Arguments) {
        if ($argument -ne '' -and $argument -notmatch '[\s"]') {
            $argument
            continue
        }
        $escaped = [regex]::Replace($argument, '(\\*)"', '$1$1\"')
        $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
        '"' + $escaped + '"'
    }
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = (Get-Command $Executable -CommandType Application -ErrorAction Stop).Source
    $start.Arguments = $quoted -join ' '
    $start.UseShellExecute = $false
    $child = [Diagnostics.Process]::Start($start)
    try {
        $child.WaitForExit()
        return $child.ExitCode
    }
    finally {
        $child.Dispose()
    }
}
try {
    $source = Join-Path $repository 'src'
    for ($index = 0; $index -lt $BuildArguments.Count; $index++) {
        if ($BuildArguments[$index] -eq '--source' -and $index + 1 -lt $BuildArguments.Count) {
            $index++
            $source = $BuildArguments[$index]
        } elseif ($BuildArguments[$index].StartsWith('--source=')) {
            $source = $BuildArguments[$index].Substring(9)
        }
    }
    $localSource = [IO.Path]::GetFullPath($source).TrimEnd('\','/') -eq (Join-Path $repository 'src')
    $python = $env:VINC_BUILD_INTERPRETER
    if (-not $python -and $localSource -and $BuildArguments -notcontains '--help' -and $BuildArguments -notcontains '-h') {
        $profile = Join-Path $repository '.vinc\toolchains\windows-x86_64.conf'
        if (Test-Path -LiteralPath $profile) {
            if ((Get-Item -LiteralPath $profile).Length -gt 16384) {
                throw "Toolchain profile exceeds size limit: $profile"
            }
            $fields = @{}
            foreach ($line in [IO.File]::ReadAllLines($profile, [Text.Encoding]::UTF8)) {
                $separator = $line.IndexOf('=')
                if ($separator -lt 1) {
                    throw "Malformed toolchain profile: $profile; rerun setup."
                }
                $key = $line.Substring(0, $separator)
                if ($fields.ContainsKey($key)) {
                    throw "Duplicate field in toolchain profile: $profile; rerun setup."
                }
                $fields[$key] = $line.Substring($separator + 1)
            }
            if ($fields['format'] -ne '1' -or $fields['platform'] -ne 'windows' -or $fields['arch'] -ne 'x86_64' -or -not $fields['build_python']) {
                throw "Invalid toolchain profile: $profile; rerun setup."
            }
            $python = $fields['build_python']
            if (-not [IO.Path]::IsPathRooted($python) -or -not (Test-Path -LiteralPath $python -PathType Leaf)) {
                throw "Remembered Python is unavailable: $python; rerun setup or set VINC_BUILD_INTERPRETER."
            }
        }
    }
    if ($python) {
        if (-not (Get-Command $python -CommandType Application -ErrorAction SilentlyContinue)) {
            throw "Python was not found: $python. Run tools\setup.bat or correct VINC_BUILD_INTERPRETER."
        }
        exit (Invoke-Python $python (@('-B', (Join-Path $repository 'build.py')) + $BuildArguments))
    }
    if (Get-Command py.exe -ErrorAction SilentlyContinue) {
        # Missing runtimes are a failed discovery probe, not a terminating
        # PowerShell stderr exception; PATH discovery may still succeed.
        $savedPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            & py.exe -3 -c 'import sys' 2>$null
            $probeStatus = $LASTEXITCODE
        }
        finally {
            $ErrorActionPreference = $savedPreference
        }
        if ($probeStatus -eq 0) {
            exit (Invoke-Python 'py.exe' (@('-3', '-B', (Join-Path $repository 'build.py')) + $BuildArguments))
        }
    }
    if (Get-Command python.exe -ErrorAction SilentlyContinue) {
        exit (Invoke-Python 'python.exe' (@('-B', (Join-Path $repository 'build.py')) + $BuildArguments))
    }
    throw 'Python was not found. Run tools\setup.bat or set VINC_BUILD_INTERPRETER.'
} catch {
    [Console]::Error.WriteLine('Build failed: ' + $_.Exception.Message)
    exit 1
}
