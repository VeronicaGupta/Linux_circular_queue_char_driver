param(
    [ValidateSet("build", "test", "shell", "clean", "image")]
    [string]$Command = "test"
)

$ErrorActionPreference = "Stop"
$ImageName = "linux-character-driver-lab:ubuntu24"
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Dockerfile = Join-Path $PSScriptRoot "Dockerfile"

function Invoke-DockerChecked {
    param([string[]]$Arguments)

    & docker @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Docker command failed with exit code $LASTEXITCODE"
    }
}

if (-not (Get-Command docker -ErrorAction SilentlyContinue)) {
    Write-Error "docker.exe is not on PATH. Install/start Docker Desktop first."
    exit 1
}

& docker info *> $null
if ($LASTEXITCODE -ne 0) {
    Write-Error "Docker Desktop is installed but the Linux Docker engine is not running."
    exit 1
}

if ($Command -eq "clean") {
    $OutputDirectory = Join-Path $PSScriptRoot "output"
    if (Test-Path $OutputDirectory) {
        Get-ChildItem $OutputDirectory -Force |
            Where-Object { $_.Name -ne ".gitkeep" } |
            Remove-Item -Recurse -Force
    }
    Write-Host "Local Docker/QEMU output cleaned."
    exit 0
}

Write-Host "Building Docker image $ImageName"
Invoke-DockerChecked @(
    "build",
    "--platform", "linux/amd64",
    "-t", $ImageName,
    "-f", $Dockerfile,
    $ProjectRoot
)

if ($Command -eq "image") {
    Write-Host "Docker image ready: $ImageName"
    exit 0
}

$MountSpec = "type=bind,source=$ProjectRoot,target=/src"
$RunArguments = @(
    "run", "--rm",
    "--platform", "linux/amd64",
    "--mount", $MountSpec,
    "-w", "/src"
)

if ($Command -eq "shell") {
    $RunArguments += @("-it")
}

$RunArguments += @($ImageName, "bash", "/src/docker/lab.sh", $Command)
Invoke-DockerChecked $RunArguments
