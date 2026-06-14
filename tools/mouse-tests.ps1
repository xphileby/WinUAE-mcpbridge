# Reusable mouse pixel-perfect harness for the mcpbridge closed-loop mouse.
# Usage:
#   .\tools\mouse-tests.ps1            # sweep current mode (auto-detects bounds)
#   .\tools\mouse-tests.ps1 -WindowModes   # cycle small/full/fullscreen window modes
# Each test: mouse_move {x,y} then get_pointer_pos; asserts |got-target| <= 1px.
param([switch]$WindowModes)
$client = [System.Net.Sockets.TcpClient]::new('127.0.0.1', 7843)
$client.ReceiveBufferSize = 2MB
$stream = $client.GetStream()
$writer = [System.IO.StreamWriter]::new($stream); $writer.NewLine="`n"; $writer.AutoFlush=$true
$reader = [System.IO.StreamReader]::new($stream); $id=0
function Call($m,$p){ $script:id++; $writer.WriteLine((@{jsonrpc='2.0';id=$script:id;method=$m;params=$p}|ConvertTo-Json -Compress -Depth 6)); $stream.ReadTimeout=60000; while($true){$l=$reader.ReadLine(); if(-not $l){return $null}; $o=$l|ConvertFrom-Json; if($o.PSObject.Properties['id'] -and $o.id -eq $script:id){return $o}} }
function Tool($n,$a=@{}){ Call 'tools/call' @{name=$n;arguments=$a} }
function Txt($r){ ($r.result.content|Where-Object{$_.type -eq 'text'}|Select-Object -First 1).text }
function PtrPos(){ ($(Txt (Tool 'get_pointer_pos')) | ConvertFrom-Json) }
function Bounds(){ Tool 'mouse_move' @{x=4000;y=4000} | Out-Null; PtrPos }
function Sweep($label){
  $b = Bounds; $mx=$b.x; $my=$b.y
  $targets = @(@(0,0),@([int]($mx*0.25),[int]($my*0.25)),@([int]($mx*0.5),[int]($my*0.5)),@([int]($mx*0.75),[int]($my*0.75)),@($mx,$my),@([int]($mx*0.9),5),@(5,[int]($my*0.9)))
  $maxErr=0; $res=@()
  foreach($t in $targets){ Tool 'mouse_move' @{x=$t[0];y=$t[1]} | Out-Null; $p=PtrPos; $e=[Math]::Abs($p.x-$t[0])+[Math]::Abs($p.y-$t[1]); if($e -gt $maxErr){$maxErr=$e}; $res+="($($t[0]),$($t[1]))->($($p.x),$($p.y))" }
  $col=if($maxErr -le 1){'Green'}else{'Red'}
  Write-Host ("  [{0}] bounds={1}x{2} max_err={3}px  {4}" -f $label,($mx+1),($my+1),$maxErr,($res -join ' ')) -ForegroundColor $col
}
Call 'initialize' @{protocolVersion='2024-11-05';capabilities=@{};clientInfo=@{name='mt';version='1'}} | Out-Null
Tool 'keyboard_release_all' | Out-Null
if ($WindowModes) {
  foreach($m in @(@('false','small-window'),@('fullwindow','full-window'),@('true','fullscreen'))){
    Tool 'set_config' @{line="gfx_fullscreen_amiga=$($m[0])"} | Out-Null; Start-Sleep -Milliseconds 3000
    Sweep $m[1]
  }
  Tool 'set_config' @{line='gfx_fullscreen_amiga=false'} | Out-Null; Start-Sleep -Milliseconds 2500
} else {
  Sweep 'current'
}
$client.Close()
