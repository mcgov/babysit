#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Run the jobs of .gitlab-ci.yml on this machine, each in a fresh container.
#
# The jobs are not copied into this script: their image, before_script and
# script are read from .gitlab-ci.yml every time, so what runs here is what
# GitLab would run. For each job a container is started from the job's
# image, the working tree is copied into it (tracked files and new files
# that are not ignored, so uncommitted changes are tested too), and the
# job's commands are run there. Nothing is written to the working tree
# except the job's build/ directory, which is copied out afterwards.
set -u

root="$(cd "$(dirname "$0")/.." && pwd)"
ci_file="$root/.gitlab-ci.yml"
out_dir="$root/build/ci-local"
engine=""
jobs=()
all=0
list=0
shell_after=0

usage() {
    cat <<USAGE
Usage: $0 [options]

Run jobs from .gitlab-ci.yml locally, each in a fresh container.

Options:
  --job NAME        run this job; may be given more than once
  --all             run every job that needs no special runner (this is the
                    default when no --job is given)
  --list            list the jobs and where they can run, then exit
  --engine NAME     container engine: podman or docker
                    (default: podman if installed, else docker)
  --ci-file FILE    pipeline definition to read (default: $ci_file)
  --out-dir DIR     where each job's build/ directory is copied to, as
                    DIR/<job>/ (default: $out_dir)
  --shell           after a job's commands, failed or not, open a shell in
                    its container; needs a terminal and exactly one --job
  --help            show this help

What it cannot do:
  - Jobs that GitLab adds from its own templates (secret_detection,
    semgrep-sast) are not in the file and are not run.
  - Jobs for FreeBSD and macOS runners are listed but cannot run in a
    container; for FreeBSD see tests/freebsd-vm.sh.
  - The arm64 jobs run under emulation (--platform linux/arm64), which
    needs qemu-user-static and is slow. They are left out of --all.

Needs: podman or docker, python3 with the yaml module (python3-pyyaml), git,
tar.
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --job) jobs+=("$2"); shift 2 ;;
        --all) all=1; shift ;;
        --list) list=1; shift ;;
        --engine) engine="$2"; shift 2 ;;
        --ci-file) ci_file="$2"; shift 2 ;;
        --out-dir) out_dir="$2"; shift 2 ;;
        --shell) shell_after=1; shift ;;
        --help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

die() { echo "ci-local: $*" >&2; exit 2; }

command -v python3 >/dev/null || die "python3 is needed to read $ci_file"
python3 -c 'import yaml' 2>/dev/null ||
    die "the python yaml module is needed (Fedora: python3-pyyaml, Debian: python3-yaml)"
[ -r "$ci_file" ] || die "cannot read $ci_file"

# ci_query list            one line per job: name, where it runs, image
# ci_query script NAME     the job's before_script and script, one command
#                          per line
# "where it runs" is "container", "arm64" (container, emulated), or the tag
# of the runner the job needs.
ci_query() {
    python3 -I - "$ci_file" "$@" <<'PY'
import sys, yaml

doc = yaml.safe_load(open(sys.argv[1]))
RESERVED = {"include", "variables", "stages", "workflow", "default", "image",
            "before_script", "after_script", "services", "cache"}
defaults = doc.get("default") or {}


def resolve(name, seen=()):
    """The job with everything it extends merged in, nearest first."""
    if name in seen or name not in doc:
        sys.exit("ci-local: cannot resolve '%s' in the pipeline file" % name)
    job = dict(doc[name])
    parents = job.pop("extends", [])
    if isinstance(parents, str):
        parents = [parents]
    merged = {}
    for parent in parents:
        merged.update(resolve(parent, seen + (name,)))
    merged.update(job)
    return merged


def lines(value):
    """Flatten a script entry, which may nest lists, into commands."""
    if value is None:
        return []
    if isinstance(value, str):
        return [value]
    out = []
    for item in value:
        out.extend(lines(item))
    return out


def where(job):
    tags = job.get("tags") or []
    if not tags:
        return "container"
    if any("arm64" in t for t in tags):
        return "arm64"
    return tags[0]


names = [n for n, v in doc.items()
         if isinstance(v, dict) and not n.startswith(".") and n not in RESERVED
         and "script" in resolve(n)]

if sys.argv[2] == "list":
    for n in names:
        job = resolve(n)
        print("%s\t%s\t%s" % (n, where(job),
                              job.get("image") or defaults.get("image", "")))
elif sys.argv[2] == "script":
    if sys.argv[3] not in names:
        sys.exit("ci-local: no job '%s' in the pipeline file" % sys.argv[3])
    job = resolve(sys.argv[3])
    before = job.get("before_script", defaults.get("before_script"))
    for cmd in lines(before) + lines(job.get("script")):
        print(" ".join(cmd.split()))
PY
}

