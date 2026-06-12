# Recovery + retry with alias-proof stepping (chunk 50, pace 90ms).
$ErrorActionPreference = 'Stop'
$client = [System.Net.Sockets.TcpClient]::new('127.0.0.1', 7843)
$client.ReceiveBufferSize = 1MB
$stream = $client.GetStream()
$writer = [System.IO.StreamWriter]::new($stream); $writer.NewLine = "`n"; $writer.AutoFlush = $true
$reader = [System.IO.StreamReader]::new($stream)
$id = 0
function Call($method, $params) {
    $script:id += 1
    $payload = @{ jsonrpc='2.0'; id=$script:id; method=$method; params=$params } | ConvertTo-Json -Compress -Depth 6
    $writer.WriteLine($payload)
    $stream.ReadTimeout = 12000
    while ($true) {
        $line = $reader.ReadLine(); if (-not $line) { return $null }
        $obj = $line | ConvertFrom-Json
        if ($obj.PSObject.Properties['id'] -and $obj.id -eq $script:id) { return $obj }
    }
}
function Tool($name, $toolArgs = @{}) { return Call 'tools/call' @{ name=$name; arguments=$toolArgs } }
function Snap($name) {
    $r = Tool 'screenshot' @{ scale='native' }
    $img = $r.result.content | Where-Object { $_.type -eq 'image' } | Select-Object -First 1
    [System.IO.File]::WriteAllBytes("C:\Dev\source\WinUAE\out\$name.png", [Convert]::FromBase64String($img.data))
    Write-Host "saved $name.png"
}
function MoveRel($dx, $dy) {
    while ($dx -ne 0 -or $dy -ne 0) {
        $sx = [Math]::Sign($dx) * [Math]::Min(50, [Math]::Abs($dx))
        $sy = [Math]::Sign($dy) * [Math]::Min(50, [Math]::Abs($dy))
        Tool 'mouse_move' @{ x = $sx; y = $sy; space = 'host' } | Out-Null
        Start-Sleep -Milliseconds 90
        $dx -= $sx; $dy -= $sy
    }
    Start-Sleep -Milliseconds 250
}
function Pin() {
    for ($i = 0; $i -lt 32; $i++) {
        Tool 'mouse_move' @{ x = -50; y = -50; space = 'host' } | Out-Null
        Start-Sleep -Milliseconds 90
    }
    Start-Sleep -Milliseconds 250
}
function GotoPx($px, $py) { Pin; MoveRel ($px - 54) ($py - 28) }
function ClickOnce() {
    Tool 'mouse_button' @{ button = 0; state = 1 } | Out-Null
    Start-Sleep -Milliseconds 90
    Tool 'mouse_button' @{ button = 0; state = 0 } | Out-Null
    Start-Sleep -Milliseconds 90
}
function DoubleClick() { ClickOnce; Start-Sleep -Milliseconds 60; ClickOnce }

Call 'initialize' @{ protocolVersion='2024-11-05'; capabilities=@{}; clientInfo=@{name='nav5';version='1'} } | Out-Null

Write-Host "[1] close DefaultIcons editor via Cancel (637,305)"
GotoPx 637 305
Snap 'nav5_0_at_cancel'
ClickOnce
Start-Sleep -Milliseconds 1200
Snap 'nav5_1_after_cancel'

Write-Host "[2] go to Input prefs icon (550,120) and double-click"
GotoPx 550 120
Snap 'nav5_2_at_input'
DoubleClick
Start-Sleep -Milliseconds 3500
Snap 'nav5_3_input_open'

$client.Close()
Write-Host "done"
