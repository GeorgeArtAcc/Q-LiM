#!/bin/sh
# Q-LiM - Copyright (c) 2026 Georgi Stanimirov Ivanov. SPDX-License-Identifier: GPL-3.0-or-later (see LICENSE)
# Installs Q-LiM into the system (for the current user only, no sudo):
#   - the icon and an entry in the applications menu;
#   - step 36l: .limproject files get their own icon and open with Q-LiM (double click).
#   sh install-icon.sh          (run it from the qlim folder; run it again if you move the folder)
# Removal: rm ~/.local/share/applications/qlim.desktop ~/.local/share/mime/packages/qlim-project.xml
#         and ~/.local/share/icons/hicolor/*/apps/qlim.* ~/.local/share/icons/hicolor/*/mimetypes/application-x-qlim-project.png
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
ICONS="$HOME/.local/share/icons/hicolor"
for s in 16 24 32 48 64 128 256 512 1024; do
    mkdir -p "$ICONS/${s}x${s}/apps" "$ICONS/${s}x${s}/mimetypes"
    cp "$DIR/assets/icons/qlim-$s.png" "$ICONS/${s}x${s}/apps/qlim.png"
    cp "$DIR/assets/icons/qlim-project-$s.png" "$ICONS/${s}x${s}/mimetypes/application-x-qlim-project.png"
done
mkdir -p "$ICONS/scalable/apps"
cp "$DIR/assets/icons/qlim.svg" "$ICONS/scalable/apps/qlim.svg"

# the file type: *.limproject = "Q-LiM project" (also recognised by the first line "# Q-LiM project")
mkdir -p "$HOME/.local/share/mime/packages"
cat > "$HOME/.local/share/mime/packages/qlim-project.xml" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">
  <mime-type type="application/x-qlim-project">
    <comment>Q-LiM project</comment>
    <comment xml:lang="bg">Проект на Q-LiM</comment>
    <icon name="application-x-qlim-project"/>
    <generic-icon name="application-x-qlim-project"/>
    <glob pattern="*.limproject"/>
    <magic priority="60">
      <match type="string" offset="0" value="# Q-LiM project"/>
    </magic>
  </mime-type>
</mime-info>
XML

mkdir -p "$HOME/.local/share/applications"
cat > "$HOME/.local/share/applications/qlim.desktop" <<EOD
[Desktop Entry]
Type=Application
Name=Q-LiM
Comment=Clip launcher
Exec="$DIR/qlim" %f
Path=$DIR
Icon=qlim
Terminal=false
StartupWMClass=qlim
Categories=AudioVideo;Audio;
MimeType=application/x-qlim-project;
EOD

command -v update-mime-database >/dev/null 2>&1 && update-mime-database "$HOME/.local/share/mime" 2>/dev/null || true
# step 36l: the "application-x-generic" icon from the theme (Yaru) must not win over ours -> generic-icon is ours;
# the file manager keeps the old one in memory -> restart (it opens by itself the next time it is used)
for fm in nemo nautilus caja; do                  # Linux Mint (Nemo), Ubuntu (Nautilus), MATE (Caja)
    command -v $fm >/dev/null 2>&1 && pgrep -x $fm >/dev/null 2>&1 && $fm -q 2>/dev/null || true
done
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q -t "$ICONS" 2>/dev/null || true
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database -q "$HOME/.local/share/applications" 2>/dev/null || true
command -v xdg-mime >/dev/null 2>&1 && xdg-mime default qlim.desktop application/x-qlim-project 2>/dev/null || true
echo ">>> Q-LiM is in the applications menu (with its icon)."
echo ">>> .limproject files: own icon + double click opens them in Q-LiM."
echo "    (If the file manager still shows the old icon: close it and open it again, or log out and in.)"