listing="$(ci_query list)" || exit 2

if [ "$list" -eq 1 ]; then
    printf '%-14s %-12s %s\n' JOB RUNS IMAGE
    while IFS=$'\t' read -r name where image; do
        case "$where" in
            container) note="here" ;;
            arm64) note="emulated" ;;
            *) note="no ($where)" ;;
        esac
        printf '%-14s %-12s %s\n' "$name" "$note" "$image"
    done <<<"$listing"
    exit 0
fi

if [ "${#jobs[@]}" -eq 0 ] || [ "$all" -eq 1 ]; then
    while IFS=$'\t' read -r name where _; do
        [ "$where" = container ] && jobs+=("$name")
    done <<<"$listing"
fi

if [ -z "$engine" ]; then
    if command -v podman >/dev/null; then engine=podman
    elif command -v docker >/dev/null; then engine=docker
    else die "neither podman nor docker is installed; see --engine"
    fi
fi
command -v "$engine" >/dev/null || die "$engine is not installed"
if [ "$shell_after" -eq 1 ]; then
    [ "${#jobs[@]}" -eq 1 ] || die "--shell needs exactly one --job"
    [ -t 0 ] && [ -t 1 ] || die "--shell needs a terminal"
fi

# The working tree as a tar stream: what is tracked, plus new files that
# are not ignored. Files deleted but not yet committed are left out.
tree_tar() {
    (cd "$root" && git ls-files -co --exclude-standard -z |
        while IFS= read -r -d '' f; do [ -e "$f" ] && printf '%s\0' "$f"; done |
        tar -cf - --null -T -)
}

run_job() { # name
    local name="$1" where image script cname rc platform=()
    where="$(awk -F '\t' -v n="$name" '$1 == n { print $2 }' <<<"$listing")"
    image="$(awk -F '\t' -v n="$name" '$1 == n { print $3 }' <<<"$listing")"
    [ -n "$where" ] || { echo "ci-local: no job '$name' in $ci_file" >&2; return 2; }
    case "$where" in
        container) ;;
        arm64) platform=(--platform linux/arm64) ;;
        *) echo "ci-local: $name needs a '$where' runner and cannot run in a container" >&2
           return 2 ;;
    esac
    script="$(ci_query script "$name")" || return 2

    cname="babysit-ci-$name-$$"
    echo "== $name ($image)"
    # Process 1 of the container is a shell that does nothing but wait. A
    # shell, not "sleep": orphans are handed to process 1, and the tests
    # leave some on purpose, which a shell collects and sleep would leave
    # as zombies.
    "$engine" create --name "$cname" "${platform[@]}" -e CI=true --stop-timeout 0 \
        -i "$image" bash -c 'trap exit TERM; while :; do sleep 3600 & wait; done' \
        >/dev/null || return 2
    "$engine" start "$cname" >/dev/null || { "$engine" rm -f "$cname" >/dev/null; return 2; }
    "$engine" exec "$cname" mkdir -p /src
    tree_tar | "$engine" exec -i "$cname" tar -xf - -C /src
    # GitLab runs a job's commands with errexit and pipefail set.
    "$engine" exec -i -w /src "$cname" bash -eo pipefail -c "$script" </dev/null
    rc=$?
    if "$engine" exec "$cname" test -d /src/build; then
        rm -rf "${out_dir:?}/$name"
        mkdir -p "$out_dir/$name"
        "$engine" cp "$cname:/src/build/." "$out_dir/$name/" >/dev/null 2>&1 ||
            echo "ci-local: could not copy build/ out of the $name container" >&2
    fi
    if [ "$shell_after" -eq 1 ]; then
        echo "ci-local: $name exited $rc; opening a shell in its container (exit to remove it)"
        "$engine" exec -it -w /src "$cname" bash
    fi
    "$engine" rm -f "$cname" >/dev/null
    return "$rc"
}

failed=()
passed=()
for job in "${jobs[@]}"; do
    if run_job "$job"; then passed+=("$job"); else failed+=("$job"); fi
done

echo
echo "passed: ${passed[*]:-none}"
if [ "${#failed[@]}" -gt 0 ]; then
    echo "FAILED: ${failed[*]}"
    echo "build output of each job, where it made any, is under $out_dir/"
    exit 1
fi
