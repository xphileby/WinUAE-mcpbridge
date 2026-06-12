# Verify closed-loop mouse accuracy across display modes.
$client = [System.Net.Sockets.TcpClient]::new('127.0.0.1', 7843)
$client.ReceiveBufferSize = 1MB
$stream = $client.GetStream()
$writer = [System.IO.StreamWriter]::new($stream); $writer.NewLine = "`n"; $writer.AutoFlush = $true
$reader = [System.IO.StreamReader]::new($stream)
$id = 0
function Call($m, $p) { $script:id++; $writer.WriteLine((@{jsonrpc='2.0';id=$script:id;method=$m;params=$p}|ConvertTo-Json -Compress -Depth 6)); $stream.ReadTimeout=40000; while($true){$l=$reader.ReadLine(); if(-not $l){return $null}; $o=$l|ConvertFrom-Json; if($o.PSObject.Properties['id'] -and $o.id -eq $script:id){return $o}} }
function Tool($n,$a=@{}){ $r=Call 'tools/call' @{name=$n;arguments=$a}; $t=$r.result.content|Where-Object{$_.type -eq 'text'}|Select-Object -First 1; Write-Host "  $n -> $($t.text)"; return $r }
function PtrPos() { $r = Tool 'get_pointer_pos'; ($r.result.content[0].text | ConvertFrom-Json) }
function Snap($name) { $r=Tool 'screenshot' @{scale='native'}; $img=$r.result.content|Where-Object{$_.type -eq 'image'}|Select-Object -First 1; [System.IO.File]::WriteAllBytes("C:\Dev\source\WinUAE\out\$name.png",[Convert]::FromBase64String($img.data)); $meta=$r.result.content|Where-Object{$_.type -eq 'text'}|Select-Object -First 1; Write-Host "    geo: $($meta.text)" }
function TestMode($label) {
    Write-Host "=== mode: $label ===" -ForegroundColor Magenta
    # Accuracy sweep
    $maxErr = 0
    foreach ($pt in @(@(37,133),@(300,80),@(550,180),@(100,40))) {
        Tool 'mouse_move' @{x=$pt[0];y=$pt[1]} | Out-Null
        $p = PtrPos
        $e = [Math]::Abs($p.x - $pt[0]) + [Math]::Abs($p.y - $pt[1])
        if ($e -gt $maxErr) { $maxErr = $e }
    }
    Write-Host "  max positional error: $maxErr px" -ForegroundColor $(if($maxErr -le 1){'Green'}else{'Yellow'})
    # Functional: open Workbench window by double-clicking its icon
    Tool 'mouse_move' @{x=37;y=133} | Out-Null
    Tool 'mouse_click' @{button=0;count=2} | Out-Null
    Start-Sleep -Milliseconds 1500
    Snap "mode_${label}_opened"
    # Close it: click the window close gadget (top-left of the window, ~Intuition 28,24)
    Tool 'mouse_move' @{x=28;y=24} | Out-Null
    Tool 'mouse_click' @{button=0;count=1} | Out-Null
    Start-Sleep -Milliseconds 1000
    Snap "mode_${label}_closed"
}

Call 'initialize' @{protocolVersion='2024-11-05';capabilities=@{};clientInfo=@{name='mode';version='1'}} | Out-Null

TestMode 'windowed'

Write-Host "switching to fullwindow (maximized)..." -ForegroundColor Cyan
Tool 'set_config' @{line='gfx_fullscreen_amiga=fullwindow'} | Out-Null
Start-Sleep -Milliseconds 2500
TestMode 'fullwindow'

Write-Host "restoring windowed..." -ForegroundColor Cyan
Tool 'set_config' @{line='gfx_fullscreen_amiga=false'} | Out-Null
Start-Sleep -Milliseconds 2000

$client.Close()
Write-Host "done"
