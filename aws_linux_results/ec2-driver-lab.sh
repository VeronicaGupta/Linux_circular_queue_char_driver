#!/usr/bin/env bash

# AWS EC2 Linux Character Driver Lab bootstrap.
#
# Supported host:
#   Amazon Linux 2023 on an EC2 x86_64 instance.
#
# Main workflow:
#   1. Install Git, Docker, compiler utilities, and OpenSSH client tools.
#   2. Start and enable Docker.
#   3. Create a dedicated GitHub SSH key for later Git push access.
#   4. Clone or update the public character-driver repository over HTTPS.
#   5. Build the Docker image containing the Linux/QEMU driver lab.
#   6. Boot the QEMU guest and run all automated driver tests.
#   7. Require the documented PASS markers before setup succeeds.
#
# Commands:
#   ./ec2-driver-lab.sh setup
#   ./ec2-driver-lab.sh test
#   ./ec2-driver-lab.sh status
#   ./ec2-driver-lab.sh show-key
#   ./ec2-driver-lab.sh git-ssh
#   ./ec2-driver-lab.sh reset
#   ./ec2-driver-lab.sh reset-all --yes

set -Eeuo pipefail

# -------------------------------
# User-configurable defaults
# -------------------------------
REPO_URL_HTTPS="${REPO_URL_HTTPS:-https://github.com/VeronicaGupta/Linux_circular_queue_char_driver.git}"
REPO_URL_SSH="${REPO_URL_SSH:-git@github.com:VeronicaGupta/Linux_circular_queue_char_driver.git}"
REPO_DIR="${REPO_DIR:-$HOME/Linux_circular_queue_char_driver}"
DOCKER_IMAGE="${DOCKER_IMAGE:-char-driver-lab}"
SSH_KEY="${SSH_KEY:-$HOME/.ssh/id_ed25519_github_char_driver_lab}"
GIT_NAME="${GIT_NAME:-}"
GIT_EMAIL="${GIT_EMAIL:-}"

COMMAND="${1:-help}"
SECOND_ARG="${2:-}"

# Use sudo for privileged host operations when the script is not already root.
if [[ "${EUID}" -eq 0 ]]; then
    SUDO=""
else
    SUDO="sudo"
fi

# -------------------------------
# Logging helpers
# -------------------------------
log() {
    printf '\n[EC2-DRIVER-LAB] %s\n' "$*"
}

warn() {
    printf '\n[EC2-DRIVER-LAB][WARN] %s\n' "$*" >&2
}

fail() {
    printf '\n[EC2-DRIVER-LAB][ERROR] %s\n' "$*" >&2
    exit 1
}

# Print the line number if any command exits unexpectedly.
trap 'printf "\n[EC2-DRIVER-LAB][ERROR] Command failed at line %s.\n" "$LINENO" >&2' ERR

# -------------------------------
# Host validation
# -------------------------------
check_amazon_linux() {
    # Read the operating-system identity supplied by systemd-compatible Linux distributions.
    [[ -r /etc/os-release ]] || fail "/etc/os-release is unavailable."

    # shellcheck disable=SC1091
    source /etc/os-release

    # The package installation below is written for Amazon Linux 2023.
    if [[ "${ID:-}" != "amzn" || "${VERSION_ID:-}" != "2023" ]]; then
        fail "Amazon Linux 2023 is required. Detected: ${PRETTY_NAME:-unknown}."
    fi

    # The Docker/QEMU image in this repository is intended for x86_64 hosts.
    if [[ "$(uname -m)" != "x86_64" ]]; then
        fail "x86_64 EC2 is required by this lab. Detected: $(uname -m)."
    fi
}

# -------------------------------
# Package and Docker setup
# -------------------------------
install_host_packages() {
    log "Updating Amazon Linux packages"
    $SUDO dnf update -y

    log "Installing Git, Docker, compiler utilities, Make, and OpenSSH client tools"
    # Kernel development headers are not required on the EC2 host for the QEMU path.
    # The Docker image provides the guest kernel and matching build headers.
    $SUDO dnf install -y \
        git \
        docker \
        gcc \
        gcc-c++ \
        make \
        openssh-clients
}

configure_docker() {
    log "Starting and enabling Docker"
    $SUDO systemctl enable --now docker

    # Add the login user to the docker group for future sessions.
    # The current script continues to use sudo so a logout/login is not required now.
    if [[ -n "${SUDO}" && -n "${USER:-}" ]]; then
        if getent group docker >/dev/null 2>&1; then
            $SUDO usermod -aG docker "$USER" || true
        fi
    fi

    # Verify that the daemon is reachable before any image build begins.
    $SUDO docker info >/dev/null
    log "Docker daemon is running"
}

