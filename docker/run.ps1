param(
    [ValidateSet("image", "build", "test", "shell", "clean", "help")]
    [string]$Command = "help"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Image = "char-driver-lab"
Set-Location $Root

if ($Command -eq "image") {
    docker build -t $Image -f docker/Dockerfile .
    exit $LASTEXITCODE
}

$exists = docker image inspect $Image 2>$null
if ($LASTEXITCODE -ne 0) {
    docker build -t $Image -f docker/Dockerfile .
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

New-Item -ItemType Directory -Force -Path "docker/output" | Out-Null
$Output = (Resolve-Path "docker/output").Path

if ($Command -eq "shell") {
    docker run --rm -it -v "${Output}:/workspace/docker/output" $Image shell
} else {
    docker run --rm -v "${Output}:/workspace/docker/output" $Image $Command
}
exit $LASTEXITCODE
