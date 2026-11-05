<#
.SYNOPSIS
  Single entry point for building gamecore DLL variants.

.DESCRIPTION
  Generates a per-variant copy of CvGameCoreDLL_Expansion2.vcproj (Release|Win32 settings, golden compiler
  flags untouched) with its own output/intermediate directories and extra preprocessor defines, builds it with
  vcbuild and writes headless\out\dll\<variant>\manifest.json.

  Variants:
    golden          sources as-is (+PDB, explicit /OPT:REF /OPT:ICF)
    diag            golden + LEKMOD_HEADLESS_DIAG
    optimized       golden + flags of -FlagSet (headless\improvements\flagsets.json)
    optimized-diag  optimized + LEKMOD_HEADLESS_DIAG (+ LEKMOD_HEADLESS_OPT_VERIFY)
    profile         golden + /Oy- (frame pointers) for the sampler
#>
param(
	[ValidateSet('golden', 'diag', 'optimized', 'optimized-diag', 'profile', 'all')]
	[string]$Variant = 'golden',
	[ValidateSet('opt', 'fix', 'release', 'hyg', 'none')]   # hyg = hygiene only: golden behaviour, digest-noise-free baseline
	[string]$FlagSet = 'opt',
	[string[]]$Only = @(),          # restrict the flag set to these entry ids (e.g. OPT-0003)
	[string[]]$ExtraDefines = @(),  # test-only: extra defines (bisect-flags passes explicit flag lists)
	[string]$OutName = '',          # override output dir name (default: variant[-flagset])
	[switch]$Clean
)
$ErrorActionPreference = 'Stop'

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$ProjDir = Join-Path $RepoRoot 'LEKMOD_DLL\CvGameCoreDLL_Expansion2'
$SrcProj = Join-Path $ProjDir 'CvGameCoreDLL_Expansion2.vcproj'
$VcBuild = if ($env:VCBUILD) { $env:VCBUILD } else { 'C:\Program Files (x86)\Microsoft Visual Studio 9.0\VC\vcpackages\vcbuild.exe' }
$FlagSetsFile = Join-Path $RepoRoot 'headless\improvements\flagsets.json'

if (-not (Test-Path $VcBuild)) { throw "vcbuild not found: $VcBuild (Visual Studio 2008 / VC9 is required; set VCBUILD to its vcbuild.exe, see headless/README.md)" }

function Get-FlagDefines([string]$Set, [string[]]$OnlyIds) {
	if ($Set -eq 'none' -or -not (Test-Path $FlagSetsFile)) { return @() }
	$fs = Get-Content $FlagSetsFile -Raw | ConvertFrom-Json
	$entries = @($fs.sets.$Set)
	if ($OnlyIds.Count -gt 0) { $entries = @($entries | Where-Object { $OnlyIds -contains $_.id }) }
	return @($entries | ForEach-Object { $_.flag })
}

