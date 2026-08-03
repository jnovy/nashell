# Nash Demo Recording

Record a short GIF of the nash TUI for the project README.

## Quick Start (VHS - recommended)

```bash
sudo dnf install vhs
bash record.sh setup
vhs nash-demo.tape
```

Output: `nash-demo.gif`

## Alternative (asciinema + agg)

```bash
cargo install --git https://github.com/asciinema/agg
bash record.sh setup
bash record.sh asciinema   # manual recording
bash record.sh gif          # convert to GIF
bash record.sh optimize     # optional: shrink with gifsicle
```

## What the demo shows

1. Typing a prompt ("add a --name flag to greet.sh")
2. Status bar transitions: READY (grey) -> RUNNING (yellow) -> DONE (green)
3. Live token streaming with spinner animation
4. Tool calls: file_read, file_edit, shell_exec with elapsed timers
5. Diff rendering with green/red character-level highlights
6. Context % updating in the status bar

## Terminal settings

- Size: 120x36 (set before recording)
- Font: JetBrains Mono 14pt (or any monospace)
- Theme: dark background (nash uses its own Catppuccin Mocha palette)

## README integration

Once `nash-demo.gif` is generated, add to the main README after line 6
(after the intro paragraph, before the architecture diagram):

```markdown
![nash TUI demo](demo/nash-demo.gif)
```

## Tips for a good recording

- Use a fast local model so the demo completes in under 30 seconds
- The task should be tiny (2-line script) so tool calls are quick
- Pause 1-2 seconds after completion to show the green DONE status
- Keep total GIF under 5MB for GitHub rendering (use `record.sh optimize`)
