#!/bin/sh
# Exercise the update command without network access or changing the real install.
set -eu

BINARY=${1:?usage: test_update.sh BINARY}
TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT INT TERM
INSTALL_DIR="$TEST_DIR/install"
MOCK_BIN="$TEST_DIR/mock-bin"
mkdir -p "$INSTALL_DIR" "$MOCK_BIN"
cp "$BINARY" "$INSTALL_DIR/loutre-view"

cat > "$TEST_DIR/installer.sh" <<'SH'
#!/bin/sh
set -eu
cat > "$LOUTREVIEW_INSTALL_DIR/loutre-view" <<'BIN'
#!/bin/sh
printf 'updated fixture\n'
BIN
chmod 755 "$LOUTREVIEW_INSTALL_DIR/loutre-view"
SH

cat > "$MOCK_BIN/curl" <<'SH'
#!/bin/sh
set -eu
while [ "$#" -gt 0 ]; do
    case "$1" in
        --output) output=$2; shift 2 ;;
        *) shift ;;
    esac
done
[ -n "${output:-}" ]
cp "$LOUTREVIEW_TEST_INSTALLER" "$output"
SH
chmod 755 "$MOCK_BIN/curl"

PATH="$INSTALL_DIR:$MOCK_BIN:$PATH" \
    LOUTREVIEW_TEST_INSTALLER="$TEST_DIR/installer.sh" \
    loutre-view update
[ "$("$INSTALL_DIR/loutre-view")" = "updated fixture" ]
printf 'Update command fixture passed\n'