# -------------------------------
# Git and SSH setup
# -------------------------------
configure_git_identity() {
    # Git identity is optional for cloning, but required for commits.
    # Values can be supplied non-interactively:
    #   GIT_NAME="Name" GIT_EMAIL="email@example.com" ./ec2-driver-lab.sh setup
    if [[ -n "$GIT_NAME" ]]; then
        git config --global user.name "$GIT_NAME"
    fi

    if [[ -n "$GIT_EMAIL" ]]; then
        git config --global user.email "$GIT_EMAIL"
    fi
}

create_github_ssh_key() {
    log "Preparing a dedicated GitHub SSH key"

    mkdir -p "$HOME/.ssh"
    chmod 700 "$HOME/.ssh"

    if [[ ! -f "$SSH_KEY" ]]; then
        # A dedicated passwordless key keeps the automated EC2 setup non-interactive.
        # GitHub receives only the .pub file; the private key remains on the instance.
        local key_comment
        key_comment="${GIT_EMAIL:-${USER:-ec2-user}@$(hostname)}"
        ssh-keygen -t ed25519 -f "$SSH_KEY" -C "$key_comment" -N ""
    else
        log "Existing dedicated SSH key preserved: $SSH_KEY"
    fi

    chmod 600 "$SSH_KEY"
    chmod 644 "${SSH_KEY}.pub"

    printf '\nGitHub public key (add this under GitHub -> Settings -> SSH and GPG keys):\n\n'
    cat "${SSH_KEY}.pub"
    printf '\n\nThe public key is not required for the automated test because the public repository is cloned over HTTPS.\n'
}

clone_or_update_repository() {
    log "Preparing repository at $REPO_DIR"

    if [[ ! -d "$REPO_DIR/.git" ]]; then
        git clone "$REPO_URL_HTTPS" "$REPO_DIR"
        return
    fi

    # Avoid overwriting local development work. Pull only when the checkout is clean.
    if [[ -n "$(git -C "$REPO_DIR" status --porcelain)" ]]; then
        warn "Repository contains local changes; automatic git pull skipped."
        return
    fi

    git -C "$REPO_DIR" fetch origin
    git -C "$REPO_DIR" pull --ff-only
}

show_public_key() {
    [[ -f "${SSH_KEY}.pub" ]] || fail "GitHub SSH key has not been generated. Run setup first."
    cat "${SSH_KEY}.pub"
}

switch_git_remote_to_ssh() {
    # This command is intended to run after the displayed public key has been added to GitHub.
    [[ -d "$REPO_DIR/.git" ]] || fail "Repository is unavailable. Run setup first."
    [[ -f "$SSH_KEY" ]] || fail "SSH key is unavailable. Run setup first."

    log "Testing GitHub SSH authentication using the dedicated key"

    # GitHub intentionally returns a non-zero shell status after successful authentication,
    # so authentication is recognized from its documented success message instead.
    local ssh_output
    # GitHub returns a non-zero shell status even after successful authentication
    # because interactive shell access is intentionally unavailable. Suppress the
    # generic ERR trap while capturing that expected status.
    trap - ERR
    set +e
    ssh_output="$(ssh \
        -i "$SSH_KEY" \
        -o IdentitiesOnly=yes \
        -o StrictHostKeyChecking=accept-new \
        -T git@github.com 2>&1)"
    local ssh_status=$?
    set -e
    trap 'printf "\n[EC2-DRIVER-LAB][ERROR] Command failed at line %s.\n" "$LINENO" >&2' ERR

    printf '%s\n' "$ssh_output"

    if ! grep -q "successfully authenticated" <<<"$ssh_output"; then
        fail "GitHub did not accept the SSH key. Add $(basename "${SSH_KEY}.pub") to GitHub and retry. SSH status: $ssh_status"
    fi

    git -C "$REPO_DIR" remote set-url origin "$REPO_URL_SSH"
    log "Git origin switched to SSH: $REPO_URL_SSH"
}

