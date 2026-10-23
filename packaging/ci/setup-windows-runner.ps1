# The Windows build machine for .github/workflows/release.yml: the tools the
# windows-package job needs, and a GitHub Actions runner as a service (label
# lantalk-windows). Adapted from allglider's; it can share a PC with
# allglider's runner, as a second service in its own folder. Run in an
# elevated PowerShell on the machine:
#
#   Set-ExecutionPolicy -Scope Process Bypass
#   .\setup-windows-runner.ps1 -Token <registration token>
#
# The token is the repository's runner registration token (valid for an hour):
#   gh api -X POST repos/ablohme86/lantalk/actions/runners/registration-token -q .token
# Without -Token only the tools are installed; run it again with one to add
# the runner. Run again any time: what is there already is left as it is.
param(
    [string]$Token = "",
    [string]$Repo = "ablohme86/lantalk",
    [string]$RunnerDir = "C:\actions-runner-lantalk",
    [string]$Name = "$env:COMPUTERNAME-lantalk",
    [string]$Labels = "lantalk-windows"
)

$ErrorActionPreference = "Stop"

function Step($text) { Write-Host ""; Write-Host "==> $text" -ForegroundColor Cyan }

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw "Run this in an elevated PowerShell (Run as administrator)." }

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw "winget is missing: install 'App Installer' from the Microsoft Store, then run this again."
}

# winget's own id, and any extra arguments for the installer.
function Install($id, $extra = @()) {
    $listed = winget list --id $id --exact --accept-source-agreements 2>$null | Out-String
    if ($listed -match [regex]::Escape($id)) {
        Write-Host "    $id is installed"
        return
    }
    Write-Host "    installing $id"
    winget install --id $id --exact --silent --accept-package-agreements --accept-source-agreements @extra
    # 3010: installed, a restart pending. -1978335189: already installed.
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 3010 -and $LASTEXITCODE -ne -1978335189) {
        throw "winget could not install $id (exit $LASTEXITCODE)"
    }
}

Step "Visual Studio 2022 Build Tools: MSVC and the Windows SDK"
# The workflow configures CMake with the "Visual Studio 17 2022" generator.
Install "Microsoft.VisualStudio.2022.BuildTools" @("--override", "--wait --quiet --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621 --add Microsoft.VisualStudio.Component.VC.CMake.Project --includeRecommended")

Step "Git, CMake, GitHub CLI, Python, .NET SDK"
# For the whole machine: the runner's service has an account of its own, and
# sees nothing installed for one user only.
Install "Git.Git"
Install "Kitware.CMake" @("--scope", "machine")
Install "GitHub.cli"
# install-qt-action installs Qt with aqtinstall, a Python tool.
Install "Python.Python.3.12" @("--scope", "machine")
# The MSI is built with WiX, a .NET tool.
Install "Microsoft.DotNet.SDK.8"

$ProgressPreference = "SilentlyContinue"

Step "PowerShell 7, for the whole machine (winget has only a per-user one)"
if (-not (Test-Path "C:\Program Files\PowerShell\7\pwsh.exe")) {
    $release = Invoke-RestMethod "https://api.github.com/repos/PowerShell/PowerShell/releases/latest"
    $msi = $release.assets | Where-Object { $_.name -match "win-x64\.msi$" } | Select-Object -First 1
    Invoke-WebRequest $msi.browser_download_url -OutFile "$env:TEMP\pwsh.msi"
    $p = Start-Process msiexec.exe -Wait -PassThru -ArgumentList "/i `"$env:TEMP\pwsh.msi`" /qn /norestart ADD_PATH=1"
    if ($p.ExitCode -ne 0) { throw "PowerShell 7 could not be installed (msiexec exit $($p.ExitCode))" }
} else {
    Write-Host "    PowerShell 7 is installed"
}

Step "Scripts allowed (Windows 11 Home forbids them; GitHub's own runners allow them)"
# install-qt-action runs PowerShell scripts of its own.
# RemoteSigned: local scripts run, downloaded ones must be signed.
# PowerShell reports an error when the session's own -Scope Process Bypass
# (from the instructions above) outranks the new setting; it is saved anyway.
try {
    Set-ExecutionPolicy -Scope LocalMachine RemoteSigned -Force
} catch {
    if ($_.FullyQualifiedErrorId -notlike "ExecutionPolicyOverride*") { throw }
}
if (Test-Path "C:\Program Files\PowerShell\7\pwsh.exe") {
    & "C:\Program Files\PowerShell\7\pwsh.exe" -NoProfile -Command "try { Set-ExecutionPolicy -Scope LocalMachine RemoteSigned -Force } catch { if (`$_.FullyQualifiedErrorId -notlike 'ExecutionPolicyOverride*') { throw } }"
}

