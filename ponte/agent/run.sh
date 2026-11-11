#!/system/bin/sh
# Supervisor on the Android phone ("Ponte"), started detached (setsid) by the Ponte app through local adb, or by the PC.
# Keeps these up as the shell user, restarting each a few seconds after it exits:
#  - tailscaled in userspace mode (no VPN on the phone; tailnet TCP to this node reaches 127.0.0.1:<port>)
#  - the guard (Guard.java): owns client port 8791 and the control port
#  - Remoto (Remoto.java, port 8795): the Android phone's apps on the Lumia; needs the phone unlocked once
#  - our push server (WnsServer.java, 127.0.0.1:8799 for the Lumia's WNS client through Tailscale Funnel, API on
#    127.0.0.1:8800 for whoever sends the toasts): needs no unlock, so always up
# run.sh [start|keep]  supervisor up
# run.sh off           everything down
# Logs: guard.log, remoto.log, wns.log, ts/ts.log.
D=/data/local/tmp/ponte
cd $D || exit 1
killall_() {
    for p in $(pgrep -f '^(/system/bin/)?sh /data/local/tmp/ponte/run[.]sh'); do [ "$p" != $$ ] && kill $p 2>/dev/null; done
    pkill -f 'dumbphone[.]ponte[.]Guard'
    pkill -f 'dumbphone[.]ponte[.]Remoto'
    pkill -f 'dumbphone[.]ponte[.]WnsServer'
    pkill -f 'ponte/ts/tailscale[d]'
}
case "$1" in
    off) killall_; rm -f $D/run.pid; exit 0 ;;
esac
tsloop() {
    while true; do
        if [ -x $D/ts/tailscaled ]; then
            HOME=$D/ts $D/ts/tailscaled --no-logs-no-support --tun=userspace-networking --statedir=$D/ts/state \
                --socket=@ponte-ts --port=41641 >$D/ts/ts.log 2>&1 </dev/null
        fi
        sleep 5
    done
}
guardloop() {
    while true; do
        CLASSPATH=$D/agent.dex app_process / dumbphone.ponte.Guard >>$D/guard.log 2>&1 </dev/null
        echo "$(date '+%F %T') run.sh: guardia uscita ($?)" >>$D/guard.log
        sleep 2
    done
}
remotoloop() {
    while true; do
        if [ "$(getprop sys.user.0.ce_available)" = true ]; then
            CLASSPATH=$D/agent.dex app_process / dumbphone.ponte.Remoto >>$D/remoto.log 2>&1 </dev/null
            echo "$(date '+%F %T') run.sh: remoto uscito ($?)" >>$D/remoto.log
        fi
        sleep 2
    done
}
wnsloop() {
    while true; do
        CLASSPATH=$D/agent.dex app_process / dumbphone.ponte.WnsServer >>$D/wns.log 2>&1 </dev/null
        echo "$(date '+%F %T') run.sh: server push uscito ($?)" >>$D/wns.log
        sleep 2
    done
}
# one supervisor only; strays from an older start (by hand) are replaced
P=$(cat $D/run.pid 2>/dev/null)
[ -n "$P" ] && grep -q run.sh /proc/$P/cmdline 2>/dev/null && exit 0
killall_
echo $$ >$D/run.pid
sleep 1
tsloop &
guardloop &
remotoloop &
wnsloop &
wait
