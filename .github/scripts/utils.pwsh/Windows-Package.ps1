# Build OBS 33+ Windows packages from the CMake install tree. Kept separate from
# the CI entry point so the archive layout can be tested without an OBS build.
function New-WindowsPluginPackage {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)]
        [string] $ProjectRoot,
        [ValidateSet('x64')]
        [string] $Target = 'x64',
        [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
        [string] $Configuration = 'RelWithDebInfo'
    )

    $ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot -ErrorAction Stop).Path
    $BuildSpec = Get-Content -LiteralPath (Join-Path $ProjectRoot 'buildspec.json') -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version
    if ($ProductName -notmatch '^[a-zA-Z0-9_-]+$' -or $ProductVersion -notmatch '^[a-zA-Z0-9][a-zA-Z0-9.+_-]*$') {
        throw 'The build specification contains an unsafe package name or version.'
    }

    $ReleaseRoot = [System.IO.Path]::GetFullPath((Join-Path $ProjectRoot 'release'))
    $SourceRoot = Join-Path $ReleaseRoot "$Configuration/$ProductName"
    foreach ($RequiredFile in @("$ProductName.dll", 'data/manifest.json', 'data/locale/en-US.ini')) {
        if (!(Test-Path -LiteralPath (Join-Path $SourceRoot $RequiredFile) -PathType Leaf)) {
            throw "Missing package input: $RequiredFile. Run CMake build and install first."
        }
    }
    if (Test-Path -LiteralPath (Join-Path $SourceRoot 'bin')) {
        throw 'The install tree contains the legacy bin directory. Use a clean install tree for OBS 33+.'
    }
    $LicensePath = Join-Path $ProjectRoot 'LICENSE'
    if (!(Test-Path -LiteralPath $LicensePath -PathType Leaf)) {
        throw 'The plugin license is missing.'
    }

    # Never traverse a junction/symlink while staging or removing old staging
    # output. All recursive removal is restricted to these two release children.
    foreach ($InputRoot in @($ReleaseRoot, $SourceRoot)) {
        if ((Get-Item -LiteralPath $InputRoot).Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
            throw "Package input must not be a reparse point: $InputRoot"
        }
    }
    if (Get-ChildItem -LiteralPath $SourceRoot -Recurse -Force | Where-Object {
        $_.Attributes -band [System.IO.FileAttributes]::ReparsePoint
    }) {
        throw 'Package inputs must not contain reparse points.'
    }

    $PackageRoot = Join-Path $ReleaseRoot 'Package'
    $PortableRoot = Join-Path $ReleaseRoot 'Portable'
    foreach ($StageRoot in @($PackageRoot, $PortableRoot)) {
        $StageRoot = [System.IO.Path]::GetFullPath($StageRoot)
        if ([System.IO.Path]::GetDirectoryName($StageRoot) -ne $ReleaseRoot -or
            [System.IO.Path]::GetFileName($StageRoot) -notin @('Package', 'Portable')) {
            throw "Refusing to clear an unexpected staging directory: $StageRoot"
        }
        if (Test-Path -LiteralPath $StageRoot) {
            $StageItem = Get-Item -LiteralPath $StageRoot
            if (!$StageItem.PSIsContainer -or
                ($StageItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -or
                (Get-ChildItem -LiteralPath $StageRoot -Recurse -Force | Where-Object {
                    $_.Attributes -band [System.IO.FileAttributes]::ReparsePoint
                })) {
                throw "Refusing to clear unsafe staging output: $StageRoot"
            }
            Remove-Item -LiteralPath $StageRoot -Recurse -Force
        }
        New-Item -ItemType Directory -Path $StageRoot -Force | Out-Null
    }

    Copy-Item -LiteralPath $SourceRoot -Destination $PackageRoot -Recurse
    Copy-Item -LiteralPath $LicensePath -Destination (Join-Path $PackageRoot "$ProductName/LICENSE")
    $PortablePlugins = New-Item -ItemType Directory -Path (Join-Path $PortableRoot 'plugins')
    Copy-Item -LiteralPath (Join-Path $PackageRoot $ProductName) -Destination $PortablePlugins.FullName -Recurse

    $OutputName = "$ProductName-$ProductVersion-windows-$Target"
    $ProgramDataArchive = Join-Path $ReleaseRoot "$OutputName-programdata.zip"
    $PortableArchive = Join-Path $ReleaseRoot "$OutputName-portable.zip"
    Compress-Archive -LiteralPath (Join-Path $PackageRoot $ProductName) -DestinationPath $ProgramDataArchive -CompressionLevel Optimal -Force
    Compress-Archive -LiteralPath $PortablePlugins.FullName -DestinationPath $PortableArchive -CompressionLevel Optimal -Force

    return @{
        PackageRoot = $PackageRoot
        ProgramDataArchive = $ProgramDataArchive
        PortableArchive = $PortableArchive
    }
}
