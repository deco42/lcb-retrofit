#!/bin/bash
set -euo pipefail

DEFAULT_UNO=lcb@192.168.123.74
DIR=/tmp/lcbd-deploy
BIN=/usr/local/bin
LOWER=/media/root-ro # eMMC below the overlayroot
FILES="lcbd lcb_test"
CHECK_S=30 # longer than the 6 s stall detection of lcbd plus WatchdogSec=15 of lcbd.service
# rt-tests (cyclictest) and tcpdump for the latency test and debugging, python3-serial for com1_tool
PACKAGES="linux-image-rt-amd64 build-essential cmake python3-serial overlayroot rt-tests tcpdump"

usage() {
    echo "usage: ./deploy.sh [USER@HOST]            build the committed lcbd on the UNO, test, install, restart"
    echo "       ./deploy.sh rollback [USER@HOST]   previous lcbd and lcb_test back"
    echo "       ./deploy.sh setup [USER@HOST]      fresh Debian 13: packages, uno/ to /, configs/ to /etc/lcbd"
    echo "       ./deploy.sh status [USER@HOST]     what lcbd needs on the UNO, ok or NO"
    echo "default USER@HOST: $DEFAULT_UNO"
    echo "If the new lcbd does not run for $CHECK_S s after the restart, the previous binaries come back by themselves."
    exit 1
}

overlay_on() { [ "$(findmnt -n -o FSTYPE /)" = overlay ] && [ -d "$LOWER$BIN" ]; }

each_root() {
    "$@" ""
    if overlay_on; then
        mount -o remount,rw "$LOWER"
        local ok=0
        "$@" "$LOWER" || ok=$?
        sync
        mount -o remount,ro "$LOWER"
        return $ok
    fi
}

install_new() {
    for f in $FILES; do
        if [ -f "$1$BIN/$f" ]; then
            cp -p "$1$BIN/$f" "$1$BIN/$f.prev"
        fi

        install -m 755 "$DIR/build/$f" "$1$BIN/$f"
    done
}

restore_prev() {
    for f in $FILES; do
        install -m 755 "$1$BIN/$f.prev" "$1$BIN/$f" # a new file, the running one cannot be overwritten
    done
}

sha_of() { sha256sum < "$1" | cut -d' ' -f1; }

restart_and_check() {
    local expected restarts pid
    expected=$(sha_of "$1")
    systemctl reset-failed lcbd 2> /dev/null || true # after many restarts systemd refuses to start it again
    systemctl restart lcbd
    restarts=$(systemctl show -p NRestarts --value lcbd)
    echo "waiting $CHECK_S s for lcbd"
    sleep $CHECK_S
    pid=$(systemctl show -p MainPID --value lcbd)
    journalctl -u lcbd -n 6 --no-pager -o cat
    systemctl is-active --quiet lcbd && [ "$(systemctl show -p NRestarts --value lcbd)" = "$restarts" ] &&
        [ "$pid" != 0 ] && [ "$(sha_of "/proc/$pid/exe")" = "$expected" ]
}

remote_build() {
    if [ ! -f /etc/systemd/system/lcbd.service ]; then
        echo "lcbd.service missing on the UNO: ./deploy.sh setup first"
        exit 1
    fi

    cd "$DIR"
    rm -rf src build
    mkdir src
    tar -xzf src.tar.gz -C src
    if ! cmake -S src/lcbd -B build -DSOEM_URL="$DIR/soem.tar.gz" > build.log 2>&1 ||
        ! nice -n 19 cmake --build build -j"$(nproc)" >> build.log 2>&1; then
        tail -20 build.log
        echo "BUILD FAILED, nothing installed ($DIR/build.log)"
        exit 1
    fi

    grep -i "warning" build.log | grep -v "_deps/soem" || echo "build: no warnings"
    local skip=()
    if ! python3 -c "import serial" 2> /dev/null; then
        skip=(-E com1_tool)
        echo "pyserial missing on the UNO: test com1_tool skipped"
    fi

    if ! (cd build && nice -n 19 ctest "${skip[@]}" --output-on-failure > ../ctest.log 2>&1); then
        tail -30 ctest.log
        echo "TESTS FAILED, nothing installed ($DIR/ctest.log)"
        exit 1
    fi

    grep "tests passed" ctest.log
}

