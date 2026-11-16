#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Run babysit's make targets on FreeBSD, in a throwaway QEMU virtual machine.
#
# GitLab has no hosted FreeBSD runners, so this is how FreeBSD gets tested
# from a Linux machine. The official FreeBSD cloud image is downloaded once
# and cached; every run boots a fresh copy-on-write overlay of it, installs
# bash and gmake on first boot, copies the source tree in over SSH, runs the
# targets, and throws the overlay away.
set -u

release="15.1"
cache_dir="${XDG_CACHE_HOME:-$HOME/.cache}/babysit/freebsd-vm"
ssh_port="2222"
memory="2048"
cpus="2"
targets="test check-asan check-ubsan check-tsan freebsd-pkg"
boot_timeout="600"
keep=0

usage() {
    cat <<USAGE
Usage: $0 [options]

Run make targets for babysit inside a throwaway FreeBSD (amd64) VM.

Options:
  --release VER        FreeBSD release to test on (default: $release)
  --targets "T ..."    gmake targets to run, in order
                       (default: "$targets")
  --cache-dir DIR      where the downloaded image is kept
                       (default: $cache_dir)
  --ssh-port PORT      local port forwarded to the VM's sshd (default: $ssh_port)
  --memory MB          VM memory (default: $memory)
  --cpus N             VM cpus (default: $cpus)
  --boot-timeout SECS  how long to wait for the VM to come up (default: $boot_timeout)
  --keep               leave the VM running afterwards and print how to reach it
  --help               show this help

Needs: qemu-system-x86_64, qemu-img, genisoimage or xorriso, curl, xz, ssh,
ssh-keygen, sha256sum. Uses KVM when /dev/kvm is usable; without it the VM
is emulated and several times slower.
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --release) release="$2"; shift 2 ;;
        --targets) targets="$2"; shift 2 ;;
        --cache-dir) cache_dir="$2"; shift 2 ;;
        --ssh-port) ssh_port="$2"; shift 2 ;;
        --memory) memory="$2"; shift 2 ;;
        --cpus) cpus="$2"; shift 2 ;;
        --boot-timeout) boot_timeout="$2"; shift 2 ;;
        --keep) keep=1; shift ;;
        --help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

die() { echo "freebsd-vm: $*" >&2; exit 1; }
for tool in qemu-system-x86_64 qemu-img curl xz ssh ssh-keygen sha256sum tar; do
    command -v "$tool" >/dev/null || die "missing required tool: $tool"
done
if command -v genisoimage >/dev/null; then
    mkiso() { genisoimage -quiet -output "$1" -volid cidata -joliet -rock "$2"/user-data "$2"/meta-data; }
elif command -v xorriso >/dev/null; then
    mkiso() { xorriso -as mkisofs -quiet -o "$1" -V cidata -J -R "$2"/user-data "$2"/meta-data; }
else
    die "missing required tool: genisoimage or xorriso"
fi

srcdir="$(cd "$(dirname "$0")/.." && pwd)"
name="FreeBSD-$release-RELEASE-amd64-BASIC-CLOUDINIT-ufs.qcow2"
url="https://download.freebsd.org/releases/VM-IMAGES/$release-RELEASE/amd64/Latest"
base="$cache_dir/$name"

# ---- the cached base image ----
mkdir -p "$cache_dir" || die "cannot create $cache_dir"
if [ ! -f "$base" ]; then
    echo "freebsd-vm: downloading $name.xz"
    curl -fL --progress-bar -o "$base.xz.part" "$url/$name.xz" || die "download failed"
    curl -fsL -o "$cache_dir/CHECKSUM.SHA256" "$url/CHECKSUM.SHA256" || die "checksum download failed"
    want=$(sed -n "s/^SHA256 ($name.xz) = //p" "$cache_dir/CHECKSUM.SHA256")
    got=$(sha256sum "$base.xz.part" | cut -d' ' -f1)
    [ -n "$want" ] && [ "$want" = "$got" ] || { rm -f "$base.xz.part"; die "checksum mismatch for $name.xz"; }
    mv "$base.xz.part" "$base.xz"
    xz -d -T0 "$base.xz" || die "cannot unpack image"
