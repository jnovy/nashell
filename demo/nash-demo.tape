# nash TUI demo recording
# Usage: cd demo/ && vhs nash-demo.tape
#
# Prerequisites:
#   dnf install vhs
#   nash must be built and in PATH
#   bash record.sh setup    (creates /tmp/nash-demo-workspace)

# --- Output settings ---
Output nash-demo.gif

# --- Terminal settings ---
Set Width 1024
Set Height 768
Set FontFamily "Adwaita Mono"
Set FontSize 14
Set Theme "Catppuccin Mocha"
Set TypingSpeed 40ms
Set Padding 12
Set Framerate 5

# --- Start nash in the demo workspace ---
Type "cd /tmp/nash-demo-workspace && nash"
Enter
Sleep 3s

# Type the prompt
Type "edit to add a --name flag to greet.sh so it says Hello, <name> instead of Hello, world"
Sleep 500ms
Enter

# Wait for the agent to complete the task
# (status bar transitions: grey READY -> yellow RUNNING -> green DONE)
Sleep 17s

Up
Sleep 1s
Enter
Sleep 5s