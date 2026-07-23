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

$apiProcess = Start-Process -FilePath $apiBinary -ArgumentList $Port -WindowStyle Hidden -PassThru

try {
    Start-Sleep -Milliseconds 500

    $lobbies = Invoke-RestMethod -Uri "$apiBase/rooms/comp-aurora/lobbies"
    Assert-Equal $lobbies.lobbies.Count 3 "Competitive lobby count"
    Assert-Equal $lobbies.lobbies[0].capacity 10 "First lobby capacity"

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

    Invoke-RestMethod `
        -Uri "$apiBase/lobbies/aurora-open-10/join" `
        -Method Post `
        -Headers (Get-AuthHeaders "player-1") `
        -ContentType "application/json" `
        -Body "{}" | Out-Null

    $orders = Invoke-RestMethod `
        -Uri "$apiBase/lobbies/aurora-open-10/me/orders" `
        -Headers (Get-AuthHeaders "player-1")
    Assert-Equal $orders.orders.Count 0 "Open orders after leave and rejoin"

    Write-Host "Competitive lobby API tests passed."
} finally {
    if ($apiProcess -and -not $apiProcess.HasExited) {
        Stop-Process -Id $apiProcess.Id -Force
    }
}
