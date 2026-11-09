# sourced by the scripts from the repository root: loads local.sh when it exists (git ignores it; it may set
# VKDEFLATE_CMAKE_ARGS and VKDEFLATE_BUSY), then refuses to start while the program named in VKDEFLATE_BUSY runs
if [ -f local.sh ]; then
    . ./local.sh
fi
if [ -n "$VKDEFLATE_BUSY" ] && pgrep -x "$VKDEFLATE_BUSY" >/dev/null 2>&1; then
    echo "$VKDEFLATE_BUSY is running, not starting"
    exit 1
fi