# -------------------------------
# Docker/QEMU build and test
# -------------------------------
build_docker_image() {
    [[ -f "$REPO_DIR/docker/Dockerfile" ]] || fail "docker/Dockerfile is missing from $REPO_DIR."

    log "Building Docker image: $DOCKER_IMAGE"
    (
        cd "$REPO_DIR"
        $SUDO docker build \
            -t "$DOCKER_IMAGE" \
            -f docker/Dockerfile \
            .
    )
}

run_qemu_tests() {
    [[ -d "$REPO_DIR" ]] || fail "Repository is unavailable. Run setup first."

    mkdir -p "$REPO_DIR/docker/output"

    local host_log
    host_log="$REPO_DIR/docker/output/ec2-qemu-test-$(date +%Y%m%d_%H%M%S).log"

    log "Running automated Docker + QEMU character-driver tests"

    # The host output directory is bind-mounted so QEMU serial logs survive container removal.
    # --rm removes the temporary Docker container after the QEMU test completes.
    # Capture QEMU output even when the test command returns non-zero, then report
    # the saved status with a precise log location instead of losing the serial log.
    trap - ERR
    set +e
    (
        cd "$REPO_DIR"
        $SUDO docker run --rm \
            -v "$REPO_DIR/docker/output:/workspace/docker/output" \
            "$DOCKER_IMAGE" test
    ) 2>&1 | tee "$host_log"
    local docker_status=${PIPESTATUS[0]}
    set -e
    trap 'printf "\n[EC2-DRIVER-LAB][ERROR] Command failed at line %s.\n" "$LINENO" >&2' ERR

    if [[ "$docker_status" -ne 0 ]]; then
        fail "Docker/QEMU test command failed with status $docker_status. Log: $host_log"
    fi

    verify_qemu_pass "$host_log"
}

verify_qemu_pass() {
    local host_log="$1"
    local guest_log="$REPO_DIR/docker/output/latest.log"

    # A generic grep for 'fail' is intentionally avoided because QEMU/ACPI and unsigned
    # out-of-tree module warnings can legitimately contain the word 'failed'.
    # Only structured test result lines and explicit final PASS markers determine success.

    if [[ -f "$guest_log" ]]; then
        if grep -Eq 'Result: [0-9]+ passed, [1-9][0-9]* failed' "$guest_log"; then
            fail "At least one guest test reported a non-zero failed count. See: $guest_log"
        fi
    fi

    if grep -Eq 'Result: [0-9]+ passed, [1-9][0-9]* failed' "$host_log"; then
        fail "At least one test reported a non-zero failed count. See: $host_log"
    fi

    # The current repository documents both markers for a complete successful run.
    local guest_pass=0
    local outer_pass=0

    if { [[ -f "$guest_log" ]] && grep -Fq "DRIVER LAB RESULT: PASS" "$guest_log"; } || \
       grep -Fq "DRIVER LAB RESULT: PASS" "$host_log"; then
        guest_pass=1
    fi

    if grep -Fq "Docker/QEMU test result: PASS" "$host_log" || \
       { [[ -f "$guest_log" ]] && grep -Fq "Docker/QEMU test result: PASS" "$guest_log"; }; then
        outer_pass=1
    fi

    if [[ "$guest_pass" -ne 1 || "$outer_pass" -ne 1 ]]; then
        fail "Expected final PASS markers were not both found. Host log: $host_log Guest log: $guest_log"
    fi

    log "QEMU TEST RESULT: PASS"
    printf 'Host test log : %s\n' "$host_log"
    printf 'Guest serial log: %s\n' "$guest_log"
}

# -------------------------------
# Status and reset
# -------------------------------
show_status() {
    printf 'Host kernel       : %s\n' "$(uname -r)"
    printf 'Architecture      : %s\n' "$(uname -m)"
    printf 'Repository        : %s\n' "$REPO_DIR"
    printf 'Docker image      : %s\n' "$DOCKER_IMAGE"
    printf 'GitHub SSH key    : %s\n' "$SSH_KEY"

    if command -v docker >/dev/null 2>&1 && $SUDO docker info >/dev/null 2>&1; then
        printf 'Docker daemon     : running\n'
    else
        printf 'Docker daemon     : unavailable\n'
    fi

    if [[ -d "$REPO_DIR/.git" ]]; then
        printf 'Repository status : present\n'
        printf 'Git remote        : %s\n' "$(git -C "$REPO_DIR" remote get-url origin 2>/dev/null || true)"
    else
        printf 'Repository status : absent\n'
    fi

    if command -v docker >/dev/null 2>&1 && $SUDO docker image inspect "$DOCKER_IMAGE" >/dev/null 2>&1; then
        printf 'Docker image      : present\n'
    else
        printf 'Docker image      : absent\n'
    fi

    if [[ -f "$REPO_DIR/docker/output/latest.log" ]]; then
        printf 'Latest QEMU log   : %s\n' "$REPO_DIR/docker/output/latest.log"
    fi
}

