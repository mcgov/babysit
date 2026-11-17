#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Build the libFuzzer targets in tests/fuzz/ with AddressSanitizer and run
# them, by default on a small RAM disk that the run fills up.
#
# The targets keep their scratch files, and the output target a log that
# only ever grows, under $TMPDIR. This script points $TMPDIR at a RAM disk
# of a fixed size, so that some way into the run the disk is full and
# babysit's handling of a log it cannot write is fuzzed as well: what it
# could not log must be counted, and noted in the log once there is room
# again. See the top of tests/fuzz/fuzz_output.c.
#
# The RAM disk is a tmpfs mounted inside a private user and mount
# namespace (unshare), which needs no root and leaves nothing mounted: it
# is gone when the fuzzer exits. Where that is not allowed, mount one
# yourself and name it with --scratch-dir:
#
#     sudo mount -t tmpfs -o size=1G,uid=$(id -u) tmpfs /mnt/fuzz
#     tests/fuzz/run.sh --scratch-dir /mnt/fuzz
#
# The corpus, the fuzzer's log and any input that crashes a target are
# kept in the build directory, not on the RAM disk. To try a crashing
# input again:  build/fuzz/fuzz_output build/fuzz/crash-<hash>
set -u

root="$(cd "$(dirname "$0")/../.." && pwd)"
seconds=900
ramdisk_size="1G"
scratch_dir=""
target="all"
all_targets="cmdline output control instance conf pack"
build_dir="build/fuzz"
max_len=16384
cc="clang"

usage() {
    cat <<USAGE
Usage: $0 [options]

Build the fuzz targets with libFuzzer and AddressSanitizer, and run each.

Options:
  --seconds N          how long to run each target (default: $seconds)
  --ramdisk-size SIZE  size of the RAM disk the targets work on, as mount
                       takes it: 256M, 1G. "none" runs without one, in
                       \$TMPDIR or /tmp, and never fills a disk
                       (default: $ramdisk_size)
  --scratch-dir DIR    work in DIR, a small filesystem you have mounted
                       yourself, instead of making a RAM disk
  --target NAME        one of: $all_targets;
                       or all (default: $target)
  --build-dir DIR      where the targets, the corpus and the logs go,
                       relative to the repository (default: $build_dir)
  --max-len N          longest input to generate, in bytes
                       (default: $max_len)
  --cc COMPILER        clang to build with (default: $cc)
  --help               show this help

The targets, each a file tests/fuzz/fuzz_NAME.c that says more:
  cmdline   the options of every subcommand
  output    the babysitter's path from the command's pipes to the log;
            this is the one that fills the disk
  control   the control FIFO and the exit_code file
  instance  an instance on disk, read by check, info, cat, wait, watch
  conf      babysit.conf, as both programs read it
  pack      a housekeeping pass of babysitd over a made-up log directory

The output target fills a 64M RAM disk every second or two, and a 1G one
in about half a minute, so any run of a few minutes fills the disk many
times over. Each time, babysit must count what its log missed and say so
once there is room again.

Needs: clang with libFuzzer (compiler-rt), and unshare (util-linux) for
the RAM disk.
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --seconds) seconds="$2"; shift 2 ;;
        --ramdisk-size) ramdisk_size="$2"; shift 2 ;;
        --scratch-dir) scratch_dir="$2"; shift 2 ;;
        --target) target="$2"; shift 2 ;;
        --build-dir) build_dir="$2"; shift 2 ;;
        --max-len) max_len="$2"; shift 2 ;;
        --cc) cc="$2"; shift 2 ;;
        --help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

case "$target" in
    all) targets="$all_targets" ;;
    cmdline | output | control | instance | conf | pack) targets="$target" ;;
    *) echo "fuzz: no such target: $target" >&2; exit 2 ;;
esac

