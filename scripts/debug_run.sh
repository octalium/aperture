#!/usr/bin/env bash
# Launch aperture under gdb in batch mode with auto-backtrace on crash.
# Output (gdb chatter + app stdio + crash bt) goes to $1, default
# /tmp/ap_gdb.log. Validation layers are on so spec violations show up
# in the log -- VK_LAYER_KHRONOS_validation must be installed.
set -u
log=${1:-/tmp/ap_gdb.log}

# gdb spawns a fork/exec child; env vars set on the gdb process are
# inherited by the debuggee.
export VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation
# Drop the "GPU hung" warn-and-keep-going so a hang lands in the
# fault handler (which we'll add separately if needed). For now,
# validation should fire BEFORE the hang and the layer error goes to
# the log.

exec gdb -batch \
    -ex 'set pagination off' \
    -ex 'set print frame-arguments all' \
    -ex 'handle SIGPIPE nostop noprint pass' \
    -ex 'run' \
    -ex 'echo --- CRASH backtrace ---\n' \
    -ex 'bt full' \
    -ex 'echo --- All threads ---\n' \
    -ex 'thread apply all bt' \
    --args ./build/aperture >"$log" 2>&1
