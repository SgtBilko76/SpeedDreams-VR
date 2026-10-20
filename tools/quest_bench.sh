#!/bin/bash
# Benchmark Speed Dreams VR settings on the Quest without wearing it.
#   tools/quest_bench.sh "label" "key=value ..." [seconds]
# Starts the current quick race directly (vr.cfg startrace), waits, and prints
# the average VrApi GPU time / fps and the game's draw time for the last half.
# The user's vr.cfg is restored afterwards.
set -u
ADB=~/Android/Sdk/platform-tools/adb
LABEL="$1"; SETTINGS="$2"; SECS="${3:-50}"
S=${TMPDIR:-/tmp}/qbench; mkdir -p $S
[ -f $S/vr.cfg.user ] || $ADB pull /sdcard/SpeedDreamsVR/vr.cfg $S/vr.cfg.user >/dev/null
cp $S/vr.cfg.user $S/vr.cfg
for kv in $SETTINGS; do
    k=${kv%%=*}; v=${kv#*=}
    if grep -q "^$k *=" $S/vr.cfg; then sed -i "s|^$k *=.*|$k = $v|" $S/vr.cfg; else echo "$k = $v" >> $S/vr.cfg; fi
done
grep -q "^startrace" $S/vr.cfg || echo "startrace = quickrace" >> $S/vr.cfg
$ADB push $S/vr.cfg /sdcard/SpeedDreamsVR/vr.cfg >/dev/null
$ADB shell am force-stop com.speeddreamsvr
$ADB shell am broadcast -a com.oculus.vrpowermanager.prox_close >/dev/null
$ADB logcat -c
$ADB shell monkey -p com.speeddreamsvr -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
sleep $SECS
P=$($ADB shell pidof com.speeddreamsvr)
$ADB logcat -d > $S/log.txt
$ADB shell am force-stop com.speeddreamsvr
$ADB push $S/vr.cfg.user /sdcard/SpeedDreamsVR/vr.cfg >/dev/null
$ADB shell am broadcast -a com.oculus.vrpowermanager.automation_disable >/dev/null
python3 - "$LABEL" "$P" $S/log.txt <<'EOF'
import re, sys
label, pid, path = sys.argv[1], sys.argv[2], sys.argv[3]
lines = open(path, errors='replace').read().splitlines()
vr = [l for l in lines if ' %s ' % pid in l and 'VrApi' in l and 'FPS=' in l]
perf = [l for l in lines if 'perf: ' in l and 'in stereo' in l and ' 0 in stereo' not in l]
def avg(xs): return sum(xs) / len(xs) if xs else float('nan')
vr = vr[len(vr) // 2:]; perf = perf[len(perf) // 2:]
fps = [int(re.search(r'FPS=(\d+)', l).group(1)) for l in vr]
gpu = [float(re.search(r'App=([\d.]+)ms', l).group(1)) for l in vr]
gp = [float(re.search(r'GPU%=([\d.]+)', l).group(1)) for l in vr]
draw = [float(re.search(r'draw ([\d.]+) ms', l).group(1)) for l in perf]
dc = [int(re.search(r'\| (\d+) draws', l).group(1)) for l in perf]
print(f"{label:28s} fps {avg(fps):5.1f}  gpu {avg(gpu):5.2f} ms ({avg(gp)*100:3.0f}%)  cpu-draw {avg(draw):5.2f} ms  draws {avg(dc):6.0f}  n={len(vr)}/{len(perf)}")
EOF