cd "$root" || exit 2
mkdir -p "$build_dir" || exit 2
build_dir="$(cd "$build_dir" && pwd)"
# shellcheck disable=SC2054  # the comma belongs to -fsanitize
flags=(-std=c99 -O1 -g -fno-omit-frame-pointer -Wall -Wextra -Werror
       -fsanitize=fuzzer,address -fno-sanitize-recover=all)

# Every target is one file, tests/fuzz/fuzz_<name>.c. Most are linked with
# all of babysit but the file with main(); conf and pack include babysitd.c
# themselves, and conf also needs babysit's reader of the same file. An
# exit() in the code under test is an answer, not a crash: -Dexit turns it
# into a jump back to the target (see tests/fuzz/fuzz_common.h).
build_target() {
    local srcs=()
    local f
    case "$1" in
        conf) srcs=(src/babysit_cmdline.c) ;;
        pack) ;;
        *) for f in src/babysit_*.c; do srcs+=("$f"); done ;;
    esac
    "$cc" "${flags[@]}" -Dexit=fuzz_exit -o "$build_dir/fuzz_$1" \
        "tests/fuzz/fuzz_$1.c" "${srcs[@]}"
}

# Where the targets work, and how to get there.
enter=()
fill=1
if [ -n "$scratch_dir" ]; then
    [ -d "$scratch_dir" ] && [ -w "$scratch_dir" ] ||
        { echo "fuzz: $scratch_dir is not a directory you can write to" >&2; exit 2; }
    scratch="$(cd "$scratch_dir" && pwd)"
    where="$scratch (yours)"
elif [ "$ramdisk_size" = "none" ]; then
    scratch="${TMPDIR:-/tmp}"
    where="$scratch, no RAM disk: the disk is not filled"
    fill=0
else
    scratch="$build_dir/ramdisk"
    mkdir -p "$scratch" || exit 2
    # shellcheck disable=SC2016  # expanded by the sh that unshare starts
    mounter='mount -t tmpfs -o "size=$1" tmpfs "$2" && shift 2 && exec "$@"'
    enter=(unshare --user --map-root-user --mount -- sh -c "$mounter" sh "$ramdisk_size" "$scratch")
    if ! "${enter[@]}" true 2>/dev/null; then
        echo "fuzz: cannot mount a RAM disk in a private namespace here." >&2
        echo "  mount one yourself and pass --scratch-dir, or use --ramdisk-size none;" >&2
        echo "  see $0 --help" >&2
        exit 2
    fi
    where="a $ramdisk_size RAM disk"
fi

failed=0
for t in $targets; do
    echo "== fuzz_$t: building"
    build_target "$t" || exit 2
    bin="$build_dir/fuzz_$t"
    log="$build_dir/$t.log"
    stats="$build_dir/$t.fill"
    mkdir -p "$build_dir/corpus/$t" || exit 2
    : >"$stats"
    env=(env "TMPDIR=$scratch")
    [ "$fill" -eq 1 ] && env+=("BABYSIT_FUZZ_FILL=$stats")

    echo "== fuzz_$t: ${seconds}s on $where"
    "${enter[@]}" "${env[@]}" "$bin" \
        "-max_total_time=$seconds" "-max_len=$max_len" -close_fd_mask=3 \
        -print_final_stats=1 "-artifact_prefix=$build_dir/" \
        "$build_dir/corpus/$t" "tests/fuzz/seeds/$t" 2>"$log"
    rc=$?
    grep -E '^(Done|stat::new_units_added|SUMMARY)' "$log"
    [ -s "$stats" ] && sed 's/^/fill: /' "$stats"
    if [ "$rc" -ne 0 ]; then
        echo "fuzz_$t FAILED (exit $rc); the end of $log:"
        tail -n 40 "$log"
        failed=1
    fi
done

if [ "$failed" -ne 0 ]; then
    echo "fuzz: failures; inputs that crash a target are in $build_dir/"
    exit 1
fi
echo "fuzz: no failures"
