#!/usr/bin/env python3
"""Pack a Tributary zip for one MPC architecture.

  python3 tools/pack_release.py --so vst/build/tributary.so \
      --skin "vst/build/skin/johnny - VST - Tributary" --arch armv7 --version 1.0.0

armv7 writes pluginList-arm. aarch64 writes pluginList-arm-64bit.
The .so is not committed; it only goes in the zip this script writes under dist/.
"""
import argparse
import hashlib
import os
import shutil
import stat
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

ARCHES = {
    "armv7": {
        "list": "pluginList-arm",
        "uname": "armv7*",
        "words": "32-bit ARM (MPC Live, Live II, One, X, Force, Key 61)",
        "zip": "mpc-armv7",
    },
    "aarch64": {
        "list": "pluginList-arm-64bit",
        "uname": "aarch64",
        "words": "64-bit ARM (MPC Gen2, AArch64)",
        "zip": "mpc-aarch64",
    },
}

INSTALL = r"""#!/bin/sh
# Tributary @VERSION@ installer. This package is @ARCH@ (@WORDS@).
# It registers the plugin in @LIST@.
#   sh install.sh [-y] [-t <synths-dir>]
set -e
cd "$(dirname "$0")"
NAME='Tributary'
SO='tributary.so'
SKIN='johnny - VST - Tributary'
UID_HEX='54726962'
ARCH='@ARCH@'
LIST='@LIST@'
SYNTHS=""
YES=0
die() { echo "error: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
    case "$1" in
        -y) YES=1; shift ;;
        -t) [ -n "$2" ] || die "-t needs a folder"; SYNTHS="$2"; shift 2 ;;
        *) die "usage: sh install.sh [-y] [-t <synths-dir>]" ;;
    esac
done
[ "$(id -u)" = 0 ] || die "run as root"
case "$ARCH" in
    armv7) case "$(uname -m)" in armv7*) ;; *) die "this package is 32-bit ARM; this device is $(uname -m)" ;; esac ;;
    aarch64) case "$(uname -m)" in aarch64) ;; *) die "this package is 64-bit ARM; this device is $(uname -m)" ;; esac ;;
    *) die "unknown package architecture $ARCH" ;;
esac
command -v systemctl >/dev/null || die "systemctl not found"
SETTINGS=$(ls /media/az01-internal/Settings/*/MPC.settings 2>/dev/null | head -n 1)
[ -n "$SETTINGS" ] && [ -f "$SETTINGS" ] || die "MPC.settings not found"
if [ -z "$SYNTHS" ]; then
    if [ -d /media/EOS_DIGITAL/Synths ]; then SYNTHS=/media/EOS_DIGITAL/Synths
    else SYNTHS=/sdcard/Synths
    fi
fi
case "$SYNTHS" in /*) ;; *) die "-t must be an absolute path" ;; esac
FILE="$SYNTHS/$SKIN/$SO"
[ -f "portable/$SKIN/plugin-meta.xml" ] || die "package is damaged: portable/$SKIN is missing"
sha256sum -c SHA256SUMS >/dev/null 2>&1 || die "files damaged (SHA256SUMS mismatch)"
mkdir -p "$SYNTHS" || die "cannot create $SYNTHS"

echo "Installing $NAME @VERSION@ ($ARCH) into $LIST:"
echo "  $FILE"
if [ "$YES" = 0 ]; then
    printf "MPC will be stopped and restarted. Save your project first. Continue? [y/N] "
    read -r ok
    case "$ok" in y|Y|yes) ;; *) echo "cancelled"; exit 1 ;; esac
fi

if systemctl cat acvs >/dev/null 2>&1; then SVC=acvs
elif systemctl cat inmusic-mpc >/dev/null 2>&1; then SVC=inmusic-mpc
else SVC=acvs
fi
systemctl stop "$SVC"
trap 'systemctl start "$SVC"' EXIT
i=0
while pidof MPC >/dev/null && [ "$i" -lt 30 ]; do sleep 1; i=$((i + 1)); done
pidof MPC >/dev/null && die "MPC did not stop"

rm -rf "$SYNTHS/$SKIN"
cp -a "portable/$SKIN" "$SYNTHS/$SKIN"
sed "s|%payload-path%|$SYNTHS|g" "portable/$SKIN/plugin-meta.xml" > "$SETTINGS.entry"
awk -v mode=add -v list="$LIST" -v file="$FILE" -v uid="$UID_HEX" -v entryfile="$SETTINGS.entry" \
    -f plugin_list.awk "$SETTINGS" > "$SETTINGS.new"
rm -f "$SETTINGS.entry"
n=$(grep -c "file=\"$FILE\"" "$SETTINGS.new" || true)
u=$(grep -c " uid=\"$UID_HEX\"" "$SETTINGS.new" || true)
[ "$n" = 1 ] && [ "$u" = 1 ] || { rm -f "$SETTINGS.new"; die "settings edit failed (entries: $n by file, $u by uid)"; }
grep -q '<PROPERTIES' "$SETTINGS.new" && grep -q '</PROPERTIES>' "$SETTINGS.new" ||
    { rm -f "$SETTINGS.new"; die "edited settings lost their root element"; }
mv "$SETTINGS.new" "$SETTINGS"
sync
echo "Done. Add Tributary to a track. The plugin list entry is in $LIST."
"""

