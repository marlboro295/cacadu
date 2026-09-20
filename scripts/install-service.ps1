[CmdletBinding()]
param(
    [string]$BinaryPath,

    [switch]$Uninstall
)

$principal = New-Object Security.Principal.WindowsPrincipal(
    [Security.Principal.WindowsIdentity]::GetCurrent()
)
if (!$principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Run this script from an elevated PowerShell session."
}

$serviceName = "TrayAppService"

function Get-ExistingService {
    try {
        return Get-Service -Name $serviceName -ErrorAction Stop
    }
    catch {
        if ($_.CategoryInfo.Category -eq
            [System.Management.Automation.ErrorCategory]::ObjectNotFound) {
            return $null
        }
        throw
    }
}

function Remove-ServiceRegistration {
    param([System.ServiceProcess.ServiceController]$Service)

    if ($null -eq $Service) { return }
    if ($Service.Status -ne [System.ServiceProcess.ServiceControllerStatus]::Stopped) {
        throw "${serviceName} is $($Service.Status). SCM STOP is intentionally disabled; stop it through TrayApp/RPC first."
    }

    & sc.exe delete $serviceName
    if ($LASTEXITCODE -ne 0) {
        throw "sc.exe delete failed with exit code $LASTEXITCODE."
    }

    for ($i = 0; $i -lt 50; $i++) {
        $remaining = Get-ExistingService
        if ($null -eq $remaining) { return }
        Start-Sleep -Milliseconds 200
    }
    throw "${serviceName} is marked for deletion or still has an open handle."
}

$existing = Get-ExistingService

if ($Uninstall) {
    Remove-ServiceRegistration $existing
    exit 0
}

$servicePath = $null
if ([string]::IsNullOrWhiteSpace($BinaryPath)) {
    throw "BinaryPath is required when installing TrayAppService."
}

$servicePath = [IO.Path]::GetFullPath($BinaryPath)
if (![IO.Path]::IsPathRooted($servicePath)) {
    throw "BinaryPath must resolve to an absolute path."
}
if (!(Test-Path -LiteralPath $servicePath -PathType Leaf)) {
    throw "TrayService.exe was not found: $servicePath"
}

$guiPath = Join-Path ([IO.Path]::GetDirectoryName($servicePath)) "TrayApp.exe"
if (!(Test-Path -LiteralPath $guiPath -PathType Leaf)) {
    throw "TrayApp.exe must be next to TrayService.exe: $guiPath"
}

if ($null -ne $existing) {
    Remove-ServiceRegistration $existing
}

$serviceParameters = @{
    Name = $serviceName
    BinaryPathName = ('"{0}"' -f $servicePath)
    DisplayName = "TrayApp Service"
    Description = "TrayApp per-session GUI service"
    StartupType = "Manual"
}
New-Service @serviceParameters

$serviceSddl = 'D:(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)(A;;LCRP;;;IU)'
& sc.exe sdset $serviceName $serviceSddl
if ($LASTEXITCODE -ne 0) {
    throw "sc.exe sdset failed with exit code $LASTEXITCODE."
}
