# tools/sd_folders.ps1 [-DryRun] <SD card, e.g. E:\>
#
# A board's SD card from the old shared folders into each machine's own
# (since 2026-11-17: every machine its folder, the three Macs sharing mac/;
# since 2026-11-18 its cartridges and the ROMs that override the built-in
# ones in that folder's roms/; rpi5emu's data folder the same, its
# tools/data_folders.sh):
#   roms\      each ROM into its machine's <folder>\roms\, by its name; .crt,
#              .mx1 .mx2, .ccc and .cpr cartridges into cbm, msx, coco, cpc
#   cbm\ coco\ cpc\ msx\  their cartridges into their roms\ (.crt; .rom .ccc
#              .bin; .cpr .rom; .rom .mx1 .mx2)
#   floppies\  .st .msa .hd into st\; .adl, .adf of the Archimedes' floppy
#              sizes (640 KB, 800 KB, 1.6 MB) and .img of them with an ADFS
#              directory, into arc\; the other .img into st\ (the ST's hard
#              disks: HDDRIVER's demo is 640 KB too), the other .adf
#              and .rp9 into amiga\; .hdf with the Amiga's RDSK or DOS mark
#              into amiga\, else in whole 516096-byte cylinders into arc\
#   disks\     into mac\
#   atari\     into xl\
# Nothing is overwritten; what isn't known stays where it is, listed. An old
# folder left empty goes. -DryRun: only say what it would do. PowerShell 7.
param(
    [switch]$DryRun,
    [Parameter(Mandatory)][string]$Card
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Card -PathType Container)) { throw "no folder $Card" }
$script:moved = 0
$script:kept = 0

function Move-ToFolder([IO.FileInfo]$file, [string]$folder) {
    $to = Join-Path $Card $folder
    $target = Join-Path $to $file.Name
    $from = "$($file.Directory.Name)\$($file.Name)"
    if (Test-Path -LiteralPath $target) {
        "kept ${from}: $folder\$($file.Name) is there already"
        $script:kept++
        return
    }
    "$from -> $folder\"
    if (-not $DryRun) {
        New-Item -ItemType Directory -Force -Path $to | Out-Null
        Move-Item -LiteralPath $file.FullName -Destination $target
    }
    $script:moved++
}

function Write-Kept([IO.FileInfo]$file, [string]$why) {
    "kept $($file.Directory.Name)\$($file.Name): $why"
    $script:kept++
}

# The machine folder for a ROM by its name, none if unknown
function Get-RomMachine([string]$name) {
    switch -Wildcard ($name.ToLowerInvariant()) {
        { $_ -in '128.rom', 'plus2.rom', 'plus3.rom', 'trdos.rom' } { return 'zx' }
        { $_ -in 'os464.rom', 'os664.rom', 'os6128.rom', 'osplus.rom', 'basic10.rom', 'basic11.rom',
                 'basic664.rom', 'basicplus.rom', 'amsdos.rom', 'system.cpr' } { return 'cpc' }
        { $_ -like 'c64*.rom' -or $_ -like 'c128*.rom' -or $_ -in '1541.rom', '1571.rom' } { return 'cbm' }
        { $_ -in 'bas12.rom', 'bas13.rom', 'extbas11.rom', 'disk10.rom', 'disk11.rom', 'coco3.rom' } { return 'coco' }
        { $_ -like 'gs_rom*.rom' -or $_ -eq 'chargen.rom' } { return 'apple2gs' }
        { $_ -in 'msx.rom', 'msx2.rom', 'msx2ext.rom', 'disk.rom', 'nextor.rom' } { return 'msx' }
        { $_ -in 'atarixl.rom', 'ataribas.rom' } { return 'xl' }
        'tos*.img' { return 'st' }
        { $_ -in 'plus.rom', 'se.rom', 'classic.rom', 'ii.rom', 'iifdhd.rom', 'lc.rom', 'lc2.rom', 'lc3.rom',
                 'mdc824.rom' } { return 'mac' }
        { $_ -like 'arthur*.rom' -or $_ -like 'riscos*.rom' } { return 'arc' }
        { $_ -like 'amiga-os-*.rom' -or $_ -like 'aros-*.rom' -or $_ -like 'kick*.rom' } { return 'amiga' }
    }
    return $null
}

