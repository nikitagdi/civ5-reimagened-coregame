<#
.SYNOPSIS
  Live smoke test of the LEKMOD_HEADLESS_DIAG RNG hooks: golden vs diag (no sink / sink / refused sink).
  Needs the game folder (for lua51_Win32.dll and the two database/localization DLLs) and DLLs built by build-dll.ps1.
.EXAMPLE
  pwsh headless\tests\diag_smoke\run.ps1 -Golden headless\out\dll\golden\CvGameCore_Expansion2.dll -Diag headless\out\dll\diag\CvGameCore_Expansion2.dll
#>
param(
	[string]$Golden = 'headless\out\dll\golden\CvGameCore_Expansion2.dll',
	[string]$Diag = 'headless\out\dll\diag\CvGameCore_Expansion2.dll',
	[string]$GameDir = $(if ($env:CIV5_GAME_DIR) { $env:CIV5_GAME_DIR } else { "C:\Program Files (x86)\Steam\steamapps\common\Sid Meier's Civilization V" })
)
$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$Sol = Join-Path $Repo 'LEKMOD_DLL\CvGameCoreDLL_Expansion2'
$vc = 'C:\Program Files (x86)\Microsoft Visual Studio 9.0'
$sdk = 'C:\Program Files\Microsoft SDKs\Windows\v6.0A'
$out = Join-Path $Repo 'headless\out\tests\diag_smoke'
New-Item -ItemType Directory -Force $out | Out-Null
$gold = (Resolve-Path $Golden).Path; $diag = (Resolve-Path $Diag).Path		# before Push-Location: paths are relative to the caller

$env:INCLUDE = "$vc\VC\include;$sdk\Include;$Sol\CvGameCoreDLLUtil\include;$Sol\CvLocalization\include;$Sol\CvGameDatabase\include;$Sol\FirePlace\include;$Sol\FirePlace\include\FireWorks;$Sol"
$env:LIB = "$vc\VC\lib;$sdk\Lib"
$env:PATH = "$vc\VC\bin;$vc\Common7\IDE;$env:PATH"
Push-Location $out
try {
	& "$vc\VC\bin\cl.exe" /nologo /EHsc /MD /O2 /DWIN32 /D_WIN32_WINNT=0x0600 /DLEKMOD_HEADLESS_DIAG /DFXS_IS_DLL /DFINAL_RELEASE /D_CRT_SECURE_NO_WARNINGS `
		(Join-Path $PSScriptRoot 'diag_smoke.cpp') /Fediag_smoke.exe /Fodiag_smoke.obj /link /nologo | Out-Null
	if ($LASTEXITCODE -ne 0 -or -not (Test-Path .\diag_smoke.exe)) { throw 'diag_smoke build failed' }

	$hashes = @{}
	foreach ($run in @(@('golden', $gold, 'nosink'), @('diag', $diag, 'nosink'), @('diag', $diag, 'sink'), @('diag', $diag, 'badsink'))) {
		$txt = & .\diag_smoke.exe $run[1] $run[2] $GameDir
		$code = $LASTEXITCODE
		Write-Host ("{0,-7} {1,-8} exit={2}  {3}" -f $run[0], $run[2], $code, (($txt | Where-Object { $_ -match 'hash|events|FAIL|failed' }) -join ' | '))
		if ($code -ne 0) { throw "smoke run failed: $($run[0]) $($run[2])" }
		$hashes["$($run[0])/$($run[2])"] = ($txt | Where-Object { $_ -match 'results hash' })
	}
	if (@($hashes.Values | Select-Object -Unique).Count -ne 1) { throw "RNG results differ between golden and diag: $($hashes | Out-String)" }
	Write-Host 'diag_smoke: ok (golden, diag, diag+sink produce identical RNG results; hooks verified)'
} finally { Pop-Location }
