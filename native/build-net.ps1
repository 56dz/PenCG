# 编译 libjsapi_cgnative.so（裸 HTTP body + 加密原语）
# 输出：libs/libjsapi_cgnative.so
$ErrorActionPreference = 'Stop'
$base = Split-Path -Parent $PSScriptRoot
$zig = $env:ZIG_EXE
if (-not $zig) {
  $c = Join-Path (Split-Path -Parent $base) 'env\pyzig\ziglang\zig.exe'
  if (Test-Path $c) { $zig = $c }
}
if (-not $zig -or -not (Test-Path $zig)) { throw 'zig not found: set ZIG_EXE' }
$cache = if ($env:ZIG_CACHE) { $env:ZIG_CACHE } else { Join-Path $env:TEMP 'pencg-zig-cache' }
$env:ZIG_GLOBAL_CACHE_DIR = $cache
$env:ZIG_LOCAL_CACHE_DIR = $cache + '-local'

$stub = "$base\native\stubs-aarch64"
$out = "$base\libs\libjsapi_cgnative.so"
New-Item -ItemType Directory -Force -Path "$base\libs" | Out-Null

$args0 = @(
  'cc', '-target', 'aarch64-linux-gnu.2.17',
  '-O2', '-g0', '-Wl,-s', '-fPIC', '-shared', '-fvisibility=hidden',
  '-I', "$base\native\iot-miniapp-sdk\include",
  '-I', "$base\native\vendor\openssl-include",
  '-o', $out,
  "$base\native\csrc\cgnative\cgnative.c",
  "$stub\libquickjs.so",
  "$stub\libssl.so.1.1",
  "$stub\libcrypto.so.1.1",
  "$stub\libcurl.so.4",
  '-lpthread',
  '-Wl,-soname,libjsapi_cgnative.so'
)
& $zig @args0
if ($LASTEXITCODE -ne 0) { throw "build fail: libjsapi_cgnative.so" }
Write-Host "OK -> $out ($([math]::Round((Get-Item $out).Length/1KB,1)) KB)" -ForegroundColor Green