Step "Long paths, for the build tree's deep folders"
New-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem" -Name LongPathsEnabled -Value 1 -PropertyType DWORD -Force | Out-Null

# The tools just installed, in this session's PATH too.
$env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User")
git config --system core.longpaths true

# bash for the workflow's steps (install-qt-action's own): Git's, on the
# machine's PATH, which the runner's service reads when it starts.
$gitBin = "C:\Program Files\Git\bin"
$machinePath = [Environment]::GetEnvironmentVariable("Path", "Machine")
if (-not ($machinePath.Split(";") -contains $gitBin)) {
    [Environment]::SetEnvironmentVariable("Path", "$machinePath;$gitBin", "Machine")
    $env:Path += ";$gitBin"
}

Step "What is there"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
Write-Host "    Visual Studio: $vs"
foreach ($tool in "git", "bash", "cmake", "gh", "pwsh", "python", "dotnet") {
    $found = Get-Command $tool -ErrorAction SilentlyContinue
    Write-Host ("    {0}: {1}" -f $tool, $(if ($found) { $found.Source } else { "MISSING - open a new PowerShell and run this again" }))
}

if (-not $Token) {
    Write-Host ""
    Write-Host "The tools are in. Run this again with -Token <registration token> for the runner." -ForegroundColor Yellow
    exit 0
}

Step "The GitHub Actions runner in $RunnerDir"
New-Item -ItemType Directory -Force -Path $RunnerDir | Out-Null
Set-Location $RunnerDir

if (-not (Test-Path "$RunnerDir\config.cmd")) {
    $release = Invoke-RestMethod "https://api.github.com/repos/actions/runner/releases/latest"
    $version = $release.tag_name.TrimStart("v")
    $zip = "actions-runner-win-x64-$version.zip"
    Write-Host "    runner $version"
    Invoke-WebRequest "https://github.com/actions/runner/releases/download/v$version/$zip" -OutFile $zip
    Expand-Archive $zip -DestinationPath $RunnerDir -Force
    Remove-Item $zip
}

# The builds are many small files: Defender scanning each slows them a lot.
try { Add-MpPreference -ExclusionPath $RunnerDir } catch { Write-Warning "could not exclude $RunnerDir from Defender: $_" }

# As a service, started with Windows; --replace takes over a runner of the
# same name registered before. As SYSTEM: under NETWORK SERVICE (the default)
# the service would not start on Windows 11 Home (error 1068).
.\config.cmd --unattended --url "https://github.com/$Repo" --token $Token --name $Name --labels $Labels --work _work --runasservice --windowslogonaccount "NT AUTHORITY\SYSTEM" --replace
if ($LASTEXITCODE -ne 0) { throw "the runner could not be registered (exit $LASTEXITCODE) - is the token an hour old?" }

# Started again, so it reads the PATH as it is now. Only this runner's
# service: allglider's runner on the same machine is left alone.
$service = Get-Content "$RunnerDir\.service" -ErrorAction SilentlyContinue
if ($service) { Restart-Service $service; Get-Service $service | Format-Table Name, Status -AutoSize }
Write-Host ""
Write-Host "Done: the runner '$Name' ($Labels) is waiting for jobs." -ForegroundColor Green
Write-Host "A restart is worth it if Visual Studio asked for one." -ForegroundColor Green
