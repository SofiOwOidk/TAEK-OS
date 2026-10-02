param([switch]$Recompilar)
$ErrorActionPreference = 'Stop'
$raizTaek = $PSScriptRoot
if ($Recompilar) {
    $wslTaek = '/mnt/' + $raizTaek.Substring(0, 1).ToLower() + $raizTaek.Substring(2).Replace('\', '/')
    & wsl -d ArchLinux --cd $wslTaek -- make iso
    if ($LASTEXITCODE) { throw 'Fallo de compilacion' }
}
$isoTaek = if ($Recompilar) {
    Get-ChildItem -LiteralPath (Join-Path $raizTaek 'build') -Filter '*.iso' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
} else {
    Get-Item -LiteralPath (Join-Path $raizTaek 'build/taek-os-h96-2026-10-02.iso')
}
$qemuTaek = 'C:\Program Files\qemu\qemu-system-x86_64.exe'
$firmwareTaek = 'C:\Program Files\qemu\share\edk2-x86_64-code.fd'
& $qemuTaek -M q35 -m 1024 -smp 4 -drive "if=pflash,format=raw,readonly=on,file=$firmwareTaek" `
    -cdrom $isoTaek.FullName -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 `
    -device intel-hda -device hda-output,audiodev=snd0 -audiodev dsound,id=snd0 `
    -netdev user,id=red0 -device e1000e,netdev=red0 -serial stdio
