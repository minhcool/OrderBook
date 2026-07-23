param(
    [int]$Port = 18093,
    [int]$RateLimitPort = 18094
)

$ErrorActionPreference = "Stop"
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$apiBinary = Join-Path $repositoryRoot "build\orderbook_api.exe"
$apiBase = "http://127.0.0.1:$Port"
$rateLimitApiBase = "http://127.0.0.1:$RateLimitPort"

function New-TestToken([string]$Subject) {
    $payloadJson = '{"sub":"' + $Subject + '"}'
    $payload = [Convert]::ToBase64String(
        [Text.Encoding]::UTF8.GetBytes($payloadJson)
    ).TrimEnd("=").Replace("+", "-").Replace("/", "_")

    return "e30.$payload.test"
}

function Get-AuthHeaders([string]$Subject) {
    return @{ Authorization = "Bearer $(New-TestToken $Subject)" }
}

function Get-BotHeaders {
    return @{ Authorization = "Bearer ops-secret" }
}

function Assert-Equal($Actual, $Expected, [string]$Message) {
    if ($Actual -ne $Expected) {
        throw "$Message. Expected '$Expected', got '$Actual'."
    }
}

function Start-TestApi([int]$TargetPort) {
    $process = Start-Process -FilePath $apiBinary -ArgumentList $TargetPort -WindowStyle Hidden -PassThru
    Start-Sleep -Milliseconds 600
    return $process
}

if (-not (Test-Path -LiteralPath $apiBinary)) {
    throw "Build the API first with 'mingw32-make api-build'."
}

$previousAllowUnverified = $env:ORDERBOOK_ALLOW_UNVERIFIED_JWT
$previousBotKeys = $env:ORDERBOOK_BOT_KEYS
$previousAdminToken = $env:ORDERBOOK_ADMIN_TOKEN
$previousRateLimit = $env:ORDERBOOK_RATE_LIMIT_PER_MINUTE
$previousMutationRateLimit = $env:ORDERBOOK_MUTATION_RATE_LIMIT_PER_MINUTE
$previousLiveWait = $env:ORDERBOOK_LIVE_WAIT_MS
$apiProcess = $null
$rateLimitProcess = $null

try {
    $env:ORDERBOOK_ALLOW_UNVERIFIED_JWT = "1"
    $env:ORDERBOOK_BOT_KEYS = "ops-bot:ops-secret"
    $env:ORDERBOOK_ADMIN_TOKEN = "admin-secret"
    $env:ORDERBOOK_RATE_LIMIT_PER_MINUTE = "500"
    $env:ORDERBOOK_MUTATION_RATE_LIMIT_PER_MINUTE = "500"
    $env:ORDERBOOK_LIVE_WAIT_MS = "500"
    $apiProcess = Start-TestApi $Port

    $ready = Invoke-RestMethod -Uri "$apiBase/ready"
    Assert-Equal $ready.ok $true "Ready status"
    Assert-Equal $ready.botKeyCount 1 "Ready bot key count"
    Assert-Equal $ready.adminConfigured $true "Ready admin configured"

    $metrics = Invoke-RestMethod -Uri "$apiBase/metrics"
    if (-not $metrics.Contains("orderbook_requests_total")) {
        throw "Metrics output did not include request counter."
    }

    try {
        Invoke-RestMethod -Uri "$apiBase/admin/summary" | Out-Null
        throw "Admin summary without token unexpectedly succeeded."
    } catch {
        Assert-Equal ([int]$_.Exception.Response.StatusCode) 401 "Admin summary auth status"
    }

    $summary = Invoke-RestMethod -Uri "$apiBase/admin/summary" -Headers @{ "X-Admin-Token" = "admin-secret" }
    Assert-Equal $summary.lobbies 3 "Admin lobby count"

    $botJoin = Invoke-RestMethod `
        -Uri "$apiBase/lobbies/aurora-open-20/join" `
        -Method Post `
        -Headers (Get-BotHeaders) `
        -ContentType "application/json" `
        -Body '{"track":"manual"}'
    Assert-Equal $botJoin.joined $true "Bot key join result"

    $botMembership = Invoke-RestMethod `
        -Uri "$apiBase/lobbies/aurora-open-20/membership" `
        -Headers (Get-BotHeaders)
    Assert-Equal $botMembership.track "bot" "Bot key forces bot track"

    $event = Invoke-RestMethod `
        -Uri "$apiBase/lobbies/aurora-open-20/events?since=0" `
        -Headers (Get-BotHeaders)
    if ($event.sequence -le 0) {
        throw "Live event did not return a sequence."
    }

    $finish = Invoke-RestMethod `
        -Uri "$apiBase/admin/lobbies/aurora-open-20/finish" `
        -Method Post `
        -Headers @{ "X-Admin-Token" = "admin-secret" } `
        -ContentType "application/json" `
        -Body "{}"
    Assert-Equal $finish.finished $true "Admin finish result"
    Assert-Equal $finish.lobby.phase "finished" "Admin finish phase"

    if ($apiProcess -and -not $apiProcess.HasExited) {
        Stop-Process -Id $apiProcess.Id -Force
        $apiProcess = $null
    }

    $env:ORDERBOOK_RATE_LIMIT_PER_MINUTE = "2"
    $env:ORDERBOOK_MUTATION_RATE_LIMIT_PER_MINUTE = "500"
    $rateLimitProcess = Start-TestApi $RateLimitPort
    Invoke-RestMethod -Uri "$rateLimitApiBase/rooms" | Out-Null
    Invoke-RestMethod -Uri "$rateLimitApiBase/rooms" | Out-Null

    try {
        Invoke-RestMethod -Uri "$rateLimitApiBase/rooms" | Out-Null
        throw "Third request unexpectedly bypassed the rate limit."
    } catch {
        Assert-Equal ([int]$_.Exception.Response.StatusCode) 429 "Rate limited status"
    }

    Write-Host "Operational API tests passed."
} finally {
    if ($apiProcess -and -not $apiProcess.HasExited) {
        Stop-Process -Id $apiProcess.Id -Force
    }
    if ($rateLimitProcess -and -not $rateLimitProcess.HasExited) {
        Stop-Process -Id $rateLimitProcess.Id -Force
    }

    $env:ORDERBOOK_ALLOW_UNVERIFIED_JWT = $previousAllowUnverified
    $env:ORDERBOOK_BOT_KEYS = $previousBotKeys
    $env:ORDERBOOK_ADMIN_TOKEN = $previousAdminToken
    $env:ORDERBOOK_RATE_LIMIT_PER_MINUTE = $previousRateLimit
    $env:ORDERBOOK_MUTATION_RATE_LIMIT_PER_MINUTE = $previousMutationRateLimit
    $env:ORDERBOOK_LIVE_WAIT_MS = $previousLiveWait
}
