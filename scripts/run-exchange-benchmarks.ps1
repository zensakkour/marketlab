[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build/benchmark',
    [ValidateRange(1, 1000000)][int]$Cycles = 100000,
    [ValidateRange(1, 100)][int]$Repetitions = 5,
    [ValidateSet('exchange', 'book', 'routed', 'all')][string]$Engine = 'exchange',
    [ValidateRange(1, 256)][int]$Instruments = 16,
    [ValidateSet('balanced', 'hot', 'all')][string]$Traffic = 'balanced',
    [string]$OutputPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'This metadata collector requires Windows. Run the executable directly on other systems.' }
if ($Engine -ne 'routed' -and ($PSBoundParameters.ContainsKey('Instruments') -or $PSBoundParameters.ContainsKey('Traffic'))) {
    throw 'Instruments and Traffic options require -Engine routed.'
}
if ($Engine -eq 'routed' -and $Instruments -eq 1 -and $Traffic -ne 'balanced') {
    throw 'Hot traffic requires at least two instruments.'
}
$repoRoot = Split-Path -Parent $PSScriptRoot
function Resolve-RepoPath([string]$Path) {
    if ([System.IO.Path]::IsPathRooted($Path)) { return [System.IO.Path]::GetFullPath($Path) }
    return [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($repoRoot, $Path))
}
$buildPath = Resolve-RepoPath $BuildDirectory
$cache = [System.IO.File]::ReadAllText((Join-Path $buildPath 'CMakeCache.txt'))
function Cache-Value([string]$Name) {
    $match = [regex]::Match($cache, '(?m)^' + [regex]::Escape($Name) + ':[^=]+=([^\r\n]*)')
    if (-not $match.Success) { throw "CMake cache entry missing: $Name" }
    return $match.Groups[1].Value
}
if ((Cache-Value 'CMAKE_BUILD_TYPE') -ne 'Release') { throw 'Configure a separate single-configuration Release build.' }
$cmake = Cache-Value 'CMAKE_COMMAND'
& $cmake --build $buildPath --target exchange_benchmark
if ($LASTEXITCODE -ne 0) { throw 'Benchmark build failed.' }
$compileCommands = Get-Content -LiteralPath (Join-Path $buildPath 'compile_commands.json') -Raw | ConvertFrom-Json
$compileEntry = @($compileCommands | Where-Object { $_.file.Replace('\', '/').EndsWith('/benchmarks/exchange_benchmark.cpp') })
if ($compileEntry.Count -ne 1) { throw 'Configure with CMAKE_EXPORT_COMPILE_COMMANDS=ON to record the exact compiler command.' }
$executable = Join-Path $buildPath 'exchange_benchmark.exe'
$commit = (& git -C $repoRoot rev-parse HEAD)
if ($LASTEXITCODE -ne 0) { throw 'Cannot read source commit.' }
$status = @(& git -C $repoRoot status --porcelain --untracked-files=normal)
if ($LASTEXITCODE -ne 0) { throw 'Cannot read source status.' }
$processors = @(Get-CimInstance Win32_Processor | ForEach-Object {
    [ordered]@{ name = $_.Name; cores = $_.NumberOfCores; logical_processors = $_.NumberOfLogicalProcessors }
})
$os = Get-CimInstance Win32_OperatingSystem
$machine = Get-CimInstance Win32_ComputerSystem
$created = [DateTime]::UtcNow

function Sample-Arguments([string]$Scenario, [bool]$Record, $Variant) {
    $sampleArguments = @($Scenario, [string]$Cycles)
    if ($Record) { $sampleArguments += '--record' }
    if ($Variant.engine -eq 'book') { $sampleArguments += '--book' }
    if ($Variant.engine -eq 'routed') { $sampleArguments += @('--instruments', [string]$Variant.instruments) }
    if ($Variant.traffic -eq 'hot') { $sampleArguments += '--skewed' }
    return $sampleArguments
}

function Invoke-Sample([string]$Scenario, [bool]$Record, $Variant) {
    $sampleArguments = @(Sample-Arguments $Scenario $Record $Variant)
    $json = & $executable @sampleArguments
    if ($LASTEXITCODE -ne 0) { throw "Benchmark failed: $Scenario record=$Record" }
    $result = ($json -join "`n") | ConvertFrom-Json
    if ($result.schema -ne 'marketlab.exchange-benchmark.v1' -or -not $result.checks_passed -or
        $result.scenario -ne $Scenario -or $result.cycles -ne $Cycles -or $result.record -ne $Record -or
        $result.engine -ne $Variant.engine -or $result.traffic -ne $Variant.traffic -or
        $result.build_config -ne 'Release' -or $result.workers -ne 1 -or $result.instruments -ne $Variant.instruments) {
        throw 'Unexpected benchmark output or configuration.'
    }
    if ($Variant.engine -eq 'routed' -and ($result.instrument_cycles.Count -ne $Variant.instruments -or
        ($result.instrument_cycles | Measure-Object -Sum).Sum -ne $Cycles)) {
        throw 'Unexpected per-instrument cycle counts.'
    }
    return $result
}

$samples = [System.Collections.Generic.List[object]]::new()
$engines = if ($Engine -eq 'all') { @('exchange', 'book') } else { @($Engine) }
$variants = foreach ($sampleEngine in $engines) {
    $patterns = if ($sampleEngine -ne 'routed') { @('single') } elseif ($Traffic -eq 'all') { @('balanced', 'hot') } else { @($Traffic) }
    foreach ($pattern in $patterns) {
        [pscustomobject]@{ engine = $sampleEngine; traffic = $pattern; instruments = $(if ($sampleEngine -eq 'routed') { $Instruments } else { 1 }) }
    }
}
foreach ($variant in $variants) {
    foreach ($scenario in @('match', 'cancel', 'sweep')) {
        foreach ($record in @($false, $true)) {
            Write-Host "engine=$($variant.engine) traffic=$($variant.traffic) instruments=$($variant.instruments) scenario=$scenario record=$record warmup=1 samples=$Repetitions cycles=$Cycles"
            $warmup = Invoke-Sample $scenario $record $variant
            for ($index = 1; $index -le $Repetitions; ++$index) {
                $arguments = @(Sample-Arguments $scenario $record $variant)
                $sample = Invoke-Sample $scenario $record $variant
                foreach ($field in @('requests', 'events', 'executions', 'volume_units', 'traded_tick_units',
                                     'record_bytes', 'record_fnv1a64', 'compiler_id', 'compiler_version')) {
                    if ($sample.$field -cne $warmup.$field) { throw "Repeated sample changed deterministic field: $field" }
                }
                if ($variant.engine -eq 'routed' -and ($sample.instrument_cycles -join ',') -cne ($warmup.instrument_cycles -join ',')) {
                    throw 'Repeated sample changed per-instrument cycle counts.'
                }
                $samples.Add([ordered]@{
                    repetition = $index
                    executable = $executable
                    arguments = $arguments
                    result = $sample
                })
            }
        }
    }
}
$document = [ordered]@{
    schema = 'marketlab.exchange-benchmark-run.v1'
    created_utc = $created.ToString('o')
    source_commit = $commit.Trim()
    source_dirty = $status.Count -ne 0
    processors = $processors
    physical_memory_bytes = [uint64]$machine.TotalPhysicalMemory
    os = [ordered]@{ name = $os.Caption; version = $os.Version; build = $os.BuildNumber; architecture = $os.OSArchitecture }
    compile = $compileEntry[0]
    cmake_generator = (Cache-Value 'CMAKE_GENERATOR')
    compiler = (Cache-Value 'CMAKE_CXX_COMPILER')
    build_config = 'Release'
    workers = 1
    instruments = $(if ($Engine -eq 'routed') { $Instruments } else { 1 })
    engines = @($engines)
    traffic_patterns = @($variants | ForEach-Object { $_.traffic } | Select-Object -Unique)
    seed = $null
    affinity = 'OS default'
    warmup_processes_per_variant = 1
    repetitions_per_variant = $Repetitions
    cycles_per_process = $Cycles
    samples = $samples.ToArray()
}
if ($OutputPath -eq '') { $OutputPath = 'build/benchmark/results-' + $created.ToString('yyyyMMddTHHmmssfffZ') + '.json' }
$outputFile = Resolve-RepoPath $OutputPath
[System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($outputFile)) | Out-Null
[System.IO.File]::WriteAllText($outputFile, (($document | ConvertTo-Json -Depth 10) + "`n"), [System.Text.UTF8Encoding]::new($false))
Write-Output $outputFile
