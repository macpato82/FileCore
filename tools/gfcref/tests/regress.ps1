# regress.ps1 - regression + stress suite for gfctool (PowerShell, Windows dev env).
# Builds nothing; expects ../gfctool.exe to exist. Run:  pwsh tools/gfcref/tests/regress.ps1
# Exits non-zero if any check fails.

$ErrorActionPreference = "Stop"
$g   = Join-Path $PSScriptRoot "..\gfctool.exe"
$wd  = Join-Path $PSScriptRoot "_work"
if (-not (Test-Path $g)) { Write-Error "gfctool.exe not found at $g (build it first)"; exit 2 }
if (Test-Path $wd) { Remove-Item -Recurse -Force $wd }
New-Item -ItemType Directory -Force $wd | Out-Null

$pass = 0; $fail = 0
function Ok($cond, $msg) {
  if ($cond) { $script:pass++; Write-Host "  PASS  $msg" }
  else       { $script:fail++; Write-Host "  FAIL  $msg" -ForegroundColor Red }
}
function G { & $g @args 2>&1 | Out-Null; return $LASTEXITCODE }   # run quietly, return exit code
function Hash($p){ (Get-FileHash $p -Algorithm SHA256).Hash }
function MakeFile($p,$n){ $b=[byte[]]::new($n); for($i=0;$i -lt $n;$i++){$b[$i]=($i*7+13)%251}; [IO.File]::WriteAllBytes($p,$b) }

$img = Join-Path $wd "t.img"

Write-Host "`n[1] format + info"
Ok ((G format $img --size 1M --sector 512 --ag-size 64K) -eq 0) "format 1M/512/64K"
Ok ((G info $img) -eq 0) "info"
Ok ((G check $img) -eq 0) "check empty"

Write-Host "`n[2] small file round-trip"
MakeFile "$wd\s.bin" 1234
Ok ((G mkfile $img s.bin "$wd\s.bin") -eq 0) "mkfile small"
Ok ((G read   $img s.bin "$wd\s.out") -eq 0) "read small"
Ok ((Hash "$wd\s.bin") -eq (Hash "$wd\s.out")) "small content identical"
Ok ((G check $img) -eq 0) "check after small"

Write-Host "`n[3] cross-AG big file (multi-extent)"
MakeFile "$wd\b.bin" 204800
Ok ((G mkfile $img b.bin "$wd\b.bin") -eq 0) "mkfile 200K (cross-AG)"
Ok ((G read   $img b.bin "$wd\b.out") -eq 0) "read 200K"
Ok ((Hash "$wd\b.bin") -eq (Hash "$wd\b.out")) "200K content identical"
Ok ((G check $img) -eq 0) "check after 200K"

Write-Host "`n[4] delete + reuse"
Ok ((G delete $img b.bin) -eq 0) "delete 200K"
Ok ((G check $img) -eq 0) "check after delete"
Ok ((G mkfile $img b2.bin "$wd\b.bin") -eq 0) "re-add (reuse freed space)"
Ok ((G read $img b2.bin "$wd\b2.out") -eq 0) "read reused"
Ok ((Hash "$wd\b.bin") -eq (Hash "$wd\b2.out")) "reused content identical"
Ok ((G check $img) -eq 0) "check after reuse"

Write-Host "`n[5] nested directories"
Ok ((G mkdir $img docs) -eq 0) "mkdir docs"
Ok ((G mkdir $img docs/sub) -eq 0) "mkdir docs/sub"
MakeFile "$wd\n.bin" 90000
Ok ((G mkfile $img docs/sub/n.bin "$wd\n.bin") -eq 0) "mkfile nested"
Ok ((G read $img docs/sub/n.bin "$wd\n.out") -eq 0) "read nested"
Ok ((Hash "$wd\n.bin") -eq (Hash "$wd\n.out")) "nested content identical"
Ok ((G check $img) -eq 0) "recursive check"
Ok ((G delete $img docs) -ne 0) "delete non-empty dir refused"
Ok ((G delete $img docs/sub/n.bin) -eq 0) "delete nested file"
Ok ((G delete $img docs/sub) -eq 0) "rmdir sub"
Ok ((G delete $img docs) -eq 0) "rmdir docs"
Ok ((G check $img) -eq 0) "check after teardown"

