#!/bin/bash
# Record a nash demo GIF for the README
#
# Two approaches:
#   A) VHS (scripted, reproducible) - recommended
#   B) asciinema + agg (manual recording, already installed)
#
# ============================================================
# APPROACH A: VHS (recommended for reproducible results)
# ============================================================
#
#   sudo dnf install vhs
#   cd demo/
#   # Set up the demo workspace first:
#   bash record.sh setup
#   # Then record:
#   vhs nash-demo.tape
#   # Output: demo/nash-demo.gif
#
# ============================================================
# APPROACH B: asciinema + agg (manual, what's already installed)
# ============================================================
#
#   # Install agg (asciinema GIF generator):
#   cargo install --git https://github.com/asciinema/agg
#
#   # Set up demo workspace:
#   bash record.sh setup
#
#   # Start recording (do the demo manually):
#   bash record.sh asciinema
#
#   # Convert to GIF:
#   bash record.sh gif
#
# ============================================================

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEMO_WS="/tmp/nash-demo-workspace"
CAST_FILE="$SCRIPT_DIR/nash-demo.cast"
GIF_FILE="$SCRIPT_DIR/nash-demo.gif"

setup() {
    echo "Setting up demo workspace at $DEMO_WS ..."
    mkdir -p "$DEMO_WS"
    cat > "$DEMO_WS/greet.sh" << 'SCRIPT'
#!/bin/bash
echo "Hello, world"
SCRIPT
    chmod +x "$DEMO_WS/greet.sh"
    echo "Done. Workspace ready at $DEMO_WS"
    echo "  greet.sh contains: $(cat "$DEMO_WS/greet.sh" | tail -1)"
}

record_asciinema() {
    echo "Recording with asciinema..."
    echo "Terminal size: $(tput cols)x$(tput lines)"
    echo ""
    echo "TIPS for a good recording:"
    echo "  1. Resize terminal to 120x36 before starting"
    echo "  2. cd $DEMO_WS"
    echo "  3. Run: nash"
    echo "  4. Type: add a --name flag to greet.sh so it says Hello, <name>"
    echo "  5. Wait for completion (status bar turns green)"
    echo "  6. Scroll up briefly to show the session"
    echo "  7. Press Ctrl-D or type /quit to exit"
    echo ""
    echo "Press Enter to start recording..."
    read -r
    asciinema rec \
        --cols 120 \
        --rows 36 \
        --title "nash - Autonomous Coding Agent" \
        "$CAST_FILE"
    echo "Recording saved to $CAST_FILE"
}

make_gif() {
    if [ ! -f "$CAST_FILE" ]; then
        echo "Error: $CAST_FILE not found. Run 'record.sh asciinema' first."
        exit 1
    fi
    echo "Converting $CAST_FILE to GIF..."
    agg \
        --cols 120 \
        --rows 36 \
        --font-family "Adwaita Mono,monospace" \
        --font-size 14 \
        --speed 1.5 \
        --idle-time-limit 3 \
        --theme mocha \
        "$CAST_FILE" \
        "$GIF_FILE"
    echo "GIF saved to $GIF_FILE ($(du -h "$GIF_FILE" | cut -f1))"
}

optimize_gif() {
    if [ ! -f "$GIF_FILE" ]; then
        echo "Error: $GIF_FILE not found."
        exit 1
    fi
    # Optimize GIF size with gifsicle if available
    if command -v gifsicle &>/dev/null; then
        echo "Optimizing with gifsicle..."
        gifsicle -O3 --lossy=80 -o "${GIF_FILE%.gif}-opt.gif" "$GIF_FILE"
        echo "Optimized: $(du -h "${GIF_FILE%.gif}-opt.gif" | cut -f1) (was $(du -h "$GIF_FILE" | cut -f1))"
    else
        echo "gifsicle not found. Install with: dnf install gifsicle"
        echo "Skipping optimization."
    fi
}

case "${1:-help}" in
    setup)      setup ;;
    asciinema)  record_asciinema ;;
    gif)        make_gif ;;
    optimize)   optimize_gif ;;
    all)        setup; record_asciinema; make_gif; optimize_gif ;;
    *)
        echo "Usage: $0 {setup|asciinema|gif|optimize|all}"
        echo ""
        echo "  setup      - Create demo workspace with sample files"
        echo "  asciinema  - Record terminal session with asciinema"
        echo "  gif        - Convert .cast to .gif with agg"
        echo "  optimize   - Optimize GIF size with gifsicle"
        echo "  all        - Run all steps in sequence"
        ;;
esac
