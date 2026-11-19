#!/bin/sh
# Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE)
#
# Old graphics cards + USB sound cards (Linux, X11): clicks / crackles in the sound while the screen is redrawn.
#
#   sh qlim-old-gpu-fix.sh on       turn the fix on   (needs root: sudo / doas)
#   sh qlim-old-gpu-fix.sh off      undo it, everything as before
#   sh qlim-old-gpu-fix.sh status   show if it is on
#
# What it does: tells the X server not to use the graphics card's 2D acceleration ("AccelMethod none", generic
# modesetting driver). On some older laptops (seen on a ThinkPad T60 with an ATI Radeon X1400, Intel 945 era) the
# card's memory transfers ("bus mastering") hold the computer's shared bus long enough to break the steady stream of
# a USB sound card, so the sound clicks while the picture changes - with ANY program, not only Q-LiM. Built-in sound
# cards are not affected. The price: drawing is done by the processor (a little more CPU use while things move).
# In Q-LiM, also try Edit > Performance > "Gentle on the graphics card" first - it is often enough.
#
# Only for X11 (Xorg). Wayland desktops and Windows are not touched. Takes effect after logging out and in
# (or a restart). It writes one file: /etc/X11/xorg.conf.d/20-qlim-no-accel.conf - "off" removes it.
CONF=/etc/X11/xorg.conf.d/20-qlim-no-accel.conf
need_root() {
    [ "$(id -u)" = 0 ] && return 0
    if command -v sudo >/dev/null 2>&1; then exec sudo sh "$0" "$@"; fi
    if command -v doas >/dev/null 2>&1; then exec doas sh "$0" "$@"; fi
    echo "Please run it as root (sudo or doas)."; exit 1
}
case "$1" in
on)
    need_root "$@"
    if [ -n "$WAYLAND_DISPLAY" ] || [ "$XDG_SESSION_TYPE" = wayland ]; then
        echo "Note: this is a Wayland session - the fix only works for X11 (Xorg) sessions."
    fi
    if grep -q '^nvidia ' /proc/modules 2>/dev/null && [ "$2" != force ]; then
        echo "The NVIDIA driver is in use - this fix is not meant for it (the screen might not start). Not changed."
        echo "(If you are sure: sh $0 on force)"
        exit 1
    fi
    mkdir -p /etc/X11/xorg.conf.d
    cat > "$CONF" <<'EOF'
# Written by Q-LiM (qlim-old-gpu-fix.sh on) - remove with: sh qlim-old-gpu-fix.sh off
# No 2D acceleration: old graphics cards otherwise break the stream of USB sound cards (clicks while drawing).
Section "Device"
    Identifier "QLiM-no-accel"
    Driver     "modesetting"
    Option     "AccelMethod" "none"
EndSection
EOF
    chmod 644 "$CONF"
    echo "The fix is ON ($CONF). Log out and in again (or restart) to use it."
    echo "To undo: sh $0 off"
    ;;
off)
    need_root "$@"
    if [ -f "$CONF" ]; then rm -f "$CONF"; echo "The fix is OFF (removed $CONF). Log out and in again (or restart)."
    else echo "The fix was not on - nothing to undo."; fi
    ;;
status)
    if [ -f "$CONF" ]; then echo "ON ($CONF)"; else echo "OFF"; fi
    ;;
*)
    sed -n '4,8p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
    ;;
esac