UNINSTALL = r"""#!/bin/sh
# Tributary @VERSION@ uninstaller. Removes the @ARCH@ entry from @LIST@.
#   sh uninstall.sh [-y] [-t <synths-dir>]
set -e
cd "$(dirname "$0")"
NAME='Tributary'
SO='tributary.so'
SKIN='johnny - VST - Tributary'
UID_HEX='54726962'
LIST='@LIST@'
SYNTHS=""
YES=0
die() { echo "error: $*" >&2; exit 1; }
while [ $# -gt 0 ]; do
    case "$1" in
        -y) YES=1; shift ;;
        -t) [ -n "$2" ] || die "-t needs a folder"; SYNTHS="$2"; shift 2 ;;
        *) die "usage: sh uninstall.sh [-y] [-t <synths-dir>]" ;;
    esac
done
[ "$(id -u)" = 0 ] || die "run as root"
SETTINGS=$(ls /media/az01-internal/Settings/*/MPC.settings 2>/dev/null | head -n 1)
[ -n "$SETTINGS" ] && [ -f "$SETTINGS" ] || die "MPC.settings not found"
if [ -z "$SYNTHS" ]; then
    if [ -d /media/EOS_DIGITAL/Synths ]; then SYNTHS=/media/EOS_DIGITAL/Synths
    else SYNTHS=/sdcard/Synths
    fi
fi
FILE="$SYNTHS/$SKIN/$SO"
echo "Removing $NAME from $LIST and $SYNTHS/$SKIN"
if [ "$YES" = 0 ]; then
    printf "MPC will be stopped and restarted. Continue? [y/N] "
    read -r ok
    case "$ok" in y|Y|yes) ;; *) echo "cancelled"; exit 1 ;; esac
fi
if systemctl cat acvs >/dev/null 2>&1; then SVC=acvs
elif systemctl cat inmusic-mpc >/dev/null 2>&1; then SVC=inmusic-mpc
else SVC=acvs
fi
systemctl stop "$SVC"
trap 'systemctl start "$SVC"' EXIT
i=0
while pidof MPC >/dev/null && [ "$i" -lt 30 ]; do sleep 1; i=$((i + 1)); done
pidof MPC >/dev/null && die "MPC did not stop"
awk -v mode=remove -v list="$LIST" -v file="$FILE" -v uid="$UID_HEX" \
    -f plugin_list.awk "$SETTINGS" > "$SETTINGS.new"
grep -q '<PROPERTIES' "$SETTINGS.new" && grep -q '</PROPERTIES>' "$SETTINGS.new" ||
    { rm -f "$SETTINGS.new"; die "edited settings lost their root element"; }
mv "$SETTINGS.new" "$SETTINGS"
rm -rf "$SYNTHS/$SKIN"
sync
echo "Done."
"""

INSTALL_TXT = """Tributary {version}

Plays one Overbridge output from an Elektron Digitone II on the MPC track the plugin sits on.

This zip is the {words} build.
The installer writes the plugin into {list}.
A 32-bit package uses pluginList-arm. A Gen2 / AArch64 package uses pluginList-arm-64bit.
The name is chosen by the package architecture.

Requirements
- Root shell (SSH) on the MPC.
- Digitone II on OS 1.10A or newer, set to Overbridge, in the MPC USB host port.
- Use the MPC as the clock. Other Overbridge machines are listed and untested.

Install
1. Copy this folder to the MPC, for example /tmp/{top}
2. ssh root@<MPC> sh /tmp/{top}/install.sh

If Synths lives somewhere else, pass it:
  sh install.sh -t /media/EOS_DIGITAL/Synths

The installer stops MPC, copies "johnny - VST - Tributary" into the Synths folder,
backs up MPC.settings, adds the plugin line to {list}, and starts MPC again.
Add -y to skip the question.

Uninstall
  sh uninstall.sh

By hand
- Copy portable/johnny - VST - Tributary/ into the Synths folder.
- Stop MPC (systemctl stop acvs, or inmusic-mpc where that is the service).
- Back up MPC.settings.
- Inside <VALUE name="{list}"><KNOWNPLUGINS>, add the line from
  portable/johnny - VST - Tributary/plugin-meta.xml, with %payload-path%
  replaced by the Synths folder.
- Start MPC.

There is a few milliseconds of latency. The plugin cannot delay the other tracks.
Recording needs a resample. Eight copies is the MPC limit unless Hakai is installed.

Demo: https://youtu.be/QJIlWZKR8m0
Licence: GPL-3. Channel maps are facts from Overwitch by David García Goñi, also GPL-3.
"""


