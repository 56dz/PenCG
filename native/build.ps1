# 编译全部 native 模块
param([switch]$NetOnly, [switch]$MediaOnly)
$ErrorActionPreference = 'Stop'
if (-not $MediaOnly) { & "$PSScriptRoot\build-net.ps1" }
if (-not $NetOnly)   { & "$PSScriptRoot\build-media.ps1" }
Get-ChildItem "$PSScriptRoot\..\libs" -Filter '*.so' | Format-Table Name, Length
