# Run with pwsh -File .github/tests/Test-WindowsPackage.ps1.
# Optional: -IsccPath path/to/ISCC.exe also compiles the real installer template
# with fixture payloads. The resulting fixture installer is never executed.
[CmdletBinding()]
param([string] $IsccPath, [switch] $KeepFixture)

$ErrorActionPreference = 'Stop'
$ProjectRoot = (Resolve-Path -LiteralPath "$PSScriptRoot/../..").Path
. "$ProjectRoot/.github/scripts/utils.pwsh/Windows-Package.ps1"

$TempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$FixtureRoot = Join-Path $TempRoot ('obs-move-package-tests-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $FixtureRoot | Out-Null

function Write-FixtureFile([string] $RelativePath, [string] $Contents) {
    $Path = Join-Path $FixtureRoot $RelativePath
    New-Item -ItemType Directory -Path ([System.IO.Path]::GetDirectoryName($Path)) -Force | Out-Null
    Set-Content -LiteralPath $Path -Value $Contents -NoNewline -Encoding utf8
}

function Read-Archive([string] $Path) {
    $Archive = [System.IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $Contents = @{}
        foreach ($Entry in $Archive.Entries) {
            if (!$Entry.Name) { continue }
            $Reader = [System.IO.StreamReader]::new($Entry.Open())
            try { $Contents[$Entry.FullName.Replace('\', '/')] = $Reader.ReadToEnd() }
            finally { $Reader.Dispose() }
        }
        return $Contents
    } finally { $Archive.Dispose() }
}

function Assert-Archive([hashtable] $Actual, [hashtable] $Expected, [string] $Label) {
    if ($Actual.Count -ne $Expected.Count) {
        throw "$Label has unexpected files: $($Actual.Keys -join ', ')"
    }
    foreach ($Name in $Expected.Keys) {
        if (!$Actual.ContainsKey($Name) -or $Actual[$Name] -cne $Expected[$Name]) {
            throw "$Label has missing or changed contents: $Name"
        }
    }
}

function Assert-PackageRejected([string] $ExpectedMessage) {
    $Rejected = $false
    try { New-WindowsPluginPackage -ProjectRoot $FixtureRoot | Out-Null }
    catch {
        if ($_.Exception.Message -notlike "*$ExpectedMessage*") { throw }
        $Rejected = $true
    }
    if (!$Rejected) { throw "Expected packaging to reject: $ExpectedMessage" }
}

try {
    $Spec = '{"name":"move-transition","version":"3.2.2"}'
    Write-FixtureFile 'buildspec.json' $Spec
    Write-FixtureFile 'LICENSE' 'fixture license'
    $Prefix = 'release/RelWithDebInfo/move-transition'
    $Payload = @{
        'move-transition.dll' = 'fixture module'
        'move-transition.pdb' = 'fixture symbols'
        'dependency.dll' = 'fixture dependency'
        'data/manifest.json' = '{"name":"Move","version":"3.2.2"}'
        'data/locale/en-US.ini' = 'Move="Move"'
        'data/fixture.effect' = 'fixture effect'
    }
    foreach ($Name in $Payload.Keys) { Write-FixtureFile "$Prefix/$Name" $Payload[$Name] }
    Write-FixtureFile 'release/RelWithDebInfo/unrelated/other.dll' 'do not package or delete'
    $Packages = New-WindowsPluginPackage -ProjectRoot $FixtureRoot
    $ExpectedProgramData = @{ 'move-transition/LICENSE' = 'fixture license' }
    foreach ($Name in $Payload.Keys) { $ExpectedProgramData["move-transition/$Name"] = $Payload[$Name] }
    Assert-Archive (Read-Archive $Packages.ProgramDataArchive) $ExpectedProgramData 'ProgramData archive'
    Write-Host 'PASS: ProgramData archive has a flat module, manifest, resources, dependencies and license.'
    $ExpectedPortable = @{}
    foreach ($Name in $ExpectedProgramData.Keys) { $ExpectedPortable["plugins/$Name"] = $ExpectedProgramData[$Name] }
    Assert-Archive (Read-Archive $Packages.PortableArchive) $ExpectedPortable 'Portable archive'
    Write-Host 'PASS: Portable archive extracts under plugins/move-transition.'

    Write-FixtureFile 'release/Package/stale.dll' 'stale output'
    Write-FixtureFile 'release/Portable/plugins/stale.dll' 'stale output'
    Remove-Item -LiteralPath (Join-Path $FixtureRoot "$Prefix/data/fixture.effect")
    $ExpectedProgramData.Remove('move-transition/data/fixture.effect')
    $ExpectedPortable.Remove('plugins/move-transition/data/fixture.effect')
    $Packages = New-WindowsPluginPackage -ProjectRoot $FixtureRoot
    Assert-Archive (Read-Archive $Packages.ProgramDataArchive) $ExpectedProgramData 'Repacked ProgramData archive'
    Assert-Archive (Read-Archive $Packages.PortableArchive) $ExpectedPortable 'Repacked portable archive'
    if (!(Test-Path -LiteralPath (Join-Path $FixtureRoot 'release/RelWithDebInfo/unrelated/other.dll'))) {
        throw 'Packaging deleted an unrelated plugin.'
    }
    Write-Host 'PASS: Repackaging removes stale staged files, adds no nested folders and preserves unrelated inputs.'

    $ArchiveHash = (Get-FileHash -LiteralPath $Packages.ProgramDataArchive).Hash
    Remove-Item -LiteralPath (Join-Path $FixtureRoot "$Prefix/data/manifest.json")
    Assert-PackageRejected 'data/manifest.json'
    if ((Get-FileHash -LiteralPath $Packages.ProgramDataArchive).Hash -ne $ArchiveHash) {
        throw 'Invalid inputs changed the existing archive.'
    }
    Write-FixtureFile "$Prefix/data/manifest.json" $Payload['data/manifest.json']
    Write-Host 'PASS: Missing manifest fails before changing previous package output.'

    Write-FixtureFile "$Prefix/bin/64bit/move-transition.dll" 'legacy module'
    Assert-PackageRejected 'legacy bin directory'
    Remove-Item -LiteralPath (Join-Path $FixtureRoot "$Prefix/bin/64bit/move-transition.dll")
    [System.IO.Directory]::Delete((Join-Path $FixtureRoot "$Prefix/bin/64bit"))
    [System.IO.Directory]::Delete((Join-Path $FixtureRoot "$Prefix/bin"))
    Write-Host 'PASS: Legacy bin/64bit install trees are rejected.'

    Remove-Item -LiteralPath (Join-Path $FixtureRoot "$Prefix/move-transition.dll")
    Assert-PackageRejected 'move-transition.dll'
    Write-FixtureFile "$Prefix/move-transition.dll" $Payload['move-transition.dll']
    Write-Host 'PASS: Missing plugin module is rejected.'

    Write-FixtureFile 'buildspec.json' '{"name":"../outside","version":"3.2.2"}'
    Assert-PackageRejected 'unsafe package name'
    Write-FixtureFile 'buildspec.json' $Spec
    Write-Host 'PASS: Unsafe package names are rejected.'

    if ($IsccPath) {
        $IsccPath = (Resolve-Path -LiteralPath $IsccPath).Path
        $Template = Get-Content -LiteralPath "$ProjectRoot/cmake/windows/resources/installer-Windows.iss.in" -Raw
        $Replacements = @{
            '@CMAKE_PROJECT_NAME@' = 'move-transition'
            '@CMAKE_PROJECT_VERSION@' = '3.2.2'
            '@PLUGIN_AUTHOR@' = 'Fixture'
            '@PLUGIN_WEBSITE@' = 'https://example.invalid'
            '@UUID_APP@' = '83443BC3-6FCC-4A35-922E-1FF66F294AA4'
        }
        foreach ($Key in $Replacements.Keys) { $Template = $Template.Replace($Key, $Replacements[$Key]) }
        Write-FixtureFile 'build_x64/installer-Windows.generated.iss' $Template
        New-Item -ItemType Directory -Path (Join-Path $FixtureRoot 'media') | Out-Null
        Copy-Item -LiteralPath "$ProjectRoot/media/icon.ico" -Destination (Join-Path $FixtureRoot 'media/icon.ico')
        & $IsccPath '/Qp' "/O$FixtureRoot/release" '/Ffixture-installer' "$FixtureRoot/build_x64/installer-Windows.generated.iss"
        if ($LASTEXITCODE -ne 0) { throw "Inno Setup compilation failed: $LASTEXITCODE" }
        Write-Host 'PASS: Real installer template compiles with fixture payloads (not installed).'
    }
} finally {
    if ($KeepFixture) {
        Write-Host "Fixture retained at $FixtureRoot"
    } elseif (Test-Path -LiteralPath $FixtureRoot) {
        $ResolvedFixture = (Resolve-Path -LiteralPath $FixtureRoot).Path
        if ([System.IO.Path]::GetDirectoryName($ResolvedFixture) -ne $TempRoot.TrimEnd('\', '/') -or
            [System.IO.Path]::GetFileName($ResolvedFixture) -notlike 'obs-move-package-tests-*') {
            throw "Refusing to remove unexpected test directory: $ResolvedFixture"
        }
        Remove-Item -LiteralPath $ResolvedFixture -Recurse -Force
    }
}
