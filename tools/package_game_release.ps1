# Release ZIP packager for smsggrecomp game repos. Ships the compiled exe,
# SDL2.dll, the Play launchers, README.txt and every license notice; never a
# ROM or generated C. Fails if any required notice is missing.
#
#   powershell -File smsggrecomp/tools/package_game_release.ps1 -ProjectRoot <game repo>
#
# Game repo inputs: CMakeLists.txt project() name (exe + zip name), VERSION,
# LICENSE, PERMISSIONS.md, packaging/README.txt.in (@VERSION@ / @EXE@ tokens).
param(
    [Parameter(Mandatory=$true)][string]$ProjectRoot,
    [string]$Build='build',
    [string]$Sdl='C:\msys64\mingw64',
    [string]$OutDir='',
    [switch]$StageOnly
)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$ProjectRoot=[IO.Path]::GetFullPath($ProjectRoot)
$framework=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildPath=if([IO.Path]::IsPathRooted($Build)) { [IO.Path]::GetFullPath($Build) } else { Join-Path $ProjectRoot $Build }
if(!$OutDir) { $OutDir=Join-Path $ProjectRoot 'release' }
$OutDir=[IO.Path]::GetFullPath($OutDir)

function Get-Required([string]$Path) {
    if(!(Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing required release input: $Path" }
    return $Path
}

$cmakeLists=Get-Content -LiteralPath (Get-Required (Join-Path $ProjectRoot 'CMakeLists.txt')) -Raw
if($cmakeLists -notmatch '(?m)^\s*project\(\s*([A-Za-z0-9_]+)') { throw "No project() name in $ProjectRoot\CMakeLists.txt" }
$name=$Matches[1]
$exe="$name.exe"
$version=(Get-Content -LiteralPath (Get-Required (Join-Path $ProjectRoot 'VERSION')) -Raw).Trim()
if($version -notmatch '^\d+\.\d+\.\d+') { throw "VERSION is not a version: '$version'" }

# Every notice the package must carry: destination name under licenses/ -> source.
$coreRoot=Join-Path $framework 'external\z80-recomp-core'
$notices=[ordered]@{
    "$name-LICENSE"                   = Join-Path $ProjectRoot 'LICENSE'
    "$name-PERMISSIONS.md"            = Join-Path $ProjectRoot 'PERMISSIONS.md'
    'smsggrecomp-LICENSE'             = Join-Path $framework 'LICENSE'
    'smsggrecomp-PERMISSIONS.md'      = Join-Path $framework 'PERMISSIONS.md'
    'z80-recomp-core-LICENSE'         = Join-Path $coreRoot 'LICENSE'
    'z80-recomp-core-PERMISSIONS.md'  = Join-Path $coreRoot 'PERMISSIONS.md'
    'z80-recomp-core-SUPERZAZU-MIT.txt' = Join-Path $coreRoot 'LICENSES\SUPERZAZU-MIT.txt'
    'superzazu-MIT.txt'               = Join-Path $framework 'runner\external\superzazu\LICENSE'
    'SDL2-LICENSE.txt'                = Join-Path $Sdl 'share\licenses\SDL2\LICENSE.txt'
}

$stage=Join-Path $OutDir "stage-$name"
if(Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'licenses') | Out-Null

Copy-Item -LiteralPath (Get-Required (Join-Path $buildPath $exe)) -Destination $stage
Copy-Item -LiteralPath (Get-Required (Join-Path $buildPath 'SDL2.dll')) -Destination $stage
foreach($n in $notices.Keys) {
    Copy-Item -LiteralPath (Get-Required $notices[$n]) -Destination (Join-Path (Join-Path $stage 'licenses') $n)
}

$readme=(Get-Content -LiteralPath (Get-Required (Join-Path $ProjectRoot 'packaging\README.txt.in')) -Raw).Replace('@VERSION@',$version).Replace('@EXE@',$exe)
if($readme -match '@[A-Z_]+@') { throw "Unexpanded token $($Matches[0]) in packaging\README.txt.in" }
$utf8=New-Object Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $stage 'README.txt'),$readme,$utf8)

$launchers=[ordered]@{ 'Play.cmd'=''; 'Play 16x.cmd'=' --internal-scale 16' }
foreach($launcher in $launchers.Keys) {
    $body="@echo off`r`ncd /d `"%~dp0`"`r`nif `"%~1`"==`"`" (`r`n  echo Drag your own ROM onto this file to play.`r`n  pause`r`n  exit /b 2`r`n)`r`n`"%~dp0$exe`" `"%~1`" --window 3$($launchers[$launcher])`r`n"
    [IO.File]::WriteAllText((Join-Path $stage $launcher),$body,$utf8)
}

if($StageOnly) { Write-Host "Staged $stage"; return }

# Forward-slash entry names so the ZIP extracts correctly on every platform.
$zip=Join-Path $OutDir "$name-windows-x64.zip"
if(Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
$archive=[IO.Compression.ZipFile]::Open($zip,[IO.Compression.ZipArchiveMode]::Create)
try {
    Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
        $entry=$_.FullName.Substring($stage.Length+1).Replace('\','/')
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,$_.FullName,$entry,[IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $archive.Dispose() }
Remove-Item -LiteralPath $stage -Recurse -Force
$hash=(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "Wrote $zip"
Write-Host "sha256 $hash"
