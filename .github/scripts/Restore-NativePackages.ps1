$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$packagesConfigPath = Join-Path $repoRoot 'packages.config'
$packagesDir = Join-Path $repoRoot 'packages'
$tempDir = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [System.IO.Path]::GetTempPath() }

[xml]$packagesConfig = Get-Content -LiteralPath $packagesConfigPath -Raw
$packages = @($packagesConfig.packages.package | ForEach-Object {
    [PSCustomObject]@{
        Id = $_.GetAttribute('id')
        Version = $_.GetAttribute('version')
    }
})

New-Item -ItemType Directory -Path $packagesDir -Force | Out-Null

try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction Stop
}
catch {
    Write-Host 'System.IO.Compression.FileSystem is already available or not required.'
}

foreach ($package in $packages) {
    $packageId = $package.Id
    $version = $package.Version
    $packageUrl = "https://www.nuget.org/api/v2/package/$packageId/$version"
    $packageFile = Join-Path $tempDir "$packageId.$version.nupkg"
    $extractDir = Join-Path $packagesDir "$packageId.$version"

    Remove-Item -LiteralPath $packageFile -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $extractDir -Recurse -Force -ErrorAction SilentlyContinue

    Write-Host "Restoring $packageId $version"
    Invoke-WebRequest -Uri $packageUrl -OutFile $packageFile
    [System.IO.Compression.ZipFile]::ExtractToDirectory($packageFile, $extractDir)
}

$requiredFiles = @(
    'packages\zlib-msvc-x64.1.2.11.8900\build\native\lib_release\zlibstatic.lib',
    'packages\zlib-msvc-x86.1.2.11.8900\build\native\lib_release\zlibstatic.lib',
    'packages\libpng-msvc-x64.1.6.33.8807\build\native\lib_release\libpng16.lib',
    'packages\libpng-msvc-x86.1.6.33.8807\build\native\lib_release\libpng16.lib'
)

foreach ($relativePath in $requiredFiles) {
    $fullPath = Join-Path $repoRoot $relativePath
    if (!(Test-Path -LiteralPath $fullPath)) {
        Write-Host 'Top-level packages directory contents:'
        Get-ChildItem -LiteralPath $packagesDir -Force -ErrorAction SilentlyContinue | Select-Object FullName
        throw "Missing restored native package file: $relativePath"
    }
}
