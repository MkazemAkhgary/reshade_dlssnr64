param(
  [Parameter(Mandatory=$true)][string]$ProjectDir,
  [string]$FxcPath = "",
  [string]$GeneratedDir = ""
)

$ProjectDir = (Resolve-Path $ProjectDir).Path
if ([string]::IsNullOrWhiteSpace($GeneratedDir)) {
  $GeneratedDir = Join-Path $ProjectDir "generated"
}
$GeneratedDir = [IO.Path]::GetFullPath($GeneratedDir)
$expandedShaderRoot = Join-Path $GeneratedDir "expanded_shaders"
$expandedShaderPrefix = [IO.Path]::GetFullPath($expandedShaderRoot).TrimEnd('\') + '\'
$sourceShaderRoot = Join-Path $ProjectDir "shaders"
$sourceShaderPrefix = [IO.Path]::GetFullPath($sourceShaderRoot).TrimEnd('\') + '\'
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

# Under MSBuild the generate target runs first via DependsOnTargets, so this
# fallback only fires when the script is invoked standalone. It regenerates the
# expanded sources so verification does not have to be preceded by a manual
# generate call.
if (-not (Test-Path -LiteralPath $expandedShaderRoot -PathType Container)) {
  & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $ProjectDir "tools\generate_shaders.ps1") -ProjectDir $ProjectDir -GeneratedDir $GeneratedDir
  if ($LASTEXITCODE -ne 0) { throw "Failed to generate expanded shader sources for verification." }
}

if ([string]::IsNullOrWhiteSpace($FxcPath)) {
  $sdkRoot = $env:WindowsSdkDir
  if ([string]::IsNullOrWhiteSpace($sdkRoot)) {
    $sdkRoot = $env:WindowsSdkVerBinPath
    if (-not [string]::IsNullOrWhiteSpace($sdkRoot)) {
      $sdkRoot = Split-Path $sdkRoot -Parent | Split-Path -Parent
    }
  }
  if (-not [string]::IsNullOrWhiteSpace($sdkRoot)) {
    $candidates = Get-ChildItem -Path (Join-Path $sdkRoot "bin") -Filter fxc.exe -Recurse -File -ErrorAction SilentlyContinue |
      Where-Object { $_.FullName -match '\\x64\\fxc\.exe$' } |
      Sort-Object FullName -Descending
    if ($candidates.Count -gt 0) { $FxcPath = $candidates[0].FullName }
  }
}

if ([string]::IsNullOrWhiteSpace($FxcPath) -or -not (Test-Path $FxcPath)) {
  throw "fxc.exe was not found. Set FxcPath=... or install a Windows SDK containing fxc.exe."
}

$roots = Get-ChildItem -Path $expandedShaderRoot -Filter *.hlsl -Recurse -File | Sort-Object FullName

# Verification is a pass/fail check, not a producer. Its only writes are the
# .cso files inside a temp directory that is deleted before this script
# returns, so there is no persistent C++ output to protect with the
# write-if-changed pattern. What makes the MSBuild target incremental is the
# verify.stamp it touches as its Outputs, not anything this script writes.
$verifyTemp = Join-Path ([System.IO.Path]::GetTempPath()) ("reshade_dlssnr_fxc_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $verifyTemp | Out-Null
$failedShaders = @()
try {
  $index = 0
  foreach ($shader in $roots) {
    $index++
    $objectPath = Join-Path $verifyTemp ("shader_{0:D4}.cso" -f $index)
    Write-Host "============================================================"
    $relativeShaderPath = $shader.FullName.Substring($expandedShaderPrefix.Length)
    $sourceShaderPath = Join-Path $sourceShaderRoot $relativeShaderPath
    Write-Host ("VERIFYING [{0}/{1}] {2}" -f $index, $roots.Count, $sourceShaderPath)

    # These sources have already had #pragma once processed during expansion;
    # FXC therefore sees neither the unsupported pragma nor duplicate includes.
    $output = @(& $FxcPath /nologo /T cs_5_1 /E CSMain /O3 /I (Join-Path $ProjectDir "shaders") /Fo $objectPath $shader.FullName 2>&1)
    $exitCode = $LASTEXITCODE

    foreach ($line in $output) {
      if ($line -is [System.Management.Automation.ErrorRecord]) {
        if ($null -ne $line.Exception) {
          $message = $line.Exception.Message
        } else {
          $message = [string]$line
        }
      } else {
        $message = [string]$line
      }
      Write-Host (Convert-ExpandedShaderDiagnostics $message)
    }

    if ($exitCode -ne 0) {
      $failedShaders += [pscustomobject]@{
        Path = $sourceShaderPath
        ExitCode = $exitCode
      }
      Write-Host ("FAILED   [{0}/{1}] {2} (FXC exit code: {3})" -f $index, $roots.Count, $sourceShaderPath, $exitCode)
    } else {
      Write-Host ("VERIFIED [{0}/{1}] {2}" -f $index, $roots.Count, $sourceShaderPath)
    }
  }
} finally {
  Remove-Item -Recurse -Force -LiteralPath $verifyTemp -ErrorAction SilentlyContinue
}

Write-Host "============================================================"
if ($failedShaders.Count -eq 0) {
  Write-Host ("All {0} shaders verified successfully." -f $roots.Count)
  exit 0
}

Write-Host ("SHADER VERIFICATION FAILED: {0} of {1} shaders failed." -f $failedShaders.Count, $roots.Count)
foreach ($failure in $failedShaders) {
  Write-Host ("  FXC exit code {0}: {1}" -f $failure.ExitCode, $failure.Path)
}
exit 1