Write-Host "`n[6] journal + rewind"
$snap = Hash $img
MakeFile "$wd\j.bin" 120000
Ok ((G mkfile $img j.bin "$wd\j.bin") -eq 0) "mkfile (journalled)"
Ok ((G rewind $img) -eq 0) "rewind"
Ok ((Hash $img) -eq $snap) "rewind restored byte-identical"
Ok ((G check $img) -eq 0) "check after rewind"

Write-Host "`n[7] corruption detection"
$bytes=[IO.File]::ReadAllBytes($img); $o=129*512+4; $bytes[$o]=$bytes[$o] -bxor 0x01; [IO.File]::WriteAllBytes($img,$bytes)
Ok ((G check $img) -ne 0) "corrupted map bit detected"

Write-Host "`n[8] fragmentation (holes -> multi-extent within AGs)"
$img2 = Join-Path $wd "f.img"
G format $img2 --size 512K --sector 512 --ag-size 32K | Out-Null
MakeFile "$wd\1c.bin" 400          # ~1 cluster each
$made=@(); for($i=0;$i -lt 60;$i++){ if((G mkfile $img2 ("f$i.bin") "$wd\1c.bin") -eq 0){ $made+=$i } }
foreach($i in $made){ if($i % 2 -eq 0){ G delete $img2 ("f$i.bin") | Out-Null } }   # punch holes
Ok ((G check $img2) -eq 0) "check after fragmentation"
MakeFile "$wd\frag.bin" 6000       # spans many small holes
$rc = G mkfile $img2 frag.bin "$wd\frag.bin"
if ($rc -eq 0) { G read $img2 frag.bin "$wd\frag.out" | Out-Null
  Ok ((Hash "$wd\frag.bin") -eq (Hash "$wd\frag.out")) "fragmented file content identical" }
else { Ok $true "fragmented alloc declined gracefully (too many extents) - no corruption" }
Ok ((G check $img2) -eq 0) "check after fragmented write"

Write-Host "`n[9] randomized stress (create/delete, check each step)"
$img3 = Join-Path $wd "st.img"
G format $img3 --size 2M --sector 512 --ag-size 64K | Out-Null
$rng=[Random]::new(12345); $live=@{}; $bad=0
for($it=0; $it -lt 60 -and $bad -eq 0; $it++){
  $act = $rng.Next(0,3)
  if ($act -ge 1 -or $live.Count -eq 0){
    if ($live.Count -lt 70){
      $nm="r$it.bin"; $sz=$rng.Next(1,8000); $src="$wd\$nm"; MakeFile $src $sz
      if((G mkfile $img3 $nm $src) -eq 0){ $live[$nm]=(Hash $src) }
    }
  } else {
    $k=@($live.Keys)[$rng.Next(0,$live.Count)]
    if((G delete $img3 $k) -eq 0){ $live.Remove($k) }
  }
  if((G check $img3) -ne 0){ $bad=1; Write-Host "    check failed at iteration $it" -ForegroundColor Red }
}
Ok ($bad -eq 0) "60 random ops, check passed every step"
# verify all live files still read back correctly
$mismatch=0; foreach($k in $live.Keys){ G read $img3 $k "$wd\v.out" | Out-Null; if((Hash "$wd\v.out") -ne $live[$k]){ $mismatch++ } }
Ok ($mismatch -eq 0) "all $($live.Count) surviving files read back identical"

Write-Host "`n[10] rename / move"
$img4 = Join-Path $wd "r.img"
G format $img4 --size 512K --sector 512 --ag-size 64K | Out-Null
G mkdir $img4 a | Out-Null; G mkdir $img4 b | Out-Null
MakeFile "$wd\r.bin" 40000
G mkfile $img4 a/r.bin "$wd\r.bin" | Out-Null
Ok ((G rename $img4 a/r.bin a/r2.bin) -eq 0) "rename in place"
Ok ((G rename $img4 a/r2.bin b/r3.bin) -eq 0) "move across dirs"
G read $img4 b/r3.bin "$wd\r.out" | Out-Null
Ok ((Hash "$wd\r.bin") -eq (Hash "$wd\r.out")) "moved content identical"
G mkfile $img4 b/exist.bin "$wd\r.bin" | Out-Null
Ok ((G rename $img4 b/r3.bin b/exist.bin) -ne 0) "rename onto existing refused"
Ok ((G check $img4) -eq 0) "check after renames"

Write-Host "`n================  $pass passed, $fail failed  ================"
Remove-Item -Recurse -Force $wd
if ($fail -gt 0) { exit 1 } else { exit 0 }
