# Ribbon 2.0 - Direct Windows 11 Installer (No Admin Rights Required)
$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ExeSrc = Join-Path $ScriptDir "dist\Ribbon.exe"
$IcoSrc = Join-Path $ScriptDir "ribbon.ico"

if (-not (Test-Path $ExeSrc)) {
    Write-Host "dist\Ribbon.exe not found. Please run build.bat first!" -ForegroundColor Red
    Pause
    exit 1
}

$InstallDir = Join-Path $env:LOCALAPPDATA "Programs\Ribbon Studio"
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
Copy-Item -Force $ExeSrc (Join-Path $InstallDir "Ribbon.exe")
if (Test-Path $IcoSrc) { Copy-Item -Force $IcoSrc (Join-Path $InstallDir "ribbon.ico") }

$ExeDst = Join-Path $InstallDir "Ribbon.exe"
$IcoDst = Join-Path $InstallDir "ribbon.ico"

# Create Start Menu & Desktop Shortcuts
$WshShell = New-Object -ComObject WScript.Shell
$StartMenuDir = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs"
$Shortcut = $WshShell.CreateShortcut((Join-Path $StartMenuDir "Ribbon 2.0.lnk"))
$Shortcut.TargetPath = $ExeDst
$Shortcut.IconLocation = $IcoDst
$Shortcut.WorkingDirectory = $InstallDir
$Shortcut.Description = "Ribbon 2.0 - Native Music Sketchpad"
$Shortcut.Save()

$DesktopShortcut = $WshShell.CreateShortcut((Join-Path ([Environment]::GetFolderPath("Desktop")) "Ribbon 2.0.lnk"))
$DesktopShortcut.TargetPath = $ExeDst
$DesktopShortcut.IconLocation = $IcoDst
$DesktopShortcut.WorkingDirectory = $InstallDir
$DesktopShortcut.Save()

# Register .rib file association (per-user, HKCU)
New-Item -Path "HKCU:\Software\Classes\.rib" -Force | Out-Null
Set-ItemProperty -Path "HKCU:\Software\Classes\.rib" -Name "(Default)" -Value "RibbonProjectFile"
New-Item -Path "HKCU:\Software\Classes\RibbonProjectFile" -Force | Out-Null
Set-ItemProperty -Path "HKCU:\Software\Classes\RibbonProjectFile" -Name "(Default)" -Value "Ribbon 2.0 Project"
New-Item -Path "HKCU:\Software\Classes\RibbonProjectFile\DefaultIcon" -Force | Out-Null
Set-ItemProperty -Path "HKCU:\Software\Classes\RibbonProjectFile\DefaultIcon" -Name "(Default)" -Value "$IcoDst,0"
New-Item -Path "HKCU:\Software\Classes\RibbonProjectFile\shell\open\command" -Force | Out-Null
Set-ItemProperty -Path "HKCU:\Software\Classes\RibbonProjectFile\shell\open\command" -Name "(Default)" -Value "`"$ExeDst`" `"%1`""

Write-Host "`nRibbon 2.0 installed to: $InstallDir" -ForegroundColor Green
Write-Host "Start Menu shortcut, Desktop shortcut, and .rib association created!" -ForegroundColor Cyan
Start-Process $ExeDst
