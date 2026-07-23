param(
    [int]$Port = 18091
)

$ErrorActionPreference = "Stop"
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$apiBinary = Join-Path $repositoryRoot "build\orderbook_api.exe"
$apiBase = "http://127.0.0.1:$Port"
$issuer = "https://orderbook-auth-test.example"

function ConvertTo-Base64Url([byte[]]$Bytes) {
    return [Convert]::ToBase64String($Bytes).TrimEnd("=").Replace("+", "-").Replace("/", "_")
}

function New-SignedToken(
    [string]$PrivateKeyPath,
    [string]$Subject,
    [int]$ExpiresInSeconds = 300,
    [int]$NotBeforeOffsetSeconds = -5
) {
    $now = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $headerJson = @{ alg = "RS256"; typ = "JWT" } | ConvertTo-Json -Compress
    $payloadJson = @{
        sub = $Subject
        iss = $issuer
        iat = $now
        nbf = $now + $NotBeforeOffsetSeconds
        exp = $now + $ExpiresInSeconds
    } | ConvertTo-Json -Compress

    $header = ConvertTo-Base64Url([Text.Encoding]::UTF8.GetBytes($headerJson))
    $payload = ConvertTo-Base64Url([Text.Encoding]::UTF8.GetBytes($payloadJson))
    $signingInput = "$header.$payload"

    $inputPath = Join-Path $script:tempRoot "signing-input.txt"
    $signaturePath = Join-Path $script:tempRoot "signature.bin"
    Set-Content -LiteralPath $inputPath -Value $signingInput -Encoding ascii -NoNewline
    Invoke-OpenSslQuiet @("dgst", "-sha256", "-sign", $PrivateKeyPath, "-out", $signaturePath, $inputPath) `
        "OpenSSL failed to sign the JWT test token."

    $signature = ConvertTo-Base64Url([IO.File]::ReadAllBytes($signaturePath))
    return "$signingInput.$signature"
}

function New-UnsignedDevToken([string]$Subject) {
    $payloadJson = '{"sub":"' + $Subject + '"}'
    $payload = ConvertTo-Base64Url([Text.Encoding]::UTF8.GetBytes($payloadJson))
    return "e30.$payload.test"
}

function Get-AuthHeaders([string]$Token) {
    return @{ Authorization = "Bearer $Token" }
}

function Assert-Equal($Actual, $Expected, [string]$Message) {
    if ($Actual -ne $Expected) {
        throw "$Message. Expected '$Expected', got '$Actual'."
    }
}

function Invoke-OpenSslQuiet([string[]]$Arguments, [string]$FailureMessage) {
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & openssl @Arguments *>$null
        if ($LASTEXITCODE -ne 0) {
            throw $FailureMessage
        }
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
}

if (-not (Test-Path -LiteralPath $apiBinary)) {
    throw "Build the API first with 'mingw32-make api-build'."
}

if (-not (Get-Command openssl -ErrorAction SilentlyContinue)) {
    throw "OpenSSL is required for this auth test."
}

$script:tempRoot = Join-Path ([IO.Path]::GetTempPath()) "orderbook-auth-test-$([Guid]::NewGuid())"
New-Item -ItemType Directory -Path $script:tempRoot | Out-Null
$privateKey = Join-Path $script:tempRoot "private.pem"
$publicKey = Join-Path $script:tempRoot "public.pem"

$previousJwtKey = $env:CLERK_JWT_KEY
$previousIssuer = $env:CLERK_ISSUER
$previousAllowUnverified = $env:ORDERBOOK_ALLOW_UNVERIFIED_JWT
$apiProcess = $null

try {
    Invoke-OpenSslQuiet @("genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048", "-out", $privateKey) `
        "OpenSSL failed to generate a test private key."
    Invoke-OpenSslQuiet @("rsa", "-in", $privateKey, "-pubout", "-out", $publicKey) `
        "OpenSSL failed to export a test public key."

    $env:CLERK_JWT_KEY = Get-Content -LiteralPath $publicKey -Raw
    $env:CLERK_ISSUER = $issuer
    $env:ORDERBOOK_ALLOW_UNVERIFIED_JWT = $null

    $apiProcess = Start-Process -FilePath $apiBinary -ArgumentList $Port -WindowStyle Hidden -PassThru
    Start-Sleep -Milliseconds 500

    $validToken = New-SignedToken -PrivateKeyPath $privateKey -Subject "verified-user"
    $session = Invoke-RestMethod -Uri "$apiBase/me/session" -Headers (Get-AuthHeaders $validToken)
    if ($session.traderId -le 0) {
        throw "Verified token did not produce a trader id."
    }

    try {
        $tampered = $validToken.Substring(0, $validToken.Length - 1) + "x"
        Invoke-RestMethod -Uri "$apiBase/me/session" -Headers (Get-AuthHeaders $tampered) | Out-Null
        throw "Tampered token unexpectedly succeeded."
    } catch {
        Assert-Equal ([int]$_.Exception.Response.StatusCode) 401 "Tampered token status"
    }

    try {
        $expiredToken = New-SignedToken -PrivateKeyPath $privateKey -Subject "expired-user" -ExpiresInSeconds -120
        Invoke-RestMethod -Uri "$apiBase/me/session" -Headers (Get-AuthHeaders $expiredToken) | Out-Null
        throw "Expired token unexpectedly succeeded."
    } catch {
        Assert-Equal ([int]$_.Exception.Response.StatusCode) 401 "Expired token status"
    }

    try {
        Invoke-RestMethod -Uri "$apiBase/me/session" -Headers (Get-AuthHeaders (New-UnsignedDevToken "unsigned-user")) | Out-Null
        throw "Unsigned dev token unexpectedly succeeded."
    } catch {
        Assert-Equal ([int]$_.Exception.Response.StatusCode) 401 "Unsigned token status"
    }

    Write-Host "Clerk JWT auth tests passed."
} finally {
    if ($apiProcess -and -not $apiProcess.HasExited) {
        Stop-Process -Id $apiProcess.Id -Force
    }

    $env:CLERK_JWT_KEY = $previousJwtKey
    $env:CLERK_ISSUER = $previousIssuer
    $env:ORDERBOOK_ALLOW_UNVERIFIED_JWT = $previousAllowUnverified

    if (Test-Path -LiteralPath $script:tempRoot) {
        $resolvedTempRoot = Resolve-Path -LiteralPath $script:tempRoot
        $systemTempRoot = [IO.Path]::GetTempPath()
        if ($resolvedTempRoot.Path.StartsWith($systemTempRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
            Remove-Item -LiteralPath $resolvedTempRoot.Path -Recurse -Force
        }
    }
}
