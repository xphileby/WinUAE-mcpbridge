# Live demo: orchestrate multiple mcpbridge tools through a single persistent
# connection, exactly the way Claude Desktop or a similar MCP client would.

$ErrorActionPreference = 'Stop'

$client = [System.Net.Sockets.TcpClient]::new('127.0.0.1', 7843)
$client.ReceiveBufferSize = 1MB
$client.SendBufferSize = 1MB
$stream = $client.GetStream()
$writer = [System.IO.StreamWriter]::new($stream)
$writer.NewLine = "`n"
$writer.AutoFlush = $true
$reader = [System.IO.StreamReader]::new($stream)

$id = 0
$collected = New-Object System.Collections.Generic.List[string]

function Call($method, $params) {
    $script:id += 1
    $payload = @{
        jsonrpc = '2.0'
        id      = $script:id
        method  = $method
        params  = $params
    } | ConvertTo-Json -Compress -Depth 6
    Write-Host "→ id=$($script:id) $method $($params | ConvertTo-Json -Compress)" -ForegroundColor Cyan
    $writer.WriteLine($payload)
    # Drain frames until we see one with our id (responses) or 2s elapses (notifications)
    $stream.ReadTimeout = 8000
    while ($true) {
        $line = $reader.ReadLine()
        if (-not $line) { return $null }
        $obj = $line | ConvertFrom-Json
        if ($obj.PSObject.Properties['id'] -and $obj.id -eq $script:id) {
            return $obj
        } elseif ($obj.PSObject.Properties['method'] -and -not $obj.PSObject.Properties['id']) {
            Write-Host "  ⟨notification⟩ $($obj.method) $($obj.params | ConvertTo-Json -Compress)" -ForegroundColor DarkYellow
        } else {
            # response to a different id (shouldn't happen in serial use)
            Write-Host "  ⟨stray⟩ $line" -ForegroundColor DarkGray
        }
    }
}

function ToolCall($name, $toolArgs = @{}) {
    $resp = Call 'tools/call' @{ name = $name; arguments = $toolArgs }
    if (-not $resp) { Write-Host "  (no reply)" -ForegroundColor Red; return $null }
    if ($resp.error) {
        Write-Host "  ✗ error: $($resp.error.message)" -ForegroundColor Red
        return $resp
    }
    $textItems = $resp.result.content | Where-Object { $_.type -eq 'text' }
    foreach ($t in $textItems) { Write-Host "  ← $($t.text)" -ForegroundColor Green }
    return $resp
}

function SaveScreenshot($filename, $toolArgs = @{ scale = 'native' }) {
    $resp = Call 'tools/call' @{ name = 'screenshot'; arguments = $toolArgs }
    $img = $resp.result.content | Where-Object { $_.type -eq 'image' } | Select-Object -First 1
    $meta = $resp.result.content | Where-Object { $_.type -eq 'text' } | Select-Object -First 1
    Write-Host "  ← screenshot $($meta.text)" -ForegroundColor Green
    if ($img) {
        [System.IO.File]::WriteAllBytes("C:\Dev\source\WinUAE\out\$filename", [Convert]::FromBase64String($img.data))
        Write-Host "  ← saved $filename" -ForegroundColor Green
    }
}

Write-Host "`n=== MCP-orchestrated WinUAE demo ===`n" -ForegroundColor Magenta

# 0) MCP handshake
Call 'initialize' @{ protocolVersion = '2024-11-05'; capabilities = @{}; clientInfo = @{ name = 'demo.ps1'; version = '1.0' } } | Out-Null

# 1) Wait until the guest is idle (boot done)
Write-Host "`n[1] Wait for Workbench to settle" -ForegroundColor Magenta
ToolCall 'wait_for_idle' @{ timeout_ms = 8000; quiet_ms = 200 } | Out-Null

# 2) Snapshot the world
Write-Host "`n[2] Initial probe" -ForegroundColor Magenta
ToolCall 'get_screen_geometry'   | Out-Null
ToolCall 'mousehack_status'      | Out-Null
ToolCall 'get_cpu_state'         | Out-Null
ToolCall 'disk_list'             | Out-Null
ToolCall 'is_paused'             | Out-Null
SaveScreenshot 'demo_1_workbench.png'

# 3) Memory inspection: read longword at 0x4 (ExecBase) and confirm it matches A6
Write-Host "`n[3] Read ExecBase pointer at 0x4" -ForegroundColor Magenta
ToolCall 'memory_read' @{ addr = '0x4'; length = 4 } | Out-Null
ToolCall 'find_in_memory' @{
    pattern_b64 = [Convert]::ToBase64String([System.Text.Encoding]::ASCII.GetBytes('exec.library'))
    start = '0xF80000'; end = '0x1000000'; max_hits = 3
} | Out-Null

# 4) Open Workbench's "Execute Command" dialog and type into it
Write-Host "`n[4] Open Execute-Command dialog + type" -ForegroundColor Magenta
ToolCall 'key_press' @{ key = 'e'; modifiers = @('RAmiga') } | Out-Null
ToolCall 'wait_for_region_change' @{ x = 460; y = 530; w = 100; h = 30; timeout_ms = 2000 } | Out-Null
SaveScreenshot 'demo_2_dialog_open.png'
ToolCall 'type_text' @{ text = 'echo HELLO_FROM_MCP_BRIDGE' } | Out-Null
Start-Sleep -Milliseconds 400
SaveScreenshot 'demo_3_dialog_typed.png'

# 5) Cancel the dialog
ToolCall 'key_press' @{ key = 'Escape' } | Out-Null
Start-Sleep -Milliseconds 200

# 6) Pause/resume to demonstrate notifications coming back
Write-Host "`n[6] Pause + resume (watch notifications)" -ForegroundColor Magenta
ToolCall 'pause'  | Out-Null
ToolCall 'is_paused' | Out-Null
ToolCall 'resume' | Out-Null

# 7) Save state, then we'll restore at the end
Write-Host "`n[7] Save state checkpoint" -ForegroundColor Magenta
ToolCall 'save_state' @{ path = 'C:\Dev\source\WinUAE\out\demo_checkpoint.uss' } | Out-Null

# 8) Disassemble a few instructions at the current PC
Write-Host "`n[8] Disassemble at PC" -ForegroundColor Magenta
$cpu = (ToolCall 'get_cpu_state').result.content[0].text | ConvertFrom-Json
$pcHex = '0x{0:x}' -f $cpu.pc
Write-Host "  (PC = $pcHex)" -ForegroundColor DarkCyan
ToolCall 'disassemble' @{ addr = $pcHex; count = 4 } | Out-Null

# 9) Debug command: run 'r' to dump registers via the built-in debugger
Write-Host "`n[9] debug('r')" -ForegroundColor Magenta
$d = ToolCall 'debug' @{ command = 'r' }
$dt = $d.result.content[0].text
Write-Host ("  (truncated, {0} chars total)" -f $dt.Length) -ForegroundColor DarkGray

# 10) Done
Write-Host "`n=== Demo complete ===" -ForegroundColor Magenta
$client.Close()
