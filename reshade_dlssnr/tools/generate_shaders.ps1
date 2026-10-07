param(
  [Parameter(Mandatory=$true)][string]$ProjectDir,
  [string]$GeneratedDir = ""
)

$ProjectDir = (Resolve-Path $ProjectDir).Path
if ([string]::IsNullOrWhiteSpace($GeneratedDir)) {
  $GeneratedDir = Join-Path $ProjectDir "generated"
}
New-Item -ItemType Directory -Force -Path $GeneratedDir | Out-Null
$shaderRoot = Join-Path $ProjectDir "shaders"

function ConvertTo-RawLiteral([string]$Text) {
  $delimiter = "DLSSNR_SHADER"
  while ($Text.Contains(")$delimiter`"")) {
    $delimiter += "_X"
  }
  return 'R"' + $delimiter + '(' + $Text + ')' + $delimiter + '"'
}

$files = Get-ChildItem -Path $shaderRoot -Recurse -File |
  Where-Object { $_.Extension -in @('.hlsl', '.hlsli', '.h') } |
  Sort-Object FullName

$header = @'
#pragma once
#include <cstddef>

namespace reshade_dlssnr {
struct EmbeddedShader { const char* name; const char* source; std::size_t size; };
extern const EmbeddedShader kEmbeddedShaders[];
extern const std::size_t kEmbeddedShaderCount;
const EmbeddedShader* FindEmbeddedShader(const char* name);
}
'@
[IO.File]::WriteAllText((Join-Path $GeneratedDir 'shader_sources.h'), $header, (New-Object Text.UTF8Encoding($false)))

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('#include "shader_sources.h"')
$lines.Add('#include <cstring>')
$lines.Add('namespace reshade_dlssnr {')
$entries = New-Object System.Collections.Generic.List[object]

foreach ($file in $files) {
  $rel = $file.FullName.Substring($shaderRoot.Length + 1).Replace('\','/')
  $safe = [regex]::Replace($rel, '[^A-Za-z0-9_]', '_')
  $symbol = "kEmbeddedShaderSource_$safe"
  $source = [IO.File]::ReadAllText($file.FullName)
  $literal = ConvertTo-RawLiteral $source
  $lines.Add("static constexpr char $symbol`[] = $literal;")
  $entries.Add([pscustomobject]@{ Name = $rel; Symbol = $symbol })
}

$lines.Add('const EmbeddedShader kEmbeddedShaders[] = {')
foreach ($entry in $entries) {
  $lines.Add(('  {{"{0}", {1}, sizeof({1}) - 1}},' -f $entry.Name, $entry.Symbol))
}
$lines.Add('};')
$lines.Add('const std::size_t kEmbeddedShaderCount = sizeof(kEmbeddedShaders) / sizeof(kEmbeddedShaders[0]);')
$lines.Add('const EmbeddedShader* FindEmbeddedShader(const char* name) {')
$lines.Add('  if (name == nullptr) return nullptr;')
$lines.Add('  for (std::size_t i = 0; i < kEmbeddedShaderCount; ++i)')
$lines.Add('    if (std::strcmp(kEmbeddedShaders[i].name, name) == 0) return &kEmbeddedShaders[i];')
$lines.Add('  return nullptr;')
$lines.Add('}')
$lines.Add('}')
[IO.File]::WriteAllLines((Join-Path $GeneratedDir 'shader_sources.cpp'), $lines, (New-Object Text.UTF8Encoding($false)))

$bytecodeHeader = @'
#pragma once
#include <cstddef>

namespace reshade_dlssnr {
struct EmbeddedShaderBytecode { const char* name; const unsigned char* data; std::size_t size; };
extern const EmbeddedShaderBytecode kEmbeddedShaderBytecodes[];
extern const std::size_t kEmbeddedShaderBytecodeCount;
const EmbeddedShaderBytecode* FindEmbeddedShaderBytecode(const char* name);
}
'@
[IO.File]::WriteAllText((Join-Path $GeneratedDir 'shader_bytecode.h'), $bytecodeHeader, (New-Object Text.UTF8Encoding($false)))
$bytecodeStub = @'
#include "shader_bytecode.h"

namespace reshade_dlssnr {
static const unsigned char kNoEmbeddedShaderBytecode = 0;
const EmbeddedShaderBytecode kEmbeddedShaderBytecodes[] = {
  {nullptr, &kNoEmbeddedShaderBytecode, 0},
};
const std::size_t kEmbeddedShaderBytecodeCount = 0;
const EmbeddedShaderBytecode* FindEmbeddedShaderBytecode(const char*) { return nullptr; }
}
'@
[IO.File]::WriteAllText((Join-Path $GeneratedDir 'shader_bytecode.cpp'), $bytecodeStub, (New-Object Text.UTF8Encoding($false)))