remote_install() {
    each_root install_new
    if overlay_on; then
        echo "installed on the live system and on the eMMC ($LOWER), previous binaries kept as *.prev"
    else
        echo "installed (no overlayroot), previous binaries kept as *.prev"
    fi

    if restart_and_check "$BIN/lcbd"; then
        echo "DEPLOYED: lcbd runs the new binary"
        return
    fi

    if [ ! -f "$BIN/lcbd.prev" ]; then
        systemctl stop lcbd
        echo "THE NEW LCBD DOES NOT RUN: no previous binaries (first install), lcbd stopped"
        exit 1
    fi

    echo "THE NEW LCBD DOES NOT RUN: previous binaries back"
    each_root restore_prev
    restart_and_check "$BIN/lcbd" && echo "previous lcbd runs again" || echo "previous lcbd does not run either"
    exit 1
}

remote_rollback() {
    for f in $FILES; do
        [ -f "$BIN/$f.prev" ] || { echo "$BIN/$f.prev missing, nothing changed"; exit 1; }
    done

    each_root restore_prev
    restart_and_check "$BIN/lcbd" && echo "ROLLED BACK: previous lcbd runs" || { echo "previous lcbd does not run"; exit 1; }
}

remote_setup() {
    if overlay_on; then
        echo "write protection (overlayroot) is on, nothing changed. Switch it off first (README)."
        exit 1
    fi

    local missing="" p f pass answer tmp
    for p in $PACKAGES; do
        dpkg-query -W -f='${Status}' "$p" 2> /dev/null | grep -q "ok installed" || missing="$missing $p"
    done

    if [ -n "$missing" ]; then
        echo "installing$missing"
        apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y $missing ||
            { echo "apt failed: the UNO needs internet once for this, e.g. LAN2 at a router"; exit 1; }
    fi

    if [ -d /boot/efi/EFI ]; then # also starts when the BIOS loses its boot entries
        echo 'grub-efi-amd64 grub2/force_efi_extra_removable boolean true' | debconf-set-selections
        if [ ! -f /boot/efi/EFI/BOOT/BOOTX64.EFI ]; then
            grub-install --target=x86_64-efi --efi-directory=/boot/efi --force-extra-removable
        fi
    fi

    tmp=$(mktemp -d) # not in $DIR, the next upload as the UNO user could not delete files of root
    tar -xzf "$DIR/uno.tar.gz" -C "$tmp"
    for f in $(cd "$tmp/uno" && find . -type f | sort); do
        install -D -m 644 "$tmp/uno/$f" "/${f#./}"
        echo "copied /${f#./}"
    done

    mkdir -p /etc/lcbd
    for f in "$tmp"/configs/*.dat; do
        if [ -f "/etc/lcbd/${f##*/}" ]; then
            echo "kept /etc/lcbd/${f##*/}" # the service page saves here
        else
            cp "$f" /etc/lcbd/
            echo "copied configs/${f##*/} to /etc/lcbd"
        fi
    done

    rm -rf "$tmp"

    if [ ! -s /etc/lcbd/web.pass ]; then
        read -r -s -p "password for the service page (empty: read only): " pass
        echo
        if [ -n "$pass" ]; then
            (umask 077 && printf '%s\n' "$pass" > /etc/lcbd/web.pass)
        fi
    fi

    systemctl daemon-reload
    systemctl enable lcbd
    read -r -p "switch on the write protection (overlayroot)? Only after commissioning. [y/N] " answer
    if [ "$answer" = y ]; then
        sed -i 's/^overlayroot=.*/overlayroot="tmpfs"/' /etc/overlayroot.conf
    fi

    update-initramfs -u -k all # network names, i915 blacklist and overlayroot act in the initramfs too
    sync
    echo "SET UP. After the reboot the UNO is at 192.168.123.74 on LAN2 only. Then ./deploy.sh status and ./deploy.sh"
    read -r -p "reboot now? [y/N] " answer
    if [ "$answer" = y ]; then
        systemctl reboot
    fi
}

check() {
    if eval "$2" > /dev/null 2>&1 < /dev/null; then echo "ok  $1"; else echo "NO  $1"; fi
}

