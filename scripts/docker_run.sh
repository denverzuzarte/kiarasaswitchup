#!/bin/sh
# Run the image like the scorer: docker_run.sh DATA_DIR OUTPUT_DIR
# The image is FROM scratch, so it has no shell/chmod utility for cleanup.
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 DATA_DIR OUTPUT_DIR" >&2
    exit 2
fi

IMAGE="${IMAGE:-scratch_build:latest}"
CPUS="${CPUS:-8}"
MEM="${MEM:-24g}"
PLATFORM="${PLATFORM:-linux/amd64}"

DATA=$(cd "$1" && pwd)
mkdir -p "$2/user_daily" "$2/campaign_daily"
OUT=$(cd "$2" && pwd)
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/shopping-sessions.XXXXXX")

# Pre-create output directories on the host. This lets the host user remove
# result files later even though the container writes them as uid 1000.
chmod 777 "$OUT" "$OUT/user_daily" "$OUT/campaign_daily" "$SCRATCH"

cleanup() {
    # For nested scratch directories, the native program should set umask(0)
    # at startup (or create them with mode 0777) so the host can remove them.
    rm -rf "$SCRATCH" 2>/dev/null || {
        echo "warning: could not fully remove scratch directory: $SCRATCH" >&2
    }
}
trap cleanup EXIT HUP INT TERM

docker run --rm --platform "$PLATFORM" \
    --network none --read-only --tmpfs /tmp:rw,size=64m,mode=1777 \
    --cap-drop ALL --security-opt no-new-privileges \
    --pids-limit 4096 --cpus "$CPUS" --memory "$MEM" --memory-swap "$MEM" \
    --user 1000:1000 \
    -v "$DATA":/data:ro,z \
    -v "$OUT":/output:z \
    -v "$SCRATCH":/scratch:z \
    -e DATA_DIR=/data \
    -e OUTPUT_DIR=/output \
    -e SCRATCH_DIR=/scratch \
    -e TMPDIR=/scratch \
    -e HOME=/scratch \
    -e NCPU="${CPUS%.*}" \
    -e MEM_GB="${MEM%g}" \
    "$IMAGE"
