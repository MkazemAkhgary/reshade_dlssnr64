param(
  [Parameter(Mandatory=$true)][string]$ProjectDir,
  [string]$FxcPath = ""
)

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

$roots = Get-ChildItem -Path (Join-Path $ProjectDir "shaders") -Filter *.hlsl -Recurse -File | Sort-Object FullName
$verifyTemp = Join-Path ([System.IO.Path]::GetTempPath()) ("reshade_dlssnr_fxc_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $verifyTemp | Out-Null
$failedShaders = @()
try {
  $index = 0
  foreach ($shader in $roots) {
    $index++
    $objectPath = Join-Path $verifyTemp ("shader_{0:D4}.cso" -f $index)
    Write-Host "============================================================"
    Write-Host ("VERIFYING [{0}/{1}] {2}" -f $index, $roots.Count, $shader.FullName)
    Write-Host ("FXC OBJECT: {0}" -f $objectPath)

    $output = @(& $FxcPath /nologo /T cs_5_1 /E CSMain /O3 /I (Join-Path $ProjectDir "shaders") /Fo $objectPath $shader.FullName 2>&1)
    $exitCode = $LASTEXITCODE

    foreach ($line in $output) {
      if ($line -is [System.Management.Automation.ErrorRecord]) {
        if ($null -ne $line.Exception) {
          Write-Host $line.Exception.Message
        } else {
          Write-Host ([string]$line)
        }
      } else {
        Write-Host ([string]$line)
      }
    }

    if ($exitCode -ne 0) {
      $failedShaders += [pscustomobject]@{
        Path = $shader.FullName
        ExitCode = $exitCode
      }
      Write-Host ("FAILED   [{0}/{1}] {2} (FXC exit code: {3})" -f $index, $roots.Count, $shader.FullName, $exitCode)
    } else {
      Write-Host ("VERIFIED [{0}/{1}] {2}" -f $index, $roots.Count, $shader.FullName)
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
