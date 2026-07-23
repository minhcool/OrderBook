param(
    [int]$Port = 18089
)

$ErrorActionPreference = "Stop"
$repositoryRoot = Split-Path -Parent $PSScriptRoot
$apiBinary = Join-Path $repositoryRoot "build\orderbook_api.exe"
$apiBase = "http://127.0.0.1:$Port"

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

function Assert-Equal($Actual, $Expected, [string]$Message) {
    if ($Actual -ne $Expected) {
        throw "$Message. Expected '$Expected', got '$Actual'."
    }
}

if (-not (Test-Path -LiteralPath $apiBinary)) {
    throw "Build the API first with 'mingw32-make api-build'."
}

$previousStartDelay = $env:ORDERBOOK_START_DELAY_SECONDS
$previousCooldown = $env:ORDERBOOK_REJOIN_COOLDOWN_SECONDS
$previousAllowUnverified = $env:ORDERBOOK_ALLOW_UNVERIFIED_JWT
$previousRateLimit = $env:ORDERBOOK_RATE_LIMIT_PER_MINUTE
$previousMutationRateLimit = $env:ORDERBOOK_MUTATION_RATE_LIMIT_PER_MINUTE
$env:ORDERBOOK_START_DELAY_SECONDS = "1"
$env:ORDERBOOK_REJOIN_COOLDOWN_SECONDS = "1"
$env:ORDERBOOK_ALLOW_UNVERIFIED_JWT = "1"
$env:ORDERBOOK_RATE_LIMIT_PER_MINUTE = "500"
$env:ORDERBOOK_MUTATION_RATE_LIMIT_PER_MINUTE = "500"
$apiProcess = Start-Process -FilePath $apiBinary -ArgumentList $Port -WindowStyle Hidden -PassThru

