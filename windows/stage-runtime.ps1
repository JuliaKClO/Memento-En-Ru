param([Parameter(Mandatory)][string]$BuildDir,[Parameter(Mandatory)][string]$OutputDir,[string]$MsysRoot='C:\msys64',[switch]$TestProfileLauncher)
$ErrorActionPreference='Stop'
if(Test-Path -LiteralPath $OutputDir){throw 'Use a new output directory'}
New-Item -ItemType Directory -Path $OutputDir | Out-Null
$bin="$MsysRoot\ucrt64\bin"
$env:Path="$bin;$MsysRoot\ucrt64\share\qt6\bin;$env:Path"
Copy-Item -LiteralPath "$BuildDir\memento.exe" -Destination $OutputDir
& "$bin\windeployqt.exe" --release --qmldir "$(Split-Path $PSScriptRoot)\src\qml" "$OutputDir\memento.exe"
if($LASTEXITCODE){throw 'windeployqt failed'}
Copy-Item "$BuildDir\src\*.qm" -Destination "$OutputDir\translations"
New-Item -ItemType Directory -Path (Join-Path $OutputDir 'audio') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'audio-readme.txt') -Destination (Join-Path $OutputDir 'audio\README.txt')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'INSTALL-RU.txt') -Destination (Join-Path $OutputDir 'УСТАНОВКА.txt')
if($TestProfileLauncher){
 @'
@echo off
setlocal
set "MEMENTO_PROFILE_NAME=Memento-En-Ru-Test"
start "" "%~dp0memento.exe" %*
'@ | Set-Content -LiteralPath (Join-Path $OutputDir 'Memento-Clean-Profile.cmd') -Encoding Ascii
}
$queue=[Collections.Generic.Queue[string]]::new()
Get-ChildItem $OutputDir -Recurse -File | Where-Object Extension -in '.dll','.exe' | ForEach-Object {$queue.Enqueue($_.FullName)}
$seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$deps=[Collections.Generic.List[object]]::new()
while($queue.Count){
 $file=$queue.Dequeue()
 if(-not $seen.Add($file)){continue}
 $imports=& "$bin\objdump.exe" -p $file | Select-String 'DLL Name: (.+)$'
 if($LASTEXITCODE){throw "Cannot inspect $file"}
 foreach($line in $imports){
  $name=$line.Matches[0].Groups[1].Value.Trim()
  $target=Join-Path $OutputDir $name
  if(Test-Path -LiteralPath "$bin\$name"){
   if(-not(Test-Path -LiteralPath $target)){Copy-Item -LiteralPath "$bin\$name" -Destination $target}
   $deps.Add([pscustomobject]@{Consumer=[IO.Path]::GetFileName($file);Dependency=$name;SHA256=(Get-FileHash -LiteralPath $target).Hash})
   $queue.Enqueue($target)
  }elseif(-not(Test-Path -LiteralPath "$env:WINDIR\System32\$name") -and $name -notmatch '^(api-ms-|ext-ms-)'){
   throw "Unresolved import $name in $file"
  }
 }
}
$deps | Sort-Object Consumer,Dependency -Unique | Export-Csv "$OutputDir\dependency-edges.csv" -NoTypeInformation
Write-Host 'Local runtime staging complete. Redistribution requires dependency license/source review.'
