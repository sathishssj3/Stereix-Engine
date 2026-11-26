# ==============================================================================
# sign_binaries.ps1 - Automated Authenticode Code-Signing for Stereix Engine
# Creates/uses a local trusted development certificate to sign injector binaries
# CRIT-03 Guard: NEVER re-signs vrinject.dll after vr-inject-cli.exe has been
# linked against expected_hash.h (signing mutates SHA-256 and breaks Exit 14).
# ==============================================================================

[CmdletBinding()]
param (
    [string]$TargetDir = "$PSScriptRoot\..\build\bin",
    [string]$CertSubject = "CN=Stereix Local Development",
    [string]$SingleFile = "",
    [switch]$SkipDllHashMutation
)

$ErrorActionPreference = "Stop"

Write-Host "=== Stereix Engine Authenticode Signing Tool ===" -ForegroundColor Cyan

# 1. Check for existing certificate (prefer Stereix cert, fall back to legacy NexVR dev cert)
$cert = Get-ChildItem -Path Cert:\CurrentUser\My -CodeSigningCert -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.Subject -in @($CertSubject, "CN=NexVR Local Development") } |
    Select-Object -First 1

if (-not $cert) {
    Write-Host "[*] Creating self-signed code-signing certificate: $CertSubject" -ForegroundColor Yellow
    $cert = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $CertSubject `
        -CertStoreLocation "Cert:\CurrentUser\My" `
        -KeyUsage DigitalSignature `
        -FriendlyName "Stereix Engine Development Certificate" `
        -NotAfter (Get-Date).AddYears(5)

    Write-Host "[OK] Self-signed code signing certificate created in CurrentUser\My." -ForegroundColor Green
} else {
    Write-Host "[OK] Found existing valid code-signing certificate ($($cert.Subject), Thumbprint: $($cert.Thumbprint))" -ForegroundColor Green
}

# Single-file pre-hash signing mode (invoked during CMake POST_BUILD before generate_hash.ps1)
if (-not [string]::IsNullOrWhiteSpace($SingleFile)) {
    if (Test-Path -LiteralPath $SingleFile) {
        $status = Set-AuthenticodeSignature -Certificate $cert -FilePath $SingleFile -HashAlgorithm SHA256
        if ($status.Status -notin @("Valid", "UnknownError")) {
            throw "Signing failed for $SingleFile : $($status.Status) - $($status.StatusMessage)"
        }
        Write-Host "[OK] Signed $SingleFile (SHA-256)" -ForegroundColor Green
    }
    exit 0
}

# 2. Post-build signing mode: ONLY sign vr-inject-cli.exe if currently unsigned.
# NEVER re-sign vrinject.dll post-link because that mutates its SHA-256 away from EXPECTED_DLL_HASH (CRIT-03).
$docsDir = if (Test-Path "$PSScriptRoot\..\stereix-docs") { "$PSScriptRoot\..\stereix-docs" } else { "$PSScriptRoot\..\nexvr-docs" }
$candidateFiles = @(
    "$TargetDir\vr-inject-cli.exe",
    "$PSScriptRoot\..\updates\vr-inject-cli.exe",
    "$docsDir\landing-page\public\updates\vr-inject-cli.exe"
)

$signedCount = 0
foreach ($filePath in $candidateFiles) {
    if (Test-Path -LiteralPath $filePath) {
        $existing = Get-AuthenticodeSignature -LiteralPath $filePath
        if ($existing.Status -ne "NotSigned") {
            Write-Host "[=] Already signed (preserving bytes): $filePath" -ForegroundColor DarkGray
            continue
        }
        Write-Host "[*] Signing unsigned CLI: $filePath" -ForegroundColor Gray
        try {
            $status = Set-AuthenticodeSignature -Certificate $cert -FilePath $filePath -HashAlgorithm SHA256
            if ($status.Status -in @("Valid", "UnknownError")) {
                Write-Host "    [OK] Authenticode signature applied successfully (SHA-256)" -ForegroundColor Green
                $signedCount++
            } else {
                Write-Host "    [WARN] Signature status: $($status.StatusMessage)" -ForegroundColor Yellow
            }
        } catch {
            Write-Host "    [WARN] Could not sign file: $_" -ForegroundColor Yellow
        }
    }
}

Write-Host "=== Signing complete. $signedCount binaries signed (vrinject.dll hashes preserved). ===" -ForegroundColor Cyan