function Build-Variant([string]$V) {
	$defines = @()
	$omitFp = $true
	$flags = @()
	switch ($V) {
		'golden' { }
		'diag' { $defines += 'LEKMOD_HEADLESS_DIAG' }
		'optimized' { $flags = Get-FlagDefines $FlagSet $Only }
		'optimized-diag' { $flags = Get-FlagDefines $FlagSet $Only; $defines += 'LEKMOD_HEADLESS_DIAG'; $defines += 'LEKMOD_HEADLESS_OPT_VERIFY' }
		'profile' { $omitFp = $false }
	}
	$defines += $flags
	if ($V -ne 'golden' -and $V -ne 'profile') { $defines = @('LEKMOD_HEADLESS_BUILD') + $defines }
	$defines += $ExtraDefines

	$name = if ($OutName) { $OutName } elseif ($V -like 'optimized*') { "$V-$FlagSet" + $(if ($Only.Count) { '-' + ($Only -join '+') } else { '' }) } else { $V }
	$OutDir = Join-Path $RepoRoot "headless\out\dll\$name"
	$IntDir = Join-Path $RepoRoot "headless\out\obj\$name"
	if ($Clean) { Remove-Item -Recurse -Force $OutDir, $IntDir -ErrorAction SilentlyContinue }
	New-Item -ItemType Directory -Force $OutDir, $IntDir | Out-Null

	# Variant project: Release|Win32 only, golden compiler flags, per-variant dirs and defines.
	[xml]$x = Get-Content $SrcProj -Raw -Encoding Default
	$cfgs = $x.VisualStudioProject.Configurations
	foreach ($c in @($cfgs.Configuration)) { if ($c.Name -ne 'Release|Win32') { [void]$cfgs.RemoveChild($c) } }
	$cfg = $cfgs.Configuration
	$cfg.OutputDirectory = "$OutDir\"
	$cfg.IntermediateDirectory = "$IntDir\"
	$cl = $cfg.Tool | Where-Object { $_.Name -eq 'VCCLCompilerTool' }
	$ln = $cfg.Tool | Where-Object { $_.Name -eq 'VCLinkerTool' }
	if ($defines.Count) { $cl.PreprocessorDefinitions = $cl.PreprocessorDefinitions + ';' + ($defines -join ';') }
	$cl.EnablePREfast = 'false'                       # /analyze is ignored by Express anyway (D9040 noise)
	# profile: frame pointers for the sampler. OmitFramePointers="false" alone emits no /Oy- next to /Ox, so add it explicitly (cl: last option wins).
	if (-not $omitFp) { $cl.OmitFramePointers = 'false'; $cl.AdditionalOptions = ($cl.AdditionalOptions + ' /Oy-').Trim() }
	$ln.GenerateDebugInformation = 'true'             # PDB; /OPT:REF (2) and /OPT:ICF (2) stay explicit
	$ln.ProgramDatabaseFile = '$(OutDir)/$(TargetName).pdb'
	$VarProj = Join-Path $ProjDir "CvGameCoreDLL_Expansion2.hl-$name.vcproj"
	$x.Save($VarProj)

	$log = Join-Path $OutDir 'build.log'
	$sw = [Diagnostics.Stopwatch]::StartNew()
	$vcArgs = @($VarProj, 'Release|Win32', '/nologo', "/logfile:$log")
	# vcbuild misses header edits that only reach the PCH: force a rebuild when any header or the define set changed.
	$stampFile = Join-Path $IntDir 'headers.stamp'
	$stamp = ($defines -join ';') + '|' + ((Get-ChildItem $ProjDir -Recurse -Include *.h, *.inl -File | Sort-Object FullName | ForEach-Object { "$($_.FullName)=$($_.LastWriteTimeUtc.Ticks)" }) -join ';')
	$stampHash = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($stamp))).Replace('-', '')
	$rebuild = $Clean -or -not (Test-Path $stampFile) -or ((Get-Content $stampFile -Raw).Trim() -ne $stampHash)
	if ($rebuild) { $vcArgs += '/rebuild' }
	& $VcBuild @vcArgs | Out-Null
	$code = $LASTEXITCODE
	$sw.Stop()
	$dll = Join-Path $OutDir 'CvGameCore_Expansion2.dll'
	$summary = (Select-String -Path $log -Pattern '\d+ error\(s\), \d+ warning\(s\)' | Select-Object -Last 1).Line
	if ($code -ne 0 -or -not (Test-Path $dll)) {
		Select-String -Path $log -Pattern ' error ' | Select-Object -First 20 | ForEach-Object { Write-Host $_.Line }
		throw "build failed ($V): $summary (log: $log)"
	}
	Set-Content $stampFile $stampHash

	$sha = git -C $RepoRoot rev-parse HEAD
	$dirty = [bool](git -C $RepoRoot status --porcelain -- LEKMOD_DLL)
	$manifest = [ordered]@{
		schema       = 1
		variant      = $V
		flagset      = $(if ($V -like 'optimized*') { $FlagSet } else { $null })
		flags        = $flags
		defines      = $defines
		git_sha      = $sha
		git_dirty    = $dirty
		dll          = 'CvGameCore_Expansion2.dll'
		dll_sha256   = (Get-FileHash $dll -Algorithm SHA256).Hash.ToLower()
		pdb          = $(if (Test-Path (Join-Path $OutDir 'CvGameCore_Expansion2.pdb')) { 'CvGameCore_Expansion2.pdb' } else { $null })
		compiler     = 'VC++ 2008 SP1 (cl 15.00.30729)'
		build_seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1)
		build_summary = $summary
	}
	$manifest | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $OutDir 'manifest.json') -Encoding UTF8
	Write-Host ("{0,-28} {1}  {2}s  {3}" -f $name, $manifest.dll_sha256.Substring(0, 16), $manifest.build_seconds, $summary)
}

$variants = if ($Variant -eq 'all') { @('golden', 'diag') } else { @($Variant) }
foreach ($v in $variants) { Build-Variant $v }

