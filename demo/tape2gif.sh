#!/bin/bash
# Convert nash-demo.tape to GIF using VHS.
#
# Usage:
#   cd demo/
#   bash tape2gif.sh              # setup workspace + record
#   bash tape2gif.sh --no-setup   # skip workspace setup (re-record only)
#
# Prerequisites:
#   sudo dnf install vhs          # VHS must be installed
#   nash must be built and in PATH

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TAPE_FILE="$SCRIPT_DIR/nash-demo.tape"
DEMO_WS="/tmp/nash-demo-workspace"
SKIP_SETUP=0

for arg in "$@"; do
    case "$arg" in
        --no-setup) SKIP_SETUP=1 ;;
        -h|--help)
            echo "Usage: $0 [--no-setup]"
            echo ""
            echo "Converts nash-demo.tape into nash-demo.gif via VHS."
            echo ""
            echo "Options:"
            echo "  --no-setup   Skip workspace reset (use existing greet.sh)"
            echo "  -h, --help   Show this help"
            exit 0
            ;;
        *)
            echo "Unknown option: $arg" >&2
            exit 1
            ;;
    esac
done

# --- Preflight checks ---

if ! command -v vhs &>/dev/null; then
    echo "Error: vhs not found. Install with: sudo dnf install vhs" >&2
    exit 1
fi

if ! command -v nash &>/dev/null; then
    echo "Error: nash not found. Build it and add to PATH first." >&2
    exit 1
fi

if [ ! -f "$TAPE_FILE" ]; then
    echo "Error: $TAPE_FILE not found." >&2
    exit 1
fi

# --- Setup demo workspace ---

if [ "$SKIP_SETUP" -eq 0 ]; then
    echo "Setting up demo workspace at $DEMO_WS ..."
    mkdir -p "$DEMO_WS"
    cat > "$DEMO_WS/greet.sh" << 'SCRIPT'
#!/bin/bash
echo "Hello, world"
SCRIPT
    chmod +x "$DEMO_WS/greet.sh"
    echo "  greet.sh reset to: $(tail -1 "$DEMO_WS/greet.sh")"
else
    echo "Skipping workspace setup (--no-setup)"
    if [ ! -f "$DEMO_WS/greet.sh" ]; then
        echo "Warning: $DEMO_WS/greet.sh does not exist. Run without --no-setup first." >&2
    fi
fi

# --- Record ---

echo ""
echo "Recording from $TAPE_FILE ..."
echo "This takes ~2-3 minutes (VHS renders every frame)."
echo ""

cd "$SCRIPT_DIR"
vhs "$TAPE_FILE"

# --- Report results ---

echo ""
echo "--- Output ---"
for ext in gif; do
    f="$SCRIPT_DIR/nash-demo.$ext"
    if [ -f "$f" ]; then
        size=$(du -h "$f" | cut -f1)
        echo "  $f  ($size)"
    fi
done

# --- Optional optimization ---

GIF_FILE="$SCRIPT_DIR/nash-demo.gif"
if [ -f "$GIF_FILE" ] && command -v gifsicle &>/dev/null; then
    OPT_FILE="${GIF_FILE%.gif}-opt.gif"
    orig=$(du -h "$GIF_FILE" | cut -f1)
    echo ""
    echo "Optimizing GIF with gifsicle ..."
    gifsicle -O3 --lossy=200 --colors 16 -o "$OPT_FILE" "$GIF_FILE"
    opt=$(du -h "$OPT_FILE" | cut -f1)
    mv "$OPT_FILE" "$GIF_FILE"
    echo "  $orig -> $opt (optimized in place)"
fi

echo ""
echo "Done. Add to README with:  ![nash TUI demo](demo/nash-demo.gif)"
