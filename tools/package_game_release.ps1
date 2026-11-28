# Assemble the existing Windows game package from its built runtime and the
# pinned UI manifest. No original images, generated game code, or player data.
param(
    [Parameter(Mandatory=$true)][string]$ProjectRoot,
    [string]$Build='build',
    [string]$Sdl='C:/msys64/mingw64',
    [string]$OutDir='',
    [hashtable]$RuntimeNoticeDirs=@{},
    [switch]$StageOnly
)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$ProjectRoot=[IO.Path]::GetFullPath($ProjectRoot)
$framework=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildPath=if([IO.Path]::IsPathRooted($Build)){[IO.Path]::GetFullPath($Build)}else{Join-Path $ProjectRoot $Build}
if(!$OutDir){$OutDir=Join-Path $ProjectRoot 'release'}
$OutDir=[IO.Path]::GetFullPath($OutDir)
$utf8=New-Object Text.UTF8Encoding($false)
function Required([string]$Path){
    if(!(Test-Path -LiteralPath $Path -PathType Leaf)){throw "Missing release input: $Path"}
    return $Path
}
function Copy-Notice([string]$Source,[string]$Name){
    Copy-Item -LiteralPath (Required $Source) -Destination (Join-Path $stage "licenses/$Name")
}
$cmakeLists=Get-Content -LiteralPath (Required (Join-Path $ProjectRoot 'CMakeLists.txt')) -Raw
if($cmakeLists -notmatch '(?m)^\s*project\(\s*([A-Za-z0-9_]+)'){throw 'Missing game project name'}
$name=$Matches[1];$exe="$name.exe"
$version=(Get-Content -LiteralPath (Required (Join-Path $ProjectRoot 'VERSION')) -Raw).Trim()
if($version -notmatch '^\d+\.\d+\.\d+'){throw 'Invalid VERSION'}
[IO.Directory]::CreateDirectory($OutDir) | Out-Null
# A unique owned stage avoids deleting a stale directory or a caller's files.
$stage=Join-Path $OutDir ("stage-$name-"+[Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory((Join-Path $stage 'licenses')) | Out-Null
$cmake=Required (Join-Path $Sdl 'bin/cmake.exe')
& $cmake "-DENGINE=$framework" "-DBUILD=$buildPath" "-DSTAGE=$stage" "-DEXE=$exe" "-DTOOLCHAIN=$Sdl" -P (Join-Path $PSScriptRoot 'stage_release.cmake')
if($LASTEXITCODE -ne 0){throw 'Runtime/asset staging failed'}
Copy-Notice (Join-Path $ProjectRoot 'LICENSE') "$name-LICENSE"
Copy-Notice (Join-Path $ProjectRoot 'PERMISSIONS.md') "$name-PERMISSIONS.md"
Copy-Notice (Join-Path $framework 'LICENSE') 'smsggrecomp-LICENSE'
Copy-Notice (Join-Path $framework 'PERMISSIONS.md') 'smsggrecomp-PERMISSIONS.md'
foreach($core in @('z80-recomp-core','sega-psg-core','recomp-mod-core','recomp-audio-core','recomp-input-core','recomp-presentation-core','recomp-persistence-core','recomp-diagnostics-core','recomp-ui')){
    Copy-Notice (Join-Path $framework "external/$core/LICENSE") "$core-LICENSE"
    $permissions=Join-Path $framework "external/$core/PERMISSIONS.md"
    if(Test-Path -LiteralPath $permissions){Copy-Notice $permissions "$core-PERMISSIONS.md"}
}
Copy-Notice (Join-Path $framework 'external/z80-recomp-core/LICENSES/SUPERZAZU-MIT.txt') 'z80-SUPERZAZU-MIT.txt'
Copy-Notice (Join-Path $framework 'runner/external/superzazu/LICENSE') 'superzazu-MIT.txt'
$ui=Join-Path $framework 'external/recomp-ui'
Copy-Notice (Join-Path $ui 'assets/common/fonts/NOTICE.md') 'launcher-fonts-NOTICE.md'
Copy-Notice (Join-Path $ui 'src/third_party/imgui/LICENSE.txt') 'imgui-LICENSE.txt'
# These vendored sources carry their complete embedded license notices.
foreach($source in @('tinyfiledialogs.c','stb_image.h','stb_image_write.h','stb_truetype.h','gl_core_3_1.h')){
    Copy-Notice (Join-Path $ui "src/third_party/$source") "$source-notices.txt"
}
Get-ChildItem -LiteralPath (Join-Path $framework 'LICENSES') -File | ForEach-Object{Copy-Notice $_.FullName $_.Name}
$runtimePackages=@{
    'SDL2.dll'='SDL2';'libgcc_s_seh-1.dll'='gcc-libs';'libstdc++-6.dll'='gcc-libs';
    'libwinpthread-1.dll'='libwinpthread';'zlib1.dll'='zlib';'libpng16-16.dll'='libpng';
    'libbz2-1.dll'='bzip2';'libbrotlidec.dll'='brotli';'libbrotlicommon.dll'='brotli';
    'libiconv-2.dll'='libiconv';'libintl-8.dll'='gettext-runtime'
}
foreach($dll in Get-Content -LiteralPath (Join-Path $stage 'runtime-dependencies.txt')){
    if(!$dll){continue}
    $directory=if($RuntimeNoticeDirs.ContainsKey($dll)){$RuntimeNoticeDirs[$dll]}elseif($runtimePackages.ContainsKey($dll)){Join-Path $Sdl "share/licenses/$($runtimePackages[$dll])"}else{throw "Supply -RuntimeNoticeDirs for dependency $dll"}
    if(!(Test-Path -LiteralPath $directory -PathType Container)){throw "Missing runtime license directory: $directory"}
    $files=@(Get-ChildItem -LiteralPath $directory -File -Recurse)
    if(!$files.Count){throw "Empty runtime license directory: $directory"}
    foreach($file in $files){
        if($file.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Reparse point in runtime notices'}
        $relative=$file.FullName.Substring(([IO.Path]::GetFullPath($directory)).TrimEnd('\','/').Length+1)
        $destination=Join-Path $stage "licenses/runtime-$dll/$relative"
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
        Copy-Item -LiteralPath $file.FullName -Destination $destination
    }
}
$readme=(Get-Content -LiteralPath (Required (Join-Path $ProjectRoot 'packaging/README.txt.in')) -Raw).Replace('@VERSION@',$version).Replace('@EXE@',$exe)
if($readme -match '@[A-Z_]+@'){throw 'Unexpanded README token'}
[IO.File]::WriteAllText((Join-Path $stage 'README.txt'),$readme,$utf8)
foreach($launcher in @('Play.cmd','Play 16x.cmd')){
    $scale=if($launcher -eq 'Play 16x.cmd'){' --internal-scale 16'}else{''}
    $body="@echo off`r`ncd /d `"%~dp0`"`r`nif `"%~1`"==`"`" (`r`n  `"%~dp0$exe`" --launcher$scale`r`n) else (`r`n  `"%~dp0$exe`" %* --window 3$scale`r`n)`r`n"
    [IO.File]::WriteAllText((Join-Path $stage $launcher),$body,$utf8)
}
$files=@(Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName)
foreach($file in $files){
    $relative=$file.FullName.Substring($stage.Length+1).Replace('\','/')
    if($file.Attributes -band [IO.FileAttributes]::ReparsePoint -or $relative -match '(?i)\.(sms|gg|rom|bin|c|cpp|sav|srm|replay|ini|log)$'){throw "Forbidden release file: $relative"}
}
$manifest=@($files | ForEach-Object{[ordered]@{path=$_.FullName.Substring($stage.Length+1).Replace('\','/');sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}})
[IO.File]::WriteAllText((Join-Path $stage 'release-manifest.json'),($manifest|ConvertTo-Json -Depth 3),$utf8)
if($StageOnly){Write-Host "Staged $stage";return}
$files=@(Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName)
$zip=Join-Path $OutDir "$name-windows-x64.zip";$pending="$zip.pending-$([Guid]::NewGuid().ToString('N'))"
$archive=[IO.Compression.ZipFile]::Open($pending,[IO.Compression.ZipArchiveMode]::Create)
try{foreach($file in $files){
    $entry=$file.FullName.Substring($stage.Length+1).Replace('\','/')
    [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,$file.FullName,$entry,[IO.Compression.CompressionLevel]::Optimal) | Out-Null
}}finally{$archive.Dispose()}
$archive=[IO.Compression.ZipFile]::OpenRead($pending)
try{
    if($archive.Entries.Count -ne $files.Count){throw 'ZIP manifest count mismatch'}
    foreach($entry in $archive.Entries){
        $source=Join-Path $stage $entry.FullName
        if($entry.FullName.Contains('\') -or $entry.FullName -match '(^/|(^|/)\.\.(/|$))' -or !(Test-Path -LiteralPath $source -PathType Leaf)){throw 'Unsafe ZIP entry'}
        $sha=[Security.Cryptography.SHA256]::Create();$stream=$entry.Open()
        try{$actual=[BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','')}finally{$stream.Dispose();$sha.Dispose()}
        if($actual -ne (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash){throw "ZIP hash mismatch: $($entry.FullName)"}
    }
}finally{$archive.Dispose()}
# Preserve a previous archive until its replacement has been reopened/verified.
if(Test-Path -LiteralPath $zip){[IO.File]::Replace($pending,$zip,$null)}else{Move-Item -LiteralPath $pending -Destination $zip}
# Delete only the unique owned stage, after containment/reparse/repository checks.
$resolved=[IO.Path]::GetFullPath($stage)
if(!$resolved.StartsWith($OutDir.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -or
   (Get-ChildItem -LiteralPath $stage -Recurse -Force | Where-Object{$_.Attributes -band [IO.FileAttributes]::ReparsePoint -or $_.Name -eq '.git'})) {throw 'Unsafe stage cleanup'}
Remove-Item -LiteralPath $resolved -Recurse -ErrorAction Stop
Write-Host "Wrote $zip"
Write-Host "sha256 $((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant())"