safe_reset() {
    log "Resetting generated Docker/QEMU state"

    # Remove any container that may still exist from this image.
    if command -v docker >/dev/null 2>&1 && $SUDO docker info >/dev/null 2>&1; then
        local container_ids
        container_ids="$($SUDO docker ps -aq --filter "ancestor=$DOCKER_IMAGE" 2>/dev/null || true)"
        if [[ -n "$container_ids" ]]; then
            # shellcheck disable=SC2086
            $SUDO docker rm -f $container_ids >/dev/null || true
        fi

        # Remove the generated Docker image but leave Docker itself installed.
        if $SUDO docker image inspect "$DOCKER_IMAGE" >/dev/null 2>&1; then
            $SUDO docker image rm -f "$DOCKER_IMAGE" >/dev/null
        fi
    fi

    # Remove generated test output while preserving source code and Git history.
    if [[ -d "$REPO_DIR/docker/output" ]]; then
        rm -rf "$REPO_DIR/docker/output"
        mkdir -p "$REPO_DIR/docker/output"
    fi

    log "Reset complete. Repository, Git configuration, SSH key, and installed packages were preserved."
}

full_reset() {
    if [[ "$SECOND_ARG" != "--yes" ]]; then
        fail "Full reset deletes the cloned repository and dedicated GitHub SSH key. Re-run: $0 reset-all --yes"
    fi

    safe_reset

    log "Removing cloned repository"
    rm -rf "$REPO_DIR"

    log "Removing dedicated GitHub SSH key"
    rm -f "$SSH_KEY" "${SSH_KEY}.pub"

    log "Full reset complete. Docker and host packages remain installed."
}

# -------------------------------
# High-level commands
# -------------------------------
setup_all() {
    check_amazon_linux
    install_host_packages
    configure_docker
    configure_git_identity
    create_github_ssh_key
    clone_or_update_repository
    build_docker_image
    run_qemu_tests

    log "SETUP COMPLETE"
    printf '\nUseful next commands:\n'
    printf '  %s status\n' "$0"
    printf '  %s show-key\n' "$0"
    printf '  %s git-ssh        # after the public key is added to GitHub\n' "$0"
    printf '  cd %s && sudo ./driver-lab.sh shell\n' "$REPO_DIR"
}

test_existing_setup() {
    check_amazon_linux
    configure_docker

    if ! $SUDO docker image inspect "$DOCKER_IMAGE" >/dev/null 2>&1; then
        build_docker_image
    fi

    run_qemu_tests
}

print_help() {
    cat <<EOF_HELP
Usage: $0 <command>

Commands:
  setup              Install host prerequisites, configure Docker/Git/SSH,
                     clone/update the repository, build the Docker image,
                     run QEMU tests, and require final PASS markers.

  test               Re-run the Docker/QEMU tests. The image is rebuilt only
                     when it is missing.

  status             Show Docker, repository, kernel, image, SSH-key, and log status.

  show-key           Print the dedicated GitHub public SSH key.

  git-ssh            Verify that GitHub accepts the dedicated SSH key and switch
                     the repository origin from HTTPS to SSH.

  reset              Remove generated Docker containers/image and QEMU logs.
                     Repository source, SSH key, Git config, and packages remain.

  reset-all --yes    Perform reset, then delete the cloned repository and the
                     dedicated GitHub SSH key. Installed host packages remain.

  help               Show this help.

Optional environment variables:
  GIT_NAME="Veronica Gupta"
  GIT_EMAIL="name@example.com"
  REPO_DIR="$HOME/Linux_circular_queue_char_driver"
  DOCKER_IMAGE="char-driver-lab"
EOF_HELP
}

case "$COMMAND" in
    setup)
        setup_all
        ;;
    test)
        test_existing_setup
        ;;
    status)
        show_status
        ;;
    show-key)
        show_public_key
        ;;
    git-ssh)
        switch_git_remote_to_ssh
        ;;
    reset)
        safe_reset
        ;;
    reset-all)
        full_reset
        ;;
    help|-h|--help)
        print_help
        ;;
    *)
        print_help >&2
        exit 2
        ;;
esac