# The machine folder for a cartridge in the old roms\ by its extension (.rom
# and .bin could be anyone's), none if unknown
function Get-CartMachine([string]$name) {
    switch ([IO.Path]::GetExtension($name).ToLowerInvariant()) {
        '.crt' { return 'cbm' }
        { $_ -in '.mx1', '.mx2' } { return 'msx' }
        '.ccc' { return 'coco' }
        '.cpr' { return 'cpc' }
    }
    return $null
}

# The first 8 KB of a file as text
function Get-Head([IO.FileInfo]$file) {
    $head = [byte[]]::new(8192)
    $s = [IO.File]::OpenRead($file.FullName)
    try { $n = $s.Read($head, 0, $head.Length) } finally { $s.Dispose() }
    return [Text.Encoding]::ASCII.GetString($head, 0, $n)
}

# st, arc or amiga for an image in floppies\, none if unknown
function Get-FloppyMachine([IO.FileInfo]$file) {
    $archie = $file.Length -in 655360, 819200, 1638400
    switch ($file.Extension.ToLowerInvariant()) {
        { $_ -in '.st', '.msa', '.hd' } { return 'st' }
        '.adl' { return 'arc' }
        '.rp9' { return 'amiga' }
        '.img' {
            # An Archimedes floppy's size and an ADFS directory ("Hugo",
            # "Nick"); else the ST's (HDDRIVER's demo hard disk is 640 KB too)
            $text = $archie ? (Get-Head $file) : ''
            return ($text.Contains('Hugo') -or $text.Contains('Nick')) ? 'arc' : 'st'
        }
        '.adf' { return $archie ? 'arc' : 'amiga' }
        '.hdf' {
            $text = Get-Head $file
            if ($text.Contains('RDSK') -or $text.StartsWith('DOS')) { return 'amiga' }
            if ($file.Length % 516096 -eq 0) { return 'arc' }
            return $null
        }
    }
    return $null
}

$roms = Join-Path $Card 'roms'
if (Test-Path -LiteralPath $roms) {
    foreach ($f in Get-ChildItem -LiteralPath $roms -File) {
        $m = Get-RomMachine $f.Name
        if (-not $m) { $m = Get-CartMachine $f.Name }
        if ($m) { Move-ToFolder $f "$m\roms" } else { Write-Kept $f 'whose ROM is it?' }
    }
}
$carts = @{ cbm = @('.crt'); coco = @('.rom', '.ccc', '.bin'); cpc = @('.cpr', '.rom'); msx = @('.rom', '.mx1', '.mx2') }
foreach ($m in $carts.Keys) {
    $from = Join-Path $Card $m
    if (Test-Path -LiteralPath $from) {
        foreach ($f in Get-ChildItem -LiteralPath $from -File) {
            if ($f.Extension.ToLowerInvariant() -in $carts[$m]) { Move-ToFolder $f "$m\roms" }
        }
    }
}
$floppies = Join-Path $Card 'floppies'
if (Test-Path -LiteralPath $floppies) {
    foreach ($f in Get-ChildItem -LiteralPath $floppies -File) {
        $m = Get-FloppyMachine $f
        if ($m) { Move-ToFolder $f $m } else { Write-Kept $f "the ST's, the Archimedes' or the Amiga's?" }
    }
}
foreach ($pair in @(@('disks', 'mac'), @('atari', 'xl'))) {
    $from = Join-Path $Card $pair[0]
    if (Test-Path -LiteralPath $from) {
        foreach ($f in Get-ChildItem -LiteralPath $from -File) { Move-ToFolder $f $pair[1] }
    }
}
if (-not $DryRun) {
    foreach ($d in 'roms', 'floppies', 'disks', 'atari') {
        $p = Join-Path $Card $d
        if ((Test-Path -LiteralPath $p) -and -not (Get-ChildItem -LiteralPath $p -Force)) {
            Remove-Item -LiteralPath $p
            "$d\ gone (empty)"
        }
    }
}
"{0}moved $script:moved, kept $script:kept" -f ($DryRun ? 'would have ' : '')
