# 编译 libjsapi_cgmedia.so（WebRTC: ICE + DTLS-SRTP + RTP 拆包 + GStreamer 出画）
# 输出：libs/libjsapi_cgmedia.so
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
$gst = "$base\native\stubs-aarch64-gst"
$out = "$base\libs\libjsapi_cgmedia.so"
New-Item -ItemType Directory -Force -Path "$base\libs" | Out-Null

# 所有 csrc/cgmedia/*.c 一起编
$srcs = Get-ChildItem -Path "$base\native\csrc\cgmedia" -Filter '*.c' | ForEach-Object { $_.FullName }
if (-not $srcs -or $srcs.Count -eq 0) { throw 'native/csrc/cgmedia/*.c 为空' }

$args0 = @(
  'cc', '-target', 'aarch64-linux-gnu.2.17',
  '-O2', '-g0', '-Wl,-s', '-fPIC', '-shared', '-fvisibility=hidden',
  '-DPEN_CG_MEDIA=1',
  '-I', "$base\native\iot-miniapp-sdk\include",
  '-I', "$base\native\vendor\openssl-include",
  '-I', "$base\native\csrc\cgmedia",
  '-o', $out
) + $srcs + @(
  "$stub\libquickjs.so",
  "$stub\libssl.so.1.1",
  "$stub\libcrypto.so.1.1",
  "$gst\libgstreamer-1.0.so.0",
  "$gst\libglib-2.0.so.0",
  "$gst\libgobject-2.0.so.0",
  "$gst\libgmodule-2.0.so.0",
  "$gst\libgstapp-1.0.so.0",
  '-lpthread', '-lm',
  '-Wl,-soname,libjsapi_cgmedia.so'
)
& $zig @args0
if ($LASTEXITCODE -ne 0) { throw "build fail: libjsapi_cgmedia.so" }
Write-Host "OK -> $out ($([math]::Round((Get-Item $out).Length/1KB,1)) KB)" -ForegroundColor Green
