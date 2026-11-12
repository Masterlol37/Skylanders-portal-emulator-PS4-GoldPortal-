#!/bin/bash
# Builds goldportal.prx. Run from WSL:  bash build.sh
set -e
export OO_PS4_TOOLCHAIN="${OO_PS4_TOOLCHAIN:-$HOME/ps4/OpenOrbis/PS4Toolchain}"
export GOLDHEN_SDK="${GOLDHEN_SDK:-$HOME/ps4/GoldHEN_Plugins_SDK}"
cd "$(dirname "$0")"
make clean
make "$@"
echo
echo "Done: $(pwd)/goldportal.prx"
