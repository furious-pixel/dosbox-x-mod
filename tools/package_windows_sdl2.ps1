param(
    [Parameter(Mandatory=$true)][string]$Destination,
    [string]$Executable = "bin/x64/Release SDL2/dosbox-x.exe"
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dest = [IO.Path]::GetFullPath($Destination)
if (Test-Path -LiteralPath $dest) { throw "Destination already exists: $dest" }
New-Item -ItemType Directory -Path $dest | Out-Null
function Copy-Payload([string]$Source, [string]$Target) {
    $output = Join-Path $dest $Target
    New-Item -ItemType Directory -Force -Path (Split-Path $output -Parent) | Out-Null
    Copy-Item -LiteralPath (Join-Path $root $Source) -Destination $output -Recurse
}
Copy-Payload $Executable 'dosbox-x.exe'
foreach ($file in @('COPYING', 'README.md', 'CREDITS.md', 'dosbox-x.reference.conf', 'dosbox-x.reference.full.conf')) {
    Copy-Payload $file $file
}
Copy-Payload 'CHANGELOG' 'CHANGELOG.txt'
Copy-Payload 'dosbox-x.reference.conf' 'dosbox-x.conf'
# This mod release uses VGA/OpenGL and emulated I/O. External PC-98/CJK/TTF
# fonts and the physical-port driver are not part of its runtime payload.
Copy-Payload 'contrib/windows/installer/drivez_readme.txt' 'drivez/readme.txt'
foreach ($file in Get-ChildItem (Join-Path $root 'contrib/glshaders') -Filter '*.glsl') {
    Copy-Payload "contrib/glshaders/$($file.Name)" "glshaders/$($file.Name)"
}
foreach ($file in @('NOTICE', 'glshaders.txt', 'crt', 'presets')) {
    Copy-Payload "contrib/glshaders/$file" "glshaders/$file"
}
foreach ($file in Get-ChildItem (Join-Path $root 'contrib/translations/*/*.lng')) {
    Copy-Payload ([IO.Path]::GetRelativePath($root, $file.FullName)) "languages/$($file.Name)"
}
$notices = @{
    'vs/zlib/LICENSE' = 'zlib.txt'
    'vs/libpng/LICENSE' = 'libpng.txt'
    'vs/sdl2/LICENSE.txt' = 'SDL2.txt'
    'vs/sdlnet/COPYING' = 'SDL_net.txt'
    'vs/freetype/LICENSE.TXT' = 'freetype/LICENSE.TXT'
    'vs/freetype/docs/GPLv2.TXT' = 'freetype/GPLv2.TXT'
    'vs/freetype/docs/FTL.TXT' = 'freetype/FTL.TXT'
    'vs/libpdcurses/README.md' = 'pdcurses/README.md'
    'vs/libpdcurses/pdcurses/README.md' = 'pdcurses/core.md'
    'vs/libpdcurses/wincon/README.md' = 'pdcurses/wincon.md'
}
foreach ($source in $notices.Keys) { Copy-Payload $source "licenses/$($notices[$source])" }
$revision = (git -C $root rev-parse HEAD).Trim()
if ($LASTEXITCODE) { throw 'Cannot identify host source revision' }
$changes = @(git -C $root diff --name-only HEAD -- . ":(exclude)include/build_timestamp.h")
if ($LASTEXITCODE) { throw 'Cannot inspect host working tree' }
$manifest = [ordered]@{
    revision = $revision
    worktree_changes = $changes
    executable_sha256 = (Get-FileHash (Join-Path $dest 'dosbox-x.exe') -Algorithm SHA256).Hash.ToLowerInvariant()
    source_url = "https://github.com/furious-pixel/dosbox-x-mod/tree/$revision"
    configuration = 'Release SDL2 x64'
    generated_header_sha256 = (Get-FileHash (Join-Path $root 'include/build_timestamp.h') -Algorithm SHA256).Hash.ToLowerInvariant()
}
$json = ($manifest | ConvertTo-Json -Depth 4) -replace "`r`n", "`n"
[IO.File]::WriteAllText((Join-Path $dest 'HOST_BUILD_INFO.json'), $json + "`n", [Text.UTF8Encoding]::new($false))
