#!/bin/sh
# Offline installer checks. All downloads and uname responses are fixtures.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT INT TERM
REAL_PATH=$PATH
mkdir -p "$TEST_DIR/bin" "$TEST_DIR/payload" "$TEST_DIR/downloads"
for tool in sh tar gzip awk mktemp rm mkdir install cp; do
  ln -s "$(command -v "$tool")" "$TEST_DIR/bin/$tool"
done
printf '#!/bin/sh\nprintf "fixture binary\\n"\n' > "$TEST_DIR/payload/loutre-view"
chmod +x "$TEST_DIR/payload/loutre-view"
tar -czf "$TEST_DIR/archive.tar.gz" -C "$TEST_DIR/payload" loutre-view
if command -v sha256sum >/dev/null 2>&1; then
  HASH=$(sha256sum "$TEST_DIR/archive.tar.gz" | awk '{print $1}')
else
  HASH=$(shasum -a 256 "$TEST_DIR/archive.tar.gz" | awk '{print $1}')
fi
cat > "$TEST_DIR/bin/uname" <<'SH'
#!/bin/sh
case "$1" in -s) printf '%s\n' "$MOCK_OS";; -m) printf '%s\n' "$MOCK_ARCH";; *) exit 1;; esac
SH
cat > "$TEST_DIR/bin/curl" <<'SH'
#!/bin/sh
while [ "$#" -gt 0 ]; do
  case "$1" in
    https://*) url=$1; shift;;
    -o) output=$2; shift 2;;
    *) shift;;
  esac
done
printf '%s\n' "$url" >> "$TEST_DIR/urls"
cp "$TEST_DIR/downloads/${url##*/}" "$output"
SH
# Simulate both checksum CLIs independently, even on hosts with only one.
cat > "$TEST_DIR/checksum" <<'SH'
#!/bin/sh
[ "${MOCK_HASH_ERROR:-0}" = 0 ] || exit 1
printf '%s  %s\n' "$HASH" fixture
SH
chmod +x "$TEST_DIR/bin/uname" "$TEST_DIR/bin/curl" "$TEST_DIR/checksum"
export TEST_DIR HASH

run_installer() {
  PATH="$TEST_DIR/bin" LOUTREVIEW_INSTALL_DIR="$TEST_DIR/installed" \
    sh "$ROOT/install.sh" > "$TEST_DIR/output" 2>&1
}
expect_failure() {
  rm -rf "$TEST_DIR/installed"
  if run_installer; then
    printf 'Expected installer failure: %s\n' "$1" >&2
    exit 1
  fi
  [ ! -e "$TEST_DIR/installed/loutre-view" ]
}

for checksum in sha256sum shasum; do
  rm -f "$TEST_DIR/bin/sha256sum" "$TEST_DIR/bin/shasum"
  ln -s "$TEST_DIR/checksum" "$TEST_DIR/bin/$checksum"
  for pair in Darwin:arm64 Darwin:x86_64 Linux:aarch64 Linux:arm64 Linux:x86_64 Linux:amd64; do
    MOCK_OS=${pair%:*}; MOCK_ARCH=${pair#*:}
    export MOCK_OS MOCK_ARCH
    case "$MOCK_OS" in Darwin) platform=darwin;; Linux) platform=linux;; esac
    case "$MOCK_ARCH" in arm64|aarch64) arch=arm64;; *) arch=amd64;; esac
    asset="loutre-view-$platform-$arch.tar.gz"
    cp "$TEST_DIR/archive.tar.gz" "$TEST_DIR/downloads/$asset"
    printf '%s  %s\n' "$HASH" "$asset" > "$TEST_DIR/downloads/checksums.txt"
    rm -rf "$TEST_DIR/installed"
    run_installer
    cmp "$TEST_DIR/payload/loutre-view" "$TEST_DIR/installed/loutre-view"
    [ -x "$TEST_DIR/installed/loutre-view" ]
    grep -F "/releases/latest/download/$asset" "$TEST_DIR/urls" >/dev/null
  done
done

LOUTREVIEW_VERSION=v1.2.3
export LOUTREVIEW_VERSION
run_installer
grep -F "/releases/download/v1.2.3/$asset" "$TEST_DIR/urls" >/dev/null
# Keep the mocked checksum valid so these reach archive-content validation.
mkdir -p "$TEST_DIR/missing-binary" "$TEST_DIR/symlink-binary"
printf 'No executable in this archive\n' > "$TEST_DIR/missing-binary/README"
tar -czf "$TEST_DIR/downloads/$asset" -C "$TEST_DIR/missing-binary" README
expect_failure 'archive missing binary'
# The target exists: rejection must be due to the symlink, not a missing target.
ln -s "$TEST_DIR/payload/loutre-view" "$TEST_DIR/symlink-binary/loutre-view"
tar -czf "$TEST_DIR/downloads/$asset" -C "$TEST_DIR/symlink-binary" loutre-view
expect_failure 'archive binary is a symlink'
grep -F 'regular loutre-view binary at its root' "$TEST_DIR/output" >/dev/null
cp "$TEST_DIR/archive.tar.gz" "$TEST_DIR/downloads/$asset"
printf '%064d  %s\n' 0 "$asset" > "$TEST_DIR/downloads/checksums.txt"
expect_failure 'checksum mismatch'
printf '%s  unrelated.tar.gz\n' "$HASH" > "$TEST_DIR/downloads/checksums.txt"
expect_failure 'missing checksum entry'
rm "$TEST_DIR/downloads/checksums.txt"
expect_failure 'missing checksum download'
printf '%s  %s\n' "$HASH" "$asset" > "$TEST_DIR/downloads/checksums.txt"
MOCK_HASH_ERROR=1
export MOCK_HASH_ERROR
expect_failure 'checksum tool failure'
unset MOCK_HASH_ERROR
rm -f "$TEST_DIR/bin/sha256sum" "$TEST_DIR/bin/shasum"
expect_failure 'missing checksum tools'
ln -s "$TEST_DIR/checksum" "$TEST_DIR/bin/sha256sum"
MOCK_OS=FreeBSD
expect_failure 'unsupported OS'
MOCK_OS=Linux MOCK_ARCH=riscv64
expect_failure 'unsupported architecture'
PATH=$REAL_PATH
printf 'Installer fixtures passed\n'
