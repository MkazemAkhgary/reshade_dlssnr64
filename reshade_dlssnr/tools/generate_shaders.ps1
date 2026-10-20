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
$script:ShaderRootFull = [IO.Path]::GetFullPath($shaderRoot).TrimEnd([char]'\')
$script:CasRootFull = [IO.Path]::GetFullPath((Join-Path $ProjectDir "third_party\FidelityFX-CAS\ffx-cas")).TrimEnd([char]'\')
$script:Utf8NoBom = New-Object Text.UTF8Encoding($false)

# Outputs that only matter to the C++ build are written through Write-IfChanged.
# Skipping the write leaves the file's mtime alone, which is what stops a
# regeneration from cascading into a full ClCompile when a shader edit
# produced byte-identical output. The expanded_shaders directory below is
# FXC-only input and is still regenerated unconditionally; nothing in the C++
# build includes it.
$script:WrittenCount = 0
$script:SkippedCount = 0

function Write-IfChanged([string]$Path, [string]$Text) {
  if (Test-Path -LiteralPath $Path) {
    try {
      $existing = [IO.File]::ReadAllText($Path, $script:Utf8NoBom)
      if ($existing -ceq $Text) {
        $script:SkippedCount++
        return
      }
    } catch { }
  }
  [IO.File]::WriteAllText($Path, $Text, $script:Utf8NoBom)
  $script:WrittenCount++
}

function ConvertTo-RawLiteral([string]$Text) {
  $Text = [regex]::Replace($Text, "\r\n|\r|\n", "`r`n")
  $delimiter = "DLSSNR_SHADER"
  while ($Text.Contains(")$delimiter`"")) { $delimiter += "_X" }
  return 'R"' + $delimiter + '(' + $Text + ')' + $delimiter + '"'
}

function Get-ShaderName([string]$Path) {
  $full = [IO.Path]::GetFullPath($Path)
  $shaderPrefix = $script:ShaderRootFull + '\'
  if ($full.StartsWith($shaderPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    return $full.Substring($shaderPrefix.Length).Replace('\','/')
  }
  $casPrefix = $script:CasRootFull + '\'
  if ($full.StartsWith($casPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    return ('third_party/FidelityFX-CAS/ffx-cas/' + $full.Substring($casPrefix.Length).Replace('\','/'))
  }
  return [IO.Path]::GetFileName($full)
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
      $allowed = $false
      foreach ($root in @($script:ShaderRootFull, $script:CasRootFull)) {
        $prefix = $root + '\'
        if ($full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
          $allowed = $true
          break
        }
      }
      if ($allowed -and (Test-Path -LiteralPath $full -PathType Leaf)) { return $full }
    } catch { }
  }
  throw ('Cannot resolve #include "{0}" from "{1}". External shader includes are allowed only from third_party/FidelityFX-CAS/ffx-cas.' -f $IncludeName, (Get-ShaderName $IncludingFile))
}

function Add-ShaderFileFragments([string]$Path) {
  $full = [IO.Path]::GetFullPath($Path)
  $source = [IO.File]::ReadAllText($full)
  $lines = [regex]::Split($source, "\r\n|\r|\n")
  $hasPragmaOnce = $false
  foreach ($line in $lines) {
    if ($line -match '^\s*#\s*pragma\s+once\b') { $hasPragmaOnce = $true; break }
  }

  if ($hasPragmaOnce -and $script:VisitedOnce.Contains($full)) { return }
  if ($script:IncludeStack.Contains($full)) {
    if ($hasPragmaOnce) { return }
    $chainPaths = @($script:IncludeStack.ToArray()) + @($full)
    $chain = ($chainPaths | ForEach-Object { Get-ShaderName $_ }) -join ' -> '
    throw ('Include cycle without #pragma once: {0}' -f $chain)
  }
  if ($hasPragmaOnce) { [void]$script:VisitedOnce.Add($full) }

  $script:IncludeStack.Add($full)
  $chunk = New-Object System.Text.StringBuilder
  try {
    for ($i = 0; $i -lt $lines.Length; $i++) {
      # ReadAllText preserves the final newline in the file as a final empty split item.
      if ($i -eq ($lines.Length - 1) -and $lines[$i] -eq '') { continue }
      $line = $lines[$i]
      if ($line -match '^\s*#\s*pragma\s+once\b') { continue }
      $include = [regex]::Match($line, '^\s*#\s*include\s*["<]([^">]+)[">]')
      if ($include.Success) {
        $text = $chunk.ToString()
        if (-not [string]::IsNullOrWhiteSpace($text)) { $script:CurrentPieces.Add($text) }
        [void]$chunk.Clear()
        $dependency = Resolve-ShaderInclude $include.Groups[1].Value $full
        Add-ShaderFileFragments $dependency
        continue
      }
      [void]$chunk.Append($line)
      [void]$chunk.Append("`n")
    }
    $text = $chunk.ToString()
    if (-not [string]::IsNullOrWhiteSpace($text)) { $script:CurrentPieces.Add($text) }
  } finally {
    $script:IncludeStack.RemoveAt($script:IncludeStack.Count - 1)
  }
}

$expandedRootDir = Join-Path $GeneratedDir "expanded_shaders"
if (Test-Path -LiteralPath $expandedRootDir) {
  Remove-Item -LiteralPath $expandedRootDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $expandedRootDir | Out-Null

$files = Get-ChildItem -Path $shaderRoot -Recurse -File |
  Where-Object { $_.Extension -in @('.hlsl', '.hlsli', '.h') } |
  Sort-Object FullName
$roots = @($files | Where-Object { $_.Extension -eq '.hlsl' })
$fragmentTexts = New-Object 'System.Collections.Generic.List[string]'
$fragmentIds = [System.Collections.Generic.Dictionary[string,int]]::new([System.StringComparer]::Ordinal)
$rootEntries = New-Object 'System.Collections.Generic.List[object]'

foreach ($root in $roots) {
  $script:CurrentPieces = New-Object 'System.Collections.Generic.List[string]'
  $script:VisitedOnce = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
  $script:IncludeStack = New-Object 'System.Collections.Generic.List[string]'
  Add-ShaderFileFragments $root.FullName
  $pieces = @($script:CurrentPieces.ToArray())
  if ($pieces.Count -eq 0) { throw ('No compilable source fragments for root shader: {0}' -f (Get-ShaderName $root.FullName)) }

  $indexes = New-Object 'System.Collections.Generic.List[int]'
  foreach ($piece in $pieces) {
    $index = 0
    if ($fragmentIds.TryGetValue($piece, [ref]$index)) {
      $indexes.Add($index)
    } else {
      $index = $fragmentTexts.Count
      $fragmentIds.Add($piece, $index)
      $fragmentTexts.Add($piece)
      $indexes.Add($index)
    }
  }

  $rel = Get-ShaderName $root.FullName
  $safe = [regex]::Replace($rel, '[^A-Za-z0-9_]', '_')
  $rootEntries.Add([pscustomobject]@{ Name=$rel; Safe=$safe; Indices=@($indexes.ToArray()) })

  $preparedPath = Join-Path $expandedRootDir ($rel.Replace('/', '\'))
  New-Item -ItemType Directory -Force -Path (Split-Path $preparedPath -Parent) | Out-Null
  [IO.File]::WriteAllText($preparedPath, ($pieces -join ''), $script:Utf8NoBom)
}

$header = @'
#pragma once
#include <cstddef>

namespace reshade_dlssnr {
struct EmbeddedShaderFragment { const char* source; std::size_t size; };
struct EmbeddedShader {
  const char* name;
  const EmbeddedShaderFragment* const* fragments;
  std::size_t fragment_count;
};
extern const EmbeddedShader kEmbeddedShaders[];
extern const std::size_t kEmbeddedShaderCount;
const EmbeddedShader* FindEmbeddedShader(const char* name);
}
'@
Write-IfChanged (Join-Path $GeneratedDir 'shader_sources.h') $header

$lines = New-Object 'System.Collections.Generic.List[string]'
$lines.Add('#include "shader_sources.h"')
$lines.Add('#include <cstring>')
$lines.Add('namespace reshade_dlssnr {')
for ($i = 0; $i -lt $fragmentTexts.Count; $i++) {
  $textSymbol = 'kEmbeddedShaderText_{0:D3}' -f $i
  $fragmentSymbol = 'kEmbeddedShaderFragment_{0:D3}' -f $i
  $literal = ConvertTo-RawLiteral $fragmentTexts[$i]
  $lines.Add("static constexpr char $textSymbol`[] = $literal;")
  $lines.Add("static constexpr EmbeddedShaderFragment $fragmentSymbol = {$textSymbol, sizeof($textSymbol) - 1};")
}
foreach ($entry in $rootEntries) {
  $partsSymbol = 'kEmbeddedShaderParts_' + $entry.Safe
  $lines.Add("static constexpr const EmbeddedShaderFragment* $partsSymbol`[] = {")
  foreach ($index in $entry.Indices) {
    $lines.Add(('  &kEmbeddedShaderFragment_{0:D3},' -f $index))
  }
  $lines.Add('};')
}
$lines.Add('const EmbeddedShader kEmbeddedShaders[] = {')
foreach ($entry in $rootEntries) {
  $partsSymbol = 'kEmbeddedShaderParts_' + $entry.Safe
  $lines.Add(('  {{"{0}", {1}, sizeof({1}) / sizeof({1}[0])}},' -f $entry.Name, $partsSymbol))
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
# WriteAllLines wrote each line followed by Environment.NewLine (\r\n on
# Windows), including a trailing one after the final line. Reproduce that
# exactly so the byte-for-byte comparison in Write-IfChanged holds.
$sourcesContent = ($lines.ToArray() -join "`r`n") + "`r`n"
Write-IfChanged (Join-Path $GeneratedDir 'shader_sources.cpp') $sourcesContent

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
Write-IfChanged (Join-Path $GeneratedDir 'shader_bytecode.h') $bytecodeHeader

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
Write-IfChanged (Join-Path $GeneratedDir 'shader_bytecode.cpp') $bytecodeStub

Write-Host ('Embedded {0} unique source fragments from {1} shader/header files; assembled {2} roots for verification/precompilation. Wrote {3}, skipped {4} unchanged.' -f $fragmentTexts.Count, $files.Count, $roots.Count, $script:WrittenCount, $script:SkippedCount)