remote_status() {
    check "PREEMPT_RT kernel" 'uname -v | grep -q PREEMPT_RT'
    check "ecat0 = LAN1, link to the EK1100" 'ip link show ecat0 | grep -q "state UP"'
    check "sens0 = LAN2, 192.168.123.74/24" 'ip -4 addr show sens0 | grep -q "inet 192.168.123.74/24"'
    check "/dev/ttyNLR, latency timer 1 ms" \
        'grep -qx 1 "/sys/bus/usb-serial/devices/$(basename "$(readlink -f /dev/ttyNLR)")/latency_timer"'
    check "i915 not loaded" '! grep -q "^i915 " /proc/modules'
    check "hardware watchdog" '[ -e /dev/watchdog0 ]'
    check "setup.dat, sensor.dat, die.dat in /etc/lcbd" \
        '[ -f /etc/lcbd/setup.dat ] && [ -f /etc/lcbd/sensor.dat ] && [ -f /etc/lcbd/die.dat ]'
    check "/etc/lcbd/web.pass (service page can switch)" '[ -s /etc/lcbd/web.pass ]'
    check "lcbd starts at boot" 'systemctl is-enabled lcbd'
    check "lcbd runs" 'systemctl is-active lcbd'
    check "write protection (overlayroot)" overlay_on
}

confirm_restart() {
    read -r -p "lcbd restarts, the angle measurement stops for about a second. Press idle? [y/N] " answer
    [ "$answer" = y ] || { echo "stopped, nothing installed"; exit 1; }
}

repo_root() { git -C "$(dirname "$0")" rev-parse --show-toplevel; }

archive_head() {
    local repo
    repo=$(repo_root)
    if [ -n "$(git -C "$repo" status --porcelain -- "$@")" ]; then
        echo "note: uncommitted changes in $* are not deployed" >&2
    fi

    git -C "$repo" archive HEAD "$@" | gzip
}

upload() { # one ssh connection, one password prompt without an SSH key
    cp "$0" "$2/deploy.sh"
    tar -cf - -C "$2" . | ssh "$1" "rm -rf $DIR && mkdir -p $DIR && tar -xf - -C $DIR"
    rm -rf "$2"
}

local_deploy() {
    local uno=$1 repo url hash cache tmp
    repo=$(repo_root)
    url=$(sed -n 's/^ *set(SOEM_URL \(.*\))$/\1/p' "$repo/lcbd/CMakeLists.txt")
    hash=$(sed -n 's/.*URL_HASH SHA256=\([0-9a-f]*\).*/\1/p' "$repo/lcbd/CMakeLists.txt")
    cache=${XDG_CACHE_HOME:-$HOME/.cache}/lcbd/$(basename "$url")
    if [ ! -f "$cache" ]; then
        echo "downloading SOEM (the UNO has no internet): $url"
        mkdir -p "$(dirname "$cache")"
        curl -fsSL -o "$cache" "$url"
    fi

    echo "$hash  $cache" | sha256sum -c --quiet
    tmp=$(mktemp -d)
    archive_head lcbd testdata > "$tmp/src.tar.gz"
    cp "$cache" "$tmp/soem.tar.gz"
    upload "$uno" "$tmp"
    echo "== build and test on $uno: $(git -C "$repo" log -1 --format="%h %s")"
    ssh -n "$uno" "bash $DIR/deploy.sh remote-build"
    confirm_restart
    echo "== install (sudo asks for the password of the UNO user)"
    ssh -t "$uno" "sudo bash $DIR/deploy.sh remote-install"
}

local_rollback() {
    confirm_restart
    upload "$1" "$(mktemp -d)"
    ssh -t "$1" "sudo bash $DIR/deploy.sh remote-rollback"
}

local_setup() {
    local tmp
    tmp=$(mktemp -d)
    archive_head uno configs > "$tmp/uno.tar.gz"
    upload "$1" "$tmp"
    echo "== setup (sudo asks for the password of the UNO user)"
    ssh -t "$1" "sudo bash $DIR/deploy.sh remote-setup"
}

case "${1:-}" in
remote-build) remote_build ;;
remote-install) remote_install ;;
remote-rollback) remote_rollback ;;
remote-setup) remote_setup ;;
rollback) local_rollback "${2:-$DEFAULT_UNO}" ;;
setup) local_setup "${2:-$DEFAULT_UNO}" ;;
status) ssh "${2:-$DEFAULT_UNO}" "bash -s remote-status" < "$0" ;;
remote-status) remote_status ;;
-h | --help) usage ;;
*) local_deploy "${1:-$DEFAULT_UNO}" ;;
esac
