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
  $Text = [regex]::Replace($Text, "\r\n|\r|\n", "`r`n")
  $delimiter = "DLSSNR_SHADER"
  while ($Text.Contains(")$delimiter`"")) {
    $delimiter += "_X"
  }
  return 'R"' + $delimiter + '(' + $Text + ')' + $delimiter + '"'
}

$script:ExpandedOnce = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$script:ExpansionStack = [System.Collections.Generic.List[string]]::new()
$script:ShaderRootFull = [IO.Path]::GetFullPath($shaderRoot).TrimEnd([char]'\')

function Get-ShaderName([string]$Path) {
  $full = [IO.Path]::GetFullPath($Path)
  return $full.Substring($script:ShaderRootFull.Length + 1).Replace('\','/')
}

function Test-PragmaOnce([string[]]$Lines) {
  foreach ($line in $Lines) {
    if ($line -match '^\s*#\s*pragma\s+once\b') { return $true }
  }
  return $false
}

function Test-IncludeGuard([string[]]$Lines) {
  $meaningful = @($Lines | Where-Object {
    $_.Trim().Length -gt 0 -and $_ -notmatch '^\s*//' -and $_ -notmatch '^\s*#\s*pragma\s+once\b'
  })
  if ($meaningful.Count -lt 2) { return $false }
  $first = [regex]::Match($meaningful[0], '^\s*#\s*ifndef\s+([A-Za-z_]\w*)\s*$')
  $second = [regex]::Match($meaningful[1], '^\s*#\s*define\s+([A-Za-z_]\w*)(?:\s|$)')
  return ($first.Success -and $second.Success -and ($first.Groups[1].Value -ceq $second.Groups[1].Value))
}

function Resolve-ShaderInclude([string]$IncludeName, [string]$IncludingFile) {
  $relative = $IncludeName.Replace('/','\')
  $candidates = @(
    (Join-Path (Split-Path $IncludingFile -Parent) $relative),
    (Join-Path $script:ShaderRootFull $relative)
  )
  foreach ($candidate in $candidates) {
    try {
      $full = [IO.Path]::GetFullPath($candidate)
      $prefix = $script:ShaderRootFull + '\'
      if ($full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $full -PathType Leaf)) {
        return $full
      }
    } catch { }
  }
  throw ('Cannot resolve #include "{0}" from "{1}"' -f $IncludeName, (Get-ShaderName $IncludingFile))
}

function Expand-ShaderSource([string]$Path) {
  $full = [IO.Path]::GetFullPath($Path)
  $source = [IO.File]::ReadAllText($full)
  $lines = [regex]::Split($source, "\r?\n")
  $pragmaOnce = Test-PragmaOnce $lines
  $includeGuard = Test-IncludeGuard $lines

  if ($pragmaOnce -and $script:ExpandedOnce.Contains($full)) { return '' }
  if ($script:ExpansionStack.Contains($full)) {
    if ($pragmaOnce -or $includeGuard) { return '' }
    $chainPaths = @($script:ExpansionStack.ToArray()) + @($full)
    $chain = ($chainPaths | ForEach-Object { Get-ShaderName $_ }) -join ' -> '
    throw ('Include cycle without #pragma once/include guard: {0}' -f $chain)
  }
  if ($pragmaOnce) { [void]$script:ExpandedOnce.Add($full) }

  $script:ExpansionStack.Add($full)
  $name = Get-ShaderName $full
  $output = [System.Collections.Generic.List[string]]::new()
  for ($i = 0; $i -lt $lines.Length; $i++) {
    $line = $lines[$i]
    if ($line -match '^\s*#\s*pragma\s+once\b') { continue }
    $include = [regex]::Match($line, '^\s*#\s*include\s*["<]([^">]+)[">]')
    if (-not $include.Success) {
      $output.Add($line)
      continue
    }

    $dependency = Resolve-ShaderInclude $include.Groups[1].Value $full
    $dependencyName = Get-ShaderName $dependency
    $expanded = Expand-ShaderSource $dependency
    if (-not [string]::IsNullOrEmpty($expanded)) {
      $output.Add(('#line 1 "{0}"' -f $dependencyName))
      $body = $expanded.TrimEnd([char[]]@("`r", "`n")) -split "\n"
      foreach ($bodyLine in $body) { $output.Add($bodyLine) }
    }
    $output.Add(('#line {0} "{1}"' -f ($i + 2), $name))
  }
  $script:ExpansionStack.RemoveAt($script:ExpansionStack.Count - 1)
  return (($output -join "`n") + "`n")
}

function Expand-ShaderRoot([string]$Path) {
  $script:ExpandedOnce.Clear()
  $script:ExpansionStack.Clear()
  return Expand-ShaderSource $Path
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
  $expandedSource = Expand-ShaderRoot $file.FullName
  if ($file.Extension -eq '.hlsl') {
    $source = $expandedSource
  }
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
$rootCount = @($files | Where-Object { $_.Extension -eq '.hlsl' }).Count
Write-Host ('Embedded {0} shader/header files; expanded includes for {1} roots.' -f $files.Count, $rootCount)

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