try {
    Start-Sleep -Milliseconds 500

    $lobbies = Invoke-RestMethod -Uri "$apiBase/rooms/comp-aurora/lobbies"
    Assert-Equal $lobbies.lobbies.Count 3 "Competitive lobby count"
    Assert-Equal $lobbies.lobbies[0].capacity 10 "First lobby capacity"

    try {
        Invoke-RestMethod `
            -Uri "$apiBase/rooms/solo-alpha/symbols" `
            -Headers (Get-AuthHeaders "solo-player") | Out-Null
        throw "Viewing solo symbols before entering unexpectedly succeeded."
    } catch {
        Assert-Equal ([int]$_.Exception.Response.StatusCode) 403 "Pre-enter solo symbols status"
    }

    $soloJoin = Invoke-RestMethod `
        -Uri "$apiBase/rooms/solo-alpha/join" `
        -Method Post `
        -Headers (Get-AuthHeaders "solo-player") `
        -ContentType "application/json" `
        -Body "{}"
    Assert-Equal $soloJoin.joined $true "Solo join result"

    $soloSymbols = Invoke-RestMethod `
        -Uri "$apiBase/rooms/solo-alpha/symbols" `
        -Headers (Get-AuthHeaders "solo-player")
    Assert-Equal $soloSymbols.symbols[0] "LYRA" "Solo symbols after entering"

    $beforeTickBook = Invoke-RestMethod `
        -Uri "$apiBase/rooms/solo-alpha/book/NOVA" `
        -Headers (Get-AuthHeaders "solo-player")

    $tick = Invoke-RestMethod `
        -Uri "$apiBase/rooms/solo-alpha/simulator/tick" `
        -Method Post `
        -Headers (Get-AuthHeaders "solo-player") `
        -ContentType "application/json" `
        -Body '{"steps":20}'
    Assert-Equal $tick.advanced $true "Simulator tick advanced"
    Assert-Equal $tick.steps 20 "Simulator tick steps"

    $afterTickBook = Invoke-RestMethod `
        -Uri "$apiBase/rooms/solo-alpha/book/NOVA" `
        -Headers (Get-AuthHeaders "solo-player")
    if ($beforeTickBook.asks[0].price -eq $afterTickBook.asks[0].price `
        -and $beforeTickBook.bids[0].price -eq $afterTickBook.bids[0].price) {
        throw "Simulator tick did not move NOVA quotes."
    }

    try {
        Invoke-RestMethod `
            -Uri "$apiBase/lobbies/aurora-open-10/join" `
            -Method Post `
            -Headers (Get-AuthHeaders "solo-player") `
            -ContentType "application/json" `
            -Body "{}" | Out-Null
        throw "Joining a second session unexpectedly succeeded."
    } catch {
        Assert-Equal ([int]$_.Exception.Response.StatusCode) 409 "One active session status"
    }

    $soloLeave = Invoke-RestMethod `
        -Uri "$apiBase/rooms/solo-alpha/leave" `
        -Method Post `
        -Headers (Get-AuthHeaders "solo-player") `
        -ContentType "application/json" `
        -Body "{}"
    Assert-Equal $soloLeave.left $true "Solo leave result"

    try {
        Invoke-RestMethod `
            -Uri "$apiBase/rooms/solo-alpha/join" `
            -Method Post `
            -Headers (Get-AuthHeaders "solo-player") `
            -ContentType "application/json" `
            -Body "{}" | Out-Null
        throw "Re-entering solo during cooldown unexpectedly succeeded."
    } catch {
        Assert-Equal ([int]$_.Exception.Response.StatusCode) 409 "Solo cooldown status"
    }

    try {
        Invoke-RestMethod `
            -Uri "$apiBase/lobbies/aurora-open-10/book/AXON" `
            -Headers (Get-AuthHeaders "outsider") | Out-Null
        throw "Viewing the book before joining unexpectedly succeeded."
    } catch {
        Assert-Equal ([int]$_.Exception.Response.StatusCode) 403 "Pre-join book status"
    }

    try {
        Invoke-RestMethod `
            -Uri "$apiBase/lobbies/aurora-open-10/orders/buy" `
            -Method Post `
            -Headers (Get-AuthHeaders "outsider") `
            -ContentType "application/json" `
            -Body '{"symbol":"AXON","price":80,"quantity":1}' | Out-Null
        throw "Trading before joining unexpectedly succeeded."
    } catch {
        Assert-Equal ([int]$_.Exception.Response.StatusCode) 403 "Pre-join trade status"
    }

    $botJoin = Invoke-RestMethod `
        -Uri "$apiBase/lobbies/aurora-open-20/join" `
        -Method Post `
        -Headers (Get-AuthHeaders "bot-player") `
        -ContentType "application/json" `
        -Body '{"track":"bot"}'
    Assert-Equal $botJoin.joined $true "Bot join result"

    $botMembership = Invoke-RestMethod `
        -Uri "$apiBase/lobbies/aurora-open-20/membership" `
        -Headers (Get-AuthHeaders "bot-player")
    Assert-Equal $botMembership.track "bot" "Bot membership track"

    $botLeave = Invoke-RestMethod `
        -Uri "$apiBase/lobbies/aurora-open-20/leave" `
        -Method Post `
        -Headers (Get-AuthHeaders "bot-player") `
        -ContentType "application/json" `
        -Body "{}"
    Assert-Equal $botLeave.left $true "Bot leave result"

    for ($i = 1; $i -le 10; $i++) {
        $join = Invoke-RestMethod `
            -Uri "$apiBase/lobbies/aurora-open-10/join" `
            -Method Post `
            -Headers (Get-AuthHeaders "player-$i") `
            -ContentType "application/json" `
            -Body "{}"
        Assert-Equal $join.joined $true "Player $i join result"
    }

    try {
        Invoke-RestMethod `
            -Uri "$apiBase/lobbies/aurora-open-10/join" `
            -Method Post `
            -Headers (Get-AuthHeaders "player-11") `
            -ContentType "application/json" `
            -Body "{}" | Out-Null
        throw "Joining a full lobby unexpectedly succeeded."
    } catch {
        Assert-Equal ([int]$_.Exception.Response.StatusCode) 409 "Full lobby status"
    }

    $fullLobby = Invoke-RestMethod -Uri "$apiBase/lobbies/aurora-open-10"
    Assert-Equal $fullLobby.status "full" "Full lobby state"
    Assert-Equal $fullLobby.spotsRemaining 0 "Full lobby remaining capacity"

    for ($i = 1; $i -le 8; $i++) {
        $join = Invoke-RestMethod `
            -Uri "$apiBase/lobbies/aurora-open-15/join" `
            -Method Post `
            -Headers (Get-AuthHeaders "late-lock-player-$i") `
            -ContentType "application/json" `
            -Body "{}"
        Assert-Equal $join.joined $true "Late-lock player $i join result"
    }

    Start-Sleep -Milliseconds 1200

    try {
        Invoke-RestMethod `
            -Uri "$apiBase/lobbies/aurora-open-15/join" `
            -Method Post `
            -Headers (Get-AuthHeaders "late-lock-player-9") `
            -ContentType "application/json" `
            -Body "{}" | Out-Null
        throw "Joining a running lobby unexpectedly succeeded."
    } catch {
        Assert-Equal ([int]$_.Exception.Response.StatusCode) 409 "Running lobby join status"
    }

    $runningLobby = Invoke-RestMethod -Uri "$apiBase/lobbies/aurora-open-15"
    Assert-Equal $runningLobby.status "closed" "Running lobby status"
    Assert-Equal $runningLobby.phase "running" "Running lobby phase"

    try {
        Invoke-RestMethod `
            -Uri "$apiBase/lobbies/aurora-open-10/orders/buy" `
            -Method Post `
            -Headers (Get-AuthHeaders "player-2") `
            -ContentType "application/json" `
            -Body '{"symbol":"AXON","price":1000000,"quantity":1000000}' | Out-Null
        throw "Overspending cash unexpectedly succeeded."
    } catch {
        Assert-Equal ([int]$_.Exception.Response.StatusCode) 400 "Cash risk status"
    }

    try {
        Invoke-RestMethod `
            -Uri "$apiBase/lobbies/aurora-open-10/orders/sell" `
            -Method Post `
            -Headers (Get-AuthHeaders "player-2") `
            -ContentType "application/json" `
            -Body '{"symbol":"AXON","price":80,"quantity":1}' | Out-Null
        throw "Selling without a position unexpectedly succeeded."
    } catch {
        Assert-Equal ([int]$_.Exception.Response.StatusCode) 400 "Position risk status"
    }

    $order = Invoke-RestMethod `
        -Uri "$apiBase/lobbies/aurora-open-10/orders/buy" `
        -Method Post `
        -Headers (Get-AuthHeaders "player-1") `
        -ContentType "application/json" `
        -Body '{"symbol":"AXON","price":80,"quantity":1}'
    Assert-Equal $order.restingQuantity 1 "Resting order before leave"

    $leave = Invoke-RestMethod `
        -Uri "$apiBase/lobbies/aurora-open-10/leave" `
        -Method Post `
        -Headers (Get-AuthHeaders "player-1") `
        -ContentType "application/json" `
        -Body "{}"
    Assert-Equal $leave.left $true "Leave result"

    try {
        Invoke-RestMethod `
            -Uri "$apiBase/lobbies/aurora-open-10/join" `
            -Method Post `
            -Headers (Get-AuthHeaders "player-1") `
            -ContentType "application/json" `
            -Body "{}" | Out-Null
        throw "Rejoining during cooldown unexpectedly succeeded."
    } catch {
        Assert-Equal ([int]$_.Exception.Response.StatusCode) 409 "Cooldown rejoin status"
    }

    Start-Sleep -Milliseconds 1200

    $rejoin = Invoke-RestMethod `
        -Uri "$apiBase/lobbies/aurora-open-10/join" `
        -Method Post `
        -Headers (Get-AuthHeaders "player-1") `
        -ContentType "application/json" `
        -Body "{}"
    Assert-Equal $rejoin.joined $true "Rejoin after cooldown"

    $orders = Invoke-RestMethod `
        -Uri "$apiBase/lobbies/aurora-open-10/me/orders" `
        -Headers (Get-AuthHeaders "player-1")
    Assert-Equal $orders.orders.Count 0 "Open orders after leave and rejoin"

    Write-Host "Competitive lobby API tests passed."
} finally {
    if ($apiProcess -and -not $apiProcess.HasExited) {
        Stop-Process -Id $apiProcess.Id -Force
    }

    $env:ORDERBOOK_START_DELAY_SECONDS = $previousStartDelay
    $env:ORDERBOOK_REJOIN_COOLDOWN_SECONDS = $previousCooldown
    $env:ORDERBOOK_ALLOW_UNVERIFIED_JWT = $previousAllowUnverified
    $env:ORDERBOOK_RATE_LIMIT_PER_MINUTE = $previousRateLimit
    $env:ORDERBOOK_MUTATION_RATE_LIMIT_PER_MINUTE = $previousMutationRateLimit
}
