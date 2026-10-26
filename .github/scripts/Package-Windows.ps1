[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw 'Package-Windows.ps1 requires CI environment'
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw 'Packaging script requires a 64-bit system to build and run.'
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    throw 'The packaging script requires PowerShell 7.2 or newer.'
}

$ProjectRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
foreach ($Utility in (Get-ChildItem -Path "$PSScriptRoot/utils.pwsh/*.ps1")) {
    . $Utility.FullName
}

$BuildSpec = Get-Content -LiteralPath "$ProjectRoot/buildspec.json" -Raw | ConvertFrom-Json
$OutputName = "$($BuildSpec.name)-$($BuildSpec.version)-windows-$Target"
$IsccFile = "$ProjectRoot/build_$Target/installer-Windows.generated.iss"
if (!(Test-Path -LiteralPath $IsccFile -PathType Leaf)) {
    throw 'Inno Setup script not found. Run CMake build and install first.'
}

Log-Information 'Creating OBS 33+ ProgramData and portable archives...'
$Packages = New-WindowsPluginPackage -ProjectRoot $ProjectRoot -Target $Target -Configuration $Configuration

Log-Information 'Creating Inno Setup installer...'
Push-Location -LiteralPath "$ProjectRoot/release"
try {
    Invoke-External iscc $IsccFile /O. /F"$OutputName-Installer"
} finally {
    Pop-Location
}