fi

# ---- a throwaway VM ----
run="$(mktemp -d)"
qemu_pid=""
cleanup() {
    if [ "$keep" -eq 0 ]; then
        [ -n "$qemu_pid" ] && kill "$qemu_pid" 2>/dev/null
        rm -rf "$run"
    fi
}
trap cleanup EXIT

ssh-keygen -q -t ed25519 -N '' -f "$run/id" || die "ssh-keygen failed"
mkdir "$run/seed"
printf 'instance-id: babysit-%s\nlocal-hostname: babysit-test\n' "$$" >"$run/seed/meta-data"
cat >"$run/seed/user-data" <<SEED
#cloud-config
users:
  - default
ssh_authorized_keys:
  - $(cat "$run/id.pub")
packages:
  - bash
  - gmake
runcmd:
  - pkg install -y bash gmake
  - touch /var/tmp/firstboot-done
SEED
mkiso "$run/seed.iso" "$run/seed" || die "cannot build the cloud-init seed"
qemu-img create -q -f qcow2 -b "$base" -F qcow2 "$run/disk.qcow2" 12G || die "cannot create overlay disk"

accel=(-accel tcg)
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    accel=(-accel kvm -cpu host)
else
    echo "freebsd-vm: /dev/kvm is not usable; emulating (slow)"
fi
qemu-system-x86_64 "${accel[@]}" -m "$memory" -smp "$cpus" \
    -drive "file=$run/disk.qcow2,if=virtio,format=qcow2" \
    -drive "file=$run/seed.iso,media=cdrom" \
    -netdev "user,id=n0,hostfwd=tcp:127.0.0.1:$ssh_port-:22" \
    -device virtio-net-pci,netdev=n0 \
    -display none -serial "file:$run/serial.log" \
    -daemonize -pidfile "$run/qemu.pid" || die "cannot start qemu (is port $ssh_port in use?)"
qemu_pid=$(cat "$run/qemu.pid")

vm() {
    ssh -i "$run/id" -p "$ssh_port" -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null -o BatchMode=yes -o LogLevel=ERROR \
        -o ConnectTimeout=5 freebsd@127.0.0.1 "$@"
}

echo "freebsd-vm: waiting for FreeBSD $release to boot"
waited=0
until vm 'test -e /var/tmp/firstboot-done && command -v gmake >/dev/null && command -v bash >/dev/null' 2>/dev/null; do
    sleep 5
    waited=$((waited + 5))
    if [ "$waited" -ge "$boot_timeout" ]; then
        tail -n 20 "$run/serial.log" >&2
        die "the VM did not come up within ${boot_timeout}s"
    fi
done
echo "freebsd-vm: up after ${waited}s: $(vm uname -srm)"

tar -C "$srcdir" -czf - --exclude=./bin --exclude=./build --exclude=./.git . |
    vm 'mkdir -p babysit && tar -xzf - -C babysit' || die "cannot copy the source tree"

status=0
for t in $targets; do
    echo "freebsd-vm: gmake $t"
    if ! vm "cd babysit && gmake $t"; then
        echo "freebsd-vm: FAILED: gmake $t" >&2
        status=1
    fi
done

if [ "$keep" -eq 1 ]; then
    echo "freebsd-vm: VM left running (qemu pid $qemu_pid, files in $run)"
    echo "  ssh -i $run/id -p $ssh_port -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null freebsd@127.0.0.1"
    echo "  stop it with: kill $qemu_pid"
fi
[ "$status" -eq 0 ] && echo "freebsd-vm: all targets passed on $(vm uname -srm)"
exit "$status"
