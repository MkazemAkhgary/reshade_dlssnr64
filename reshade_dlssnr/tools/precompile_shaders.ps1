param(
  [Parameter(Mandatory=$true)][string]$ProjectDir,
  [Parameter(Mandatory=$true)][string]$GeneratedDir,
  [string]$FxcPath = ""
)

$project = (Resolve-Path $ProjectDir).Path
$generated = (Resolve-Path $GeneratedDir).Path
$shaderRoot = Join-Path $project "shaders"
$csodir = Join-Path $generated "shader_cache"
New-Item -ItemType Directory -Force -Path $csodir | Out-Null

$roots = Get-ChildItem -Path $shaderRoot -Filter *.hlsl -Recurse -File | Sort-Object FullName
$entries = @()
foreach ($shader in $roots) {
  $rel = $shader.FullName.Substring($shaderRoot.Length + 1).Replace('\','/')
  $safe = [regex]::Replace($rel, '[^A-Za-z0-9_]', '_')
  $cso = Join-Path $csodir ($safe + '.cso')
  & $FxcPath /nologo /T cs_5_1 /E CSMain /O3 /I $shaderRoot /Fo $cso $shader.FullName
  if ($LASTEXITCODE -ne 0) { throw "Precompile failed: $rel" }
  $bytes = [IO.File]::ReadAllBytes($cso)
  $hex = ($bytes | ForEach-Object { '0x{0:X2}' -f $_ }) -join ','
  $entries += [pscustomobject]@{ Name=$rel; Bytes=$hex; Count=$bytes.Length }
}

$out = @('#include "shader_bytecode.h"','#include <cstring>','','namespace reshade_dlssnr {')
foreach ($e in $entries) {
  $safe = [regex]::Replace($e.Name, '[^A-Za-z0-9_]', '_')
  $sym = 'kEmbeddedShaderBytecode_' + $safe
  $out += "static const unsigned char $sym`[] = {$($e.Bytes)};"
}
$out += 'const EmbeddedShaderBytecode kEmbeddedShaderBytecodes[] = {'
foreach ($e in $entries) {
  $safe = [regex]::Replace($e.Name, '[^A-Za-z0-9_]', '_')
  $sym = 'kEmbeddedShaderBytecode_' + $safe
  $out += "  {\"$($e.Name)\", $sym, sizeof($sym)},"
}
$out += '};'
$out += 'const std::size_t kEmbeddedShaderBytecodeCount = sizeof(kEmbeddedShaderBytecodes) / sizeof(kEmbeddedShaderBytecodes[0]);'
$out += 'const EmbeddedShaderBytecode* FindEmbeddedShaderBytecode(const char* name) {'
$out += '  if (name == nullptr) return nullptr;'
$out += '  for (std::size_t i = 0; i < kEmbeddedShaderBytecodeCount; ++i)'
$out += '    if (std::strcmp(kEmbeddedShaderBytecodes[i].name, name) == 0) return &kEmbeddedShaderBytecodes[i];'
$out += '  return nullptr;'
$out += '}'
$out += '}'
Set-Content -Path (Join-Path $generated "shader_bytecode.cpp") -Value $out -Encoding UTF8