def write_exe(path, text):
    with open(path, "w", newline="\n") as f:
        f.write(text)
    os.chmod(path, 0o755)


def walk(top):
    for d, dirs, files in os.walk(top):
        dirs.sort()
        for name in sorted(files):
            yield os.path.join(d, name)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--so", required=True)
    ap.add_argument("--skin", required=True)
    ap.add_argument("--arch", required=True, choices=sorted(ARCHES))
    ap.add_argument("--version", default="1.0.0")
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"))
    args = ap.parse_args()
    info = ARCHES[args.arch]
    skin_name = os.path.basename(os.path.normpath(args.skin))
    if skin_name != "johnny - VST - Tributary":
        raise SystemExit("skin folder must be named 'johnny - VST - Tributary'")
    if os.path.basename(args.so) != "tributary.so":
        raise SystemExit("--so must be tributary.so")
    for need in ("version.xml", "Plugin Skins/TUI.json"):
        if not os.path.exists(os.path.join(args.skin, need)):
            raise SystemExit("skin is missing " + need)

    top = "Tributary-%s" % args.version
    stage = tempfile.mkdtemp()
    root = os.path.join(stage, top)
    dest = os.path.join(root, "portable", skin_name)
    shutil.copytree(args.skin, dest)
    shutil.copy2(args.so, os.path.join(dest, "tributary.so"))
    meta = (
        '<PLUGIN name="Tributary" descriptiveName="Tributary" format="VST" '
        'category="Synth" manufacturer="johnny" version="1.0" '
        'file="%payload-path%/' + skin_name + '/tributary.so" uid="54726962" isInstrument="1" '
        'fileTime="0" infoUpdateTime="0" numInputs="0" numOutputs="2" isShell="0"/>\n'
    )
    with open(os.path.join(dest, "plugin-meta.xml"), "w", newline="\n") as f:
        f.write(meta)
    shutil.copy2(os.path.join(HERE, "plugin_list.awk"), root)
    repl = {
        "@VERSION@": args.version,
        "@ARCH@": args.arch,
        "@LIST@": info["list"],
        "@WORDS@": info["words"],
    }
    inst = INSTALL
    uninst = UNINSTALL
    for k, v in repl.items():
        inst = inst.replace(k, v)
        uninst = uninst.replace(k, v)
    write_exe(os.path.join(root, "install.sh"), inst)
    write_exe(os.path.join(root, "uninstall.sh"), uninst)
    txt = INSTALL_TXT.format(
        version=args.version, words=info["words"], list=info["list"], top=top
    )
    with open(os.path.join(root, "INSTALL.txt"), "w", newline="\n") as f:
        f.write(txt)

    sums = []
    for path in walk(root):
        digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
        sums.append("%s  %s" % (digest, os.path.relpath(path, root)))
    with open(os.path.join(root, "SHA256SUMS"), "w", newline="\n") as f:
        f.write("\n".join(sums) + "\n")

    os.makedirs(args.out, exist_ok=True)
    zpath = os.path.join(args.out, "%s-%s.zip" % (top, info["zip"]))
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for path in walk(root):
            rel = os.path.relpath(path, stage)
            info_z = zipfile.ZipInfo(rel, date_time=(2026, 1, 1, 0, 0, 0))
            info_z.create_system = 3
            info_z.compress_type = zipfile.ZIP_DEFLATED
            mode = 0o755 if os.stat(path).st_mode & stat.S_IXUSR else 0o644
            info_z.external_attr = (stat.S_IFREG | mode) << 16
            with open(path, "rb") as f:
                z.writestr(info_z, f.read())
    shutil.rmtree(stage)
    print("%s (%d bytes)" % (zpath, os.path.getsize(zpath)))


if __name__ == "__main__":
    main()
