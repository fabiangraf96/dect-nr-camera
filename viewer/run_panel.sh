#!/usr/bin/env bash
# Launch the viewer with its control side panel.
#
#   ./run_panel.sh                        # autodetects the receiver's VCOM1
#   ./run_panel.sh --port /dev/ttyACM2
#   ./run_panel.sh --replay capture.bin   # controls are inert
set -eu
cd "$(dirname "$0")"

# NCS sets these and they break the venv's python.
unset PYTHONHOME PYTHONPATH

exec ./.venv/bin/python -m dectcam_viewer.panel "$@"
