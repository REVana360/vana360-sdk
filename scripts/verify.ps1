Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$python = Get-Command python -ErrorAction Stop
$clangFormat = Get-Command clang-format -ErrorAction Stop
$ruff = Get-Command ruff -ErrorAction Stop

$versionOutput = @(& $clangFormat.Source --version 2>&1)
if ($LASTEXITCODE -ne 0 -or
    ($versionOutput -join "`n") -notmatch 'clang-format version 22\.') {
    throw "clang-format 22.x is required: $($versionOutput -join ' ')"
}

$requiredRuffVersion = '0.15.21'
$ruffVersionOutput = @(& $ruff.Source --version 2>&1)
if ($LASTEXITCODE -ne 0 -or
    ($ruffVersionOutput -join "`n").Trim() -cne "ruff $requiredRuffVersion") {
    throw "Ruff $requiredRuffVersion is required: $($ruffVersionOutput -join ' ')"
}

$sourceFiles = @(git -C $repo ls-files -- include src tests |
    Where-Object { $_ -match '(?i)\.(c|cpp|h|hpp)$' } |
    ForEach-Object { Join-Path $repo $_ })
if ($LASTEXITCODE -ne 0 -or $sourceFiles.Count -eq 0) {
    throw 'git ls-files returned no C or C++ source files'
}

$batchSize = 100
for ($offset = 0; $offset -lt $sourceFiles.Count; $offset += $batchSize) {
    $last = [Math]::Min($offset + $batchSize - 1, $sourceFiles.Count - 1)
    $formatOutput = @(& $clangFormat.Source --dry-run --Werror `
        @($sourceFiles[$offset..$last]) 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "clang-format failed: $($formatOutput -join ' ')"
    }
}

$pythonFiles = @(git -C $repo ls-files -- scripts |
    Where-Object { $_ -match '(?i)\.py$' } |
    ForEach-Object { Join-Path $repo $_ })
if ($LASTEXITCODE -ne 0 -or $pythonFiles.Count -eq 0) {
    throw 'git ls-files returned no Python source files'
}

$ruffOutput = @(& $ruff.Source check --no-cache @pythonFiles 2>&1)
if ($LASTEXITCODE -ne 0) {
    throw "Ruff lint failed: $($ruffOutput -join ' ')"
}
$ruffOutput = @(& $ruff.Source format --check --no-cache @pythonFiles 2>&1)
if ($LASTEXITCODE -ne 0) {
    throw "Ruff format failed: $($ruffOutput -join ' ')"
}

& $python.Source (Join-Path $PSScriptRoot 'check_vulkan_stack.py')
if ($LASTEXITCODE -ne 0) {
    throw 'Vulkan dependency check failed'
}

& $python.Source -B -m pytest (Join-Path $PSScriptRoot 'tests') -q
if ($LASTEXITCODE -ne 0) {
    throw 'Python tests failed'
}

& $python.Source (Join-Path $PSScriptRoot 'check_commit_subject.py')
if ($LASTEXITCODE -ne 0) {
    throw 'commit subject check failed'
}

foreach ($cached in @($false, $true)) {
    $arguments = @('-C', $repo, 'diff')
    if ($cached) {
        $arguments += '--cached'
    }
    $arguments += '--check'
    $diffOutput = @(& git @arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw "git diff --check failed: $($diffOutput -join ' ')"
    }
}

Write-Output "verify: passed files=$($sourceFiles.Count) clang-format=22 Ruff=$requiredRuffVersion Vulkan=1 Python-tests=1 commit-subject=1 git-whitespace=1"
