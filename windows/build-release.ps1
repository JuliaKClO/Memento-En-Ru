param([string]$MsysRoot='C:\msys64', [string]$BuildDir='build-release', [string]$QCoroSource='', [switch]$TestProfile)
$ErrorActionPreference='Stop'
$nativeGit=Get-Command git.exe -ErrorAction Stop
$env:Path="$MsysRoot\ucrt64\bin;$MsysRoot\usr\bin;$env:Path"
$source=Split-Path $PSScriptRoot
$testProfileValue=if($TestProfile){'ON'}else{'OFF'}
$argsList=@('-S',$source,'-B',$BuildDir,'-G','MinGW Makefiles','-DCMAKE_BUILD_TYPE=Release',"-DCMAKE_PREFIX_PATH=$MsysRoot/ucrt64",'-DMEMENTO_MECAB_SUPPORT=OFF','-DMEMENTO_OCR_SUPPORT=OFF',"-DMEMENTO_TEST_PROFILE=$testProfileValue",'-DMEMENTO_BUILD_ENGLISH_TESTS=ON')
if($QCoroSource){$argsList+="-DFETCHCONTENT_SOURCE_DIR_QCORO=$QCoroSource"}
$argsList+="-DGIT_EXECUTABLE=$($nativeGit.Source)"
& cmake @argsList
if($LASTEXITCODE){throw 'CMake configure failed'}
& cmake --build $BuildDir --parallel 4
if($LASTEXITCODE){throw 'Build failed'}
& ctest --test-dir $BuildDir --output-on-failure
if($LASTEXITCODE){throw 'Tests failed'}
