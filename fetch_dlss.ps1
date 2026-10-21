# Downloads the NVIDIA DLSS SDK files the build needs into third_party\dlss.
# They come from NVIDIA's own repository (https://github.com/NVIDIA/DLSS) and
# are not kept in this repository: the NVIDIA RTX SDKs license only allows
# distributing them built into an application.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$tag = 'v310.9.1'
$base = "https://raw.githubusercontent.com/NVIDIA/DLSS/$tag"
$dest = Join-Path $PSScriptRoot 'third_party\dlss'

# Local path, path in NVIDIA's repository, SHA-256.
$files = @(
    @('LICENSE.txt', 'LICENSE.txt', 'd4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0'),
    @('include\nvsdk_ngx.h', 'include/nvsdk_ngx.h', 'dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848'),
    @('include\nvsdk_ngx_defs.h', 'include/nvsdk_ngx_defs.h', 'ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e'),
    @('include\nvsdk_ngx_loader.h', 'include/nvsdk_ngx_loader.h', 'abdf5f66a692620538357d29fdf78e3e2a677ee8140b7a638962ecb766585e75'),
    @('include\nvsdk_ngx_params.h', 'include/nvsdk_ngx_params.h', '943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc'),
    @('include\nvsdk_ngx_helpers.h', 'include/nvsdk_ngx_helpers.h', '5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471'),
    @('include\nvsdk_ngx_helpers_d3d.h', 'include/nvsdk_ngx_helpers_d3d.h', 'ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba'),
    @('include\nvsdk_ngx_helpers_cuda.h', 'include/nvsdk_ngx_helpers_cuda.h', 'f2851a76107bdf4fbcb7b261f3c35573c312d8ecb4bc18fb02d9e2cec7705042'),
    @('lib\nvsdk_ngx_s.lib', 'lib/Windows_x86_64/x64/nvsdk_ngx_s.lib', '4e5d355086d2bc11e1a0842457d2519ea528ee1f3e112c45679a84960c07dff3'),
    @('lib\nvngx_dlss.dll', 'lib/Windows_x86_64/rel/nvngx_dlss.dll', '3975567b8943c53acce397f2b72380092f84f162d00b0d2c7d08a1025c563983')
)

foreach ($f in $files) {
    $local = Join-Path $dest $f[0]
    if ((Test-Path $local) -and (Get-FileHash $local -Algorithm SHA256).Hash -eq $f[2]) { continue }
    New-Item -ItemType Directory -Force (Split-Path $local) | Out-Null
    Write-Host "Downloading $($f[1])"
    Invoke-WebRequest -UseBasicParsing "$base/$($f[1])" -OutFile $local
    $hash = (Get-FileHash $local -Algorithm SHA256).Hash
    if ($hash -ne $f[2]) {
        Remove-Item $local
        throw "Checksum mismatch for $($f[1]) (got $hash)"
    }
}
Write-Host "DLSS SDK $tag files are in $dest"
