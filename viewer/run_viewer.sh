#!/usr/bin/env bash
# Launch the DECT camera viewer.
#
#   ./run_viewer.sh                          # live from the RX board's VCOM1
#   ./run_viewer.sh --port /dev/ttyACM3
#   ./run_viewer.sh --replay capture.bin --replay-rate 60
#   ./run_viewer.sh --no-gui                 # statistics only
set -eu
cd "$(dirname "$0")"

# NCS sets these and they break the venv's python.
unset PYTHONHOME PYTHONPATH

# opencv-python ships Qt but no fonts, which produces a wall of
# "QFontDatabase: Cannot find font directory" warnings. Point Qt at the
# system fonts instead.
QT_FONTS="$PWD/.venv/lib/python3.12/site-packages/cv2/qt/fonts"
if [ ! -d "$QT_FONTS" ] && [ -d /usr/share/fonts/truetype/dejavu ]; then
    mkdir -p "$QT_FONTS"
    cp -n /usr/share/fonts/truetype/dejavu/*.ttf "$QT_FONTS/" 2>/dev/null || true
fi

exec ./.venv/bin/python -m dectcam_viewer.viewer "$@"
