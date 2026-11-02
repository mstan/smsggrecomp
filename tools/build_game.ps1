param(
    [Parameter(Mandatory=$true)][string]$ProjectRoot,
    [string]$Rom='',
    [string]$Build='build',
    [string]$Sdl='C:\msys64\mingw64',
    [switch]$SkipRegenerate,
    [ValidateSet('O1','O2','O3')][string]$Optimization='O2',
    [switch]$PortableDisplay,
    [switch]$ReferenceVDP,
    [switch]$ReferenceExecution,
    [ValidateRange(1,8)][int]$Jobs=2
)
$ErrorActionPreference='Stop'
$ProjectRoot=[IO.Path]::GetFullPath($ProjectRoot)
$buildPath=if([IO.Path]::IsPathRooted($Build)) { [IO.Path]::GetFullPath($Build) } else { Join-Path $ProjectRoot $Build }
$tools=Join-Path $Sdl 'bin'
$cmake=Join-Path $tools 'cmake.exe'
$frameworkRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
function Invoke-Native([string]$Executable,[string[]]$Arguments) {
    $previousPreference=$ErrorActionPreference
    try { $ErrorActionPreference='Continue'; & $Executable @Arguments; $nativeExit=$LASTEXITCODE }
    finally { $ErrorActionPreference=$previousPreference }
    if($nativeExit -ne 0) { throw "$Executable failed ($nativeExit)" }
}
foreach($name in @('cmake.exe','ninja.exe','gcc.exe')) {
    if(!(Test-Path -LiteralPath (Join-Path $tools $name))) { throw "Missing native build tool: $tools\$name" }
}
$options=@('-S',$ProjectRoot,'-B',$buildPath,'-G','Ninja','-DCMAKE_BUILD_TYPE=Release',
    "-DCMAKE_C_COMPILER=$tools/gcc.exe","-DCMAKE_MAKE_PROGRAM=$tools/ninja.exe",
    "-DCMAKE_PREFIX_PATH=$Sdl","-DSMSGG_ROOT=$frameworkRoot",('-DSMSGG_REGENERATE='+$(if($SkipRegenerate){'OFF'}else{'ON'})),
    "-DSMSGG_OPTIMIZATION=$Optimization",('-DSMSGG_ENABLE_GPU='+$(if($PortableDisplay){'OFF'}else{'ON'})),
    ('-DSMSGG_REFERENCE_VDP='+$(if($ReferenceVDP){'ON'}else{'OFF'})),
    ('-DSMSGG_NATIVE_EXECUTION='+$(if($ReferenceExecution){'OFF'}else{'ON'})))
if($Rom) { $options+="-DSMSGG_ROM=$((Resolve-Path -LiteralPath $Rom).Path)" }
Invoke-Native $cmake $options
Invoke-Native $cmake @('--build',$buildPath,'--parallel',"$Jobs")
Write-Host "Build ready in $buildPath"
