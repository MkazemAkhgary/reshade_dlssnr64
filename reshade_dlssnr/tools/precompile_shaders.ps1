param(
  [Parameter(Mandatory=$true)][string]$ProjectDir,
  [Parameter(Mandatory=$true)][string]$GeneratedDir,
  [string]$FxcPath = ""
)

$project = (Resolve-Path $ProjectDir).Path
$generated = (Resolve-Path $GeneratedDir).Path
$shaderRoot = Join-Path $project "shaders"
$expandedShaderRoot = Join-Path $generated "expanded_shaders"
$expandedShaderPrefix = [IO.Path]::GetFullPath($expandedShaderRoot).TrimEnd('\') + '\'
$sourceShaderPrefix = [IO.Path]::GetFullPath($shaderRoot).TrimEnd('\') + '\'
$projectPrefix = [IO.Path]::GetFullPath($ProjectDir).TrimEnd('\') + '\'
$expandedRelativePrefix = ''
if ($expandedShaderRoot.StartsWith($projectPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
  $expandedRelativePrefix = $expandedShaderRoot.Substring($projectPrefix.Length).TrimEnd('\') + '\'
}
$sourceRelativePrefix = 'shaders\'

function Convert-ExpandedShaderDiagnostics([string]$Text) {
  $converted = $Text.Replace($expandedShaderPrefix, $sourceShaderPrefix)
  $converted = $converted.Replace($expandedShaderPrefix.Replace('\', '/'), $sourceShaderPrefix.Replace('\', '/'))
  if (-not [string]::IsNullOrEmpty($expandedRelativePrefix)) {
    $converted = $converted.Replace($expandedRelativePrefix, $sourceRelativePrefix)
    $converted = $converted.Replace($expandedRelativePrefix.Replace('\', '/'), $sourceRelativePrefix.Replace('\', '/'))
  }
  return $converted
}
if (-not (Test-Path -LiteralPath $expandedShaderRoot -PathType Container)) {
  throw "Expanded shader sources not found. Run generate_shaders.ps1 before precompiling."
}
$csodir = Join-Path $generated "shader_cache"
New-Item -ItemType Directory -Force -Path $csodir | Out-Null

$roots = Get-ChildItem -Path $expandedShaderRoot -Filter *.hlsl -Recurse -File | Sort-Object FullName
$entries = @()
foreach ($shader in $roots) {
  $rel = $shader.FullName.Substring($expandedShaderRoot.Length + 1).Replace('\','/')
  $safe = [regex]::Replace($rel, '[^A-Za-z0-9_]', '_')
  $name = [System.IO.Path]::GetFileNameWithoutExtension($rel)
  $cso = Join-Path $csodir ($name + '.cso')
  $fxcOutput = @(& $FxcPath /nologo /T cs_5_1 /E CSMain /O3 /I $shaderRoot /Fo $cso $shader.FullName 2>&1)
  $fxcExitCode = $LASTEXITCODE
  foreach ($line in $fxcOutput) {
    if ($line -is [System.Management.Automation.ErrorRecord] -and $null -ne $line.Exception) {
      $message = $line.Exception.Message
    } else {
      $message = [string]$line
    }
    Write-Host (Convert-ExpandedShaderDiagnostics $message)
  }
  if ($fxcExitCode -ne 0) {
    $sourcePath = Join-Path $shaderRoot $rel.Replace('/', '\')
    throw "Precompile failed: $sourcePath"
  }
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
  $out += ('  {{"{0}", {1}, sizeof({1})}},' -f $e.Name, $sym)
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

# shader_bytecode.cpp is a C++ input, so restamping it cascades into a full
# ClCompile of that TU and of every TU that includes shader_bytecode.h. The
# previous Set-Content -Encoding UTF8 wrote a BOM and rewrote the file on every
# precompile run regardless of whether the bytecode had changed; the
# write-if-changed pattern below, matching generate_shaders.ps1, leaves the
# mtime alone when the content is identical. The .cso files in shader_cache are
# not treated the same way: they never feed the C++ compile, and the FXC call
# that produces them is what this script exists to run.
$utf8 = New-Object Text.UTF8Encoding($false)
$bytecodePath = Join-Path $generated "shader_bytecode.cpp"
$bytecodeText = ($out -join "`r`n") + "`r`n"

$changed = $true
if (Test-Path -LiteralPath $bytecodePath) {
  try {
    $existing = [IO.File]::ReadAllText($bytecodePath, $utf8)
    if ($existing -ceq $bytecodeText) { $changed = $false }
  } catch {
    $changed = $true
  }
}
if ($changed) {
  [IO.File]::WriteAllText($bytecodePath, $bytecodeText, $utf8)
  Write-Host ("Embedded {0} shader bytecode tables; shader_bytecode.cpp updated." -f $entries.Count)
} else {
  Write-Host ("Embedded {0} shader bytecode tables; shader_bytecode.cpp unchanged." -f $entries.Count)
}
