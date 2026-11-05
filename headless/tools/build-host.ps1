<#
.SYNOPSIS
  Builds civhost.exe (headless host for the gamecore DLL).

.DESCRIPTION
  vcbuild headless\host\civhost.vcproj "Release|Win32" -> headless\out\host\Release\civhost.exe (+PDB).
  -Regen reruns the stub generator (headless\tools\civsim\enginehost\gen_stubs.py) first; the generated sources
  are committed, so a normal build needs no Python.
#>
param(
	[ValidateSet('Release')]
	[string]$Config = 'Release',
	[switch]$Clean,
	[switch]$Regen
)
$ErrorActionPreference = 'Stop'

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Proj = Join-Path $RepoRoot 'headless\host\civhost.vcproj'
$OutDir = Join-Path $RepoRoot "headless\out\host\$Config"
$ObjDir = Join-Path $RepoRoot "headless\out\host\obj\$Config"
$VcBuild = if ($env:VCBUILD) { $env:VCBUILD } else { 'C:\Program Files (x86)\Microsoft Visual Studio 9.0\VC\vcpackages\vcbuild.exe' }
if (-not (Test-Path $VcBuild)) { throw "vcbuild not found: $VcBuild" }

if ($Regen) {
	Push-Location (Join-Path $RepoRoot 'headless\tools')
	try {
		& python -m civsim.enginehost.gen_stubs
		if ($LASTEXITCODE -ne 0) { throw "gen_stubs failed" }
	} finally { Pop-Location }
}

if ($Clean) { Remove-Item -Recurse -Force $OutDir, $ObjDir -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force $OutDir, $ObjDir | Out-Null

# A just-run civhost.exe is sometimes still open (AV scan) and the link fails with LNK1104; a locked exe can be renamed.
$exePath = Join-Path $OutDir 'civhost.exe'
Get-ChildItem $OutDir -Filter 'civhost.exe.old-*' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
if (Test-Path $exePath) {
	try { [IO.File]::Open($exePath, 'Open', 'ReadWrite', 'None').Close() }
	catch { Rename-Item $exePath ("civhost.exe.old-{0}" -f [DateTime]::Now.Ticks) }
}

$log = Join-Path $ObjDir 'build.log'
$sw = [Diagnostics.Stopwatch]::StartNew()
$vcArgs = @($Proj, "$Config|Win32", '/nologo', "/logfile:$log")
if ($Clean) { $vcArgs += '/rebuild' }
& $VcBuild @vcArgs | Out-Null
$code = $LASTEXITCODE
$sw.Stop()
$exe = Join-Path $OutDir 'civhost.exe'
$summary = (Select-String -Path $log -Pattern '\d+ error\(s\), \d+ warning\(s\)' | Select-Object -Last 1).Line
if ($code -ne 0 -or -not (Test-Path $exe)) {
	Select-String -Path $log -Pattern ': (fatal )?error ' | Select-Object -First 30 | ForEach-Object { Write-Host $_.Line }
	throw "civhost build failed: $summary (log: $log)"
}
Write-Host ("civhost {0}  {1}s  {2}" -f $exe, [math]::Round($sw.Elapsed.TotalSeconds, 1), $summary)
