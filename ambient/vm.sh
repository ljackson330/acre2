#!/usr/bin/env bash
# Drive the Windows development VM from the host shell.
#
# Why this exists: the ambient feature's Windows backend uses WASAPI process
# loopback (AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK), which Proton does not
# implement and GitHub's Windows runners cannot exercise -- they have no audio
# endpoint, so there is nothing for a render process to render into and nothing
# for a loopback capture to capture. A local VM with an emulated intel-hda device
# has both, which makes it the only place the capture path can be tested without
# a physical Windows machine.
#
# The VM does not need Arma. What is unverified is the process-loopback API
# contract -- activation, the negotiated format, PID scoping, and the resampler.
# All of that is provable against any process that renders audio, so a tone
# generator stands in for the game. Whether Arma's own render stream captures
# cleanly is the part that still needs a real Windows box.
#
# Setup is handled by quickemu; see ambient/README.md. The guest runs OpenSSH,
# provisioned by an autounattend.xml first-logon command, so every step below is
# scriptable rather than a click in a VM window.
set -euo pipefail

VM_DIR="${ACRE2_VM_DIR:-$HOME/VMs}"
VM_CONF="$VM_DIR/windows-11.conf"
VM_ISO="$VM_DIR/windows-11/windows-11.iso"
SSH_KEY="$HOME/.ssh/acre2-vm"
SSH_PORT=2222
SSH_USER="Quickemu"

REPO="$(cd "$(dirname "$0")/.." && pwd)"

ssh_args=(
    -p "$SSH_PORT"
    -i "$SSH_KEY"
    -o StrictHostKeyChecking=no
    -o UserKnownHostsFile=/dev/null
    -o LogLevel=ERROR
    -o ConnectTimeout=5
)

die() { echo "error: $*" >&2; exit 1; }

# Ask qemu what it can actually do rather than assuming.
#
# quickemu defaults to sdl, and a qemu display backend can be installed but
# unloadable -- on this machine ui-sdl.so links against libjxl 0.12 while the
# system has 0.11, so `-display sdl` fails at startup and the VM silently never
# boots. The failure surfaces only in windows-11.log, which is easy to miss.
pick_display() {
    if [[ -n "${ACRE2_VM_DISPLAY:-}" ]]; then
        echo "$ACRE2_VM_DISPLAY"
        return
    fi
    local available
    available="$(qemu-system-x86_64 -display help 2>/dev/null)"
    for candidate in gtk sdl spice-app; do
        if grep -qx "$candidate" <<<"$available"; then
            echo "$candidate"
            return
        fi
    done
    echo "none"
}

require_iso() {
    [[ -f "$VM_ISO" ]] || die "Windows ISO missing at $VM_ISO
Microsoft blocks quickget's automated download by IP, so fetch it manually:
  https://www.microsoft.com/en-us/software-download/windows11
  -> 'Download Windows 11 Disk Image (ISO) for x64 devices'
Save it to exactly that path, then re-run this."
}

case "${1:-}" in
up)
    require_iso
    command -v quickemu >/dev/null || die "quickemu not installed"
    echo ">> starting VM (SSH will be on localhost:$SSH_PORT once provisioned)"
    echo "   display: $(pick_display)"
    cd "$VM_DIR" && exec quickemu --vm "$VM_CONF" --display "$(pick_display)"
    ;;

headless)
    require_iso
    echo ">> starting VM headless"
    cd "$VM_DIR" && exec quickemu --vm "$VM_CONF" --display none
    ;;

status)
    # Ask something every shell understands first. If setup-ssh.ps1 installed
    # OpenSSH but failed to set DefaultShell, the session lands in cmd, and a
    # PowerShell-only probe would report that as "unreachable" rather than as
    # the half-provisioned guest it actually is.
    if ssh "${ssh_args[@]}" "$SSH_USER@localhost" "hostname" 2>/dev/null; then
        echo "ok: guest reachable over SSH"
        if ssh "${ssh_args[@]}" "$SSH_USER@localhost" \
               '$PSVersionTable.PSVersion.ToString()' 2>/dev/null; then
            echo "ok: login shell is PowerShell"
        else
            echo "warning: login shell is not PowerShell -- setup-ssh.ps1 only"
            echo "  partly succeeded. Check C:\\setup-ssh.log in the guest."
        fi
    else
        echo "guest not reachable on localhost:$SSH_PORT"
        echo "  - is the VM running?  ambient/vm.sh up"
        echo "  - first boot installs OpenSSH via Windows Update; give it a few minutes"
        echo "  - check C:\\setup-ssh.log inside the guest if it never comes up"
        exit 1
    fi
    ;;

ssh)
    shift
    exec ssh "${ssh_args[@]}" "$SSH_USER@localhost" "$@"
    ;;

install-vs)
    # VCTools is the compiler workload only -- no IDE. Roughly a 5 GB install.
    echo ">> installing Visual Studio Build Tools (this takes a while)"
    ssh "${ssh_args[@]}" "$SSH_USER@localhost" \
        'winget install --id Microsoft.VisualStudio.2022.BuildTools --accept-package-agreements --accept-source-agreements --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"'
    ;;

sync)
    # Only the ambient sources and their immediate dependencies. The guest does
    # not need the Arma-side addons or the rest of the fork.
    echo ">> syncing sources to guest"
    ssh "${ssh_args[@]}" "$SSH_USER@localhost" \
        'New-Item -ItemType Directory -Force -Path C:\acre2\src | Out-Null'
    # scp rather than a tar pipe: the guest's login shell is PowerShell, which
    # treats stdin as text and corrupts binary passed through its pipeline.
    scp -P "$SSH_PORT" -i "$SSH_KEY" \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR -r \
        "$REPO/extensions/src/ACRE2Core" "$REPO/extensions/src/ACRE2Shared" \
        "$SSH_USER@localhost:C:/acre2/src/"
    echo "   synced to C:\\acre2\\src"
    ;;

*)
    cat <<EOF
usage: ambient/vm.sh <command>

  up          launch the VM with a window (first run installs Windows)
  headless    launch with no display, SSH only
  status      check whether the guest is reachable over SSH
  ssh [cmd]   open a shell in the guest, or run one command
  install-vs  install MSVC build tools in the guest
  sync        copy the ambient sources into the guest

VM lives in $VM_DIR (outside the repo -- the ISOs are several GB).
EOF
    [[ -n "${1:-}" ]] && exit 1 || exit 0
    ;;
esac
