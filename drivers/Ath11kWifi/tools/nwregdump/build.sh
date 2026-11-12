#!/bin/sh
# Builds nwregdump.ko for the phone's stock GKI 6.1.172 kernel with Homebrew LLVM (no kernel tree; see nwregdump.c).
#   build.sh [OUT]   (default: nwregdump.ko next to this script)
set -e
here=$(cd "$(dirname "$0")" && pwd)
llvm=${LLVM:-/opt/homebrew/opt/llvm/bin}
out=${1:-$here/nwregdump.ko}
"$llvm/clang" --target=aarch64-linux-gnu -O2 -ffreestanding -fno-builtin -fno-stack-protector -ffixed-x18 -mgeneral-regs-only \
	-mbranch-protection=pac-ret+bti -fsanitize=kcfi -fno-pic -mcmodel=small -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-fno-common -std=gnu11 -Wall -c "$here/nwregdump.c" -o "$out"
echo "built $out"
