param(
    [switch]$UsbDisk = $true,
    [ValidateSet("ntfs", "fat32", "exfat", "ext4")][string]$Fs = "ext4",
    [switch]$TraceXhci = $false,
    [ValidateSet("hda", "ac97")][string]$Audio = "hda"
)

# Configurar la consola de Windows para UTF-8 nativo (¡Soporte total para la Ñ y tildes!)
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
[Console]::InputEncoding  = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8
chcp 65001 > $null

Write-Host "==> Compilando TAEK OS en WSL (Arch Linux)..." -ForegroundColor Cyan
$directorioActual = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }

# Convertir C:\... a /mnt/c/... (robusto frente a escape de contrabarras en WSL)
$posixPath = $directorioActual.Replace('\', '/')
$wslDir = "/mnt/" + $directorioActual.Substring(0, 1).ToLower() + $directorioActual.Substring(2).Replace('\', '/')

# Validar contra wslpath usando barras inclinadas
$wslPathRes = (wsl wslpath -u "$posixPath" 2>$null)
if ($wslPathRes) {
    $wslDir = $wslPathRes.Trim()
}

wsl bash -c "cd '$wslDir' && make"
if ($LASTEXITCODE -ne 0) {
    Write-Host "Error en la compilación." -ForegroundColor Red
    exit 1
}

Write-Host "==> Lanzando TAEK OS en QEMU UEFI con xHCI, Teclado USB, Disco USB Mass Storage y Audio ($Audio)..." -ForegroundColor Green
$qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$ovmf = "C:\Program Files\qemu\share\edk2-x86_64-code.fd"
$img  = "$directorioActual\build\taek-os.img"

$argsQemu = @(
    "-drive", "if=pflash,format=raw,readonly=on,file=$ovmf",
    "-drive", "file=$img,format=raw",
    "-m", "512M",
    "-M", "q35",
    "-audiodev", "dsound,id=snd0"
)

if ($Audio -eq "hda") {
    $argsQemu += @("-device", "intel-hda", "-device", "hda-output,audiodev=snd0")
} else {
    $argsQemu += @("-device", "AC97,audiodev=snd0")
}

$argsQemu += @(
    "-device", "qemu-xhci,id=xhci",
    "-device", "usb-kbd,bus=xhci.0,id=kbd1",
    "-device", "usb-kbd,bus=xhci.0,id=kbd2",
    "-serial", "stdio"
)

if ($UsbDisk) {
    if ($Fs -eq "ext4") {
        $usbImg = "$directorioActual\build\disco_usb_ext4.img"
        if (-not (Test-Path $usbImg)) {
            Write-Host "  [+] Creando disco USB virtual ext4 (aislado en /tmp/ con sync)..." -ForegroundColor Cyan
            wsl bash -c "dd if=/dev/zero of=/tmp/disco_ext4_tmp.img bs=1M count=64 status=none && mkfs.ext4 -F -L 'TAEK_EXT4' -b 4096 /tmp/disco_ext4_tmp.img && debugfs -w -R 'mkdir /boot' /tmp/disco_ext4_tmp.img && debugfs -w -R 'mkdir /documentos' /tmp/disco_ext4_tmp.img && echo 'Hola desde Linux ext4 en Ring 0 de TAEK OS!' > /tmp/leeme.txt && debugfs -w -R 'write /tmp/leeme.txt /leeme.txt' /tmp/disco_ext4_tmp.img && sync && cp /tmp/disco_ext4_tmp.img build/disco_usb_ext4.img && sync && rm -f /tmp/disco_ext4_tmp.img /tmp/leeme.txt"
        }
    } elseif ($Fs -eq "ntfs") {
        $usbImg = "$directorioActual\build\disco_usb_ntfs.img"
        if (-not (Test-Path $usbImg)) {
            Write-Host "  [+] Creando disco USB virtual NTFS (aislado en /tmp/ con sync)..." -ForegroundColor Cyan
            wsl bash -c "dd if=/dev/zero of=/tmp/disco_ntfs_tmp.img bs=1M count=64 status=none && mkfs.ntfs -F -L 'TAEK_NTFS' -q /tmp/disco_ntfs_tmp.img && echo 'Hola desde un pendrive NTFS en Anillo 0 de TAEK OS!' > /tmp/leeme_ntfs.txt && echo 'Super secreto: El Huevo es inmortal en NTFS.' > /tmp/notas_ntfs.txt && ntfscp -f /tmp/disco_ntfs_tmp.img /tmp/leeme_ntfs.txt leeme.txt && ntfscp -f /tmp/disco_ntfs_tmp.img /tmp/notas_ntfs.txt notas.txt && sync && cp /tmp/disco_ntfs_tmp.img build/disco_usb_ntfs.img && sync && rm -f /tmp/disco_ntfs_tmp.img /tmp/leeme_ntfs.txt /tmp/notas_ntfs.txt"
        }
    } elseif ($Fs -eq "exfat") {
        $usbImg = "$directorioActual\build\disco_usb_exfat.img"
        if (-not (Test-Path $usbImg)) {
            Write-Host "  [+] Creando disco USB virtual exFAT (aislado en /tmp/ con sync)..." -ForegroundColor Cyan
            wsl bash -c "dd if=/dev/zero of=/tmp/disco_exfat_tmp.img bs=1M count=64 status=none && mkfs.exfat -L 'TAEK_EXFAT' /tmp/disco_exfat_tmp.img && sync && cp /tmp/disco_exfat_tmp.img build/disco_usb_exfat.img && sync && rm -f /tmp/disco_exfat_tmp.img"
        }
    } else {
        $usbImg = "$directorioActual\build\disco_usb_prueba.img"
        if (-not (Test-Path $usbImg)) {
            Write-Host "  [+] Creando disco USB virtual FAT32 (aislado en /tmp/ con sync)..." -ForegroundColor Cyan
            wsl bash -c "dd if=/dev/zero of=/tmp/disco_fat32_tmp.img bs=1M count=64 status=none && mformat -i /tmp/disco_fat32_tmp.img -F -v 'TAEK_USB' :: && mmd -i /tmp/disco_fat32_tmp.img ::/boot && mmd -i /tmp/disco_fat32_tmp.img ::/boot/efi && mmd -i /tmp/disco_fat32_tmp.img ::/documentos && mmd -i /tmp/disco_fat32_tmp.img ::/musica && echo 'Hola desde TAEK OS! Archivo leido de un pendrive USB en FAT32.' > /tmp/leeme.txt && echo 'Super secreto: El Huevo es inmortal.' > /tmp/notas.txt && mcopy -i /tmp/disco_fat32_tmp.img /tmp/leeme.txt ::/leeme.txt && mcopy -i /tmp/disco_fat32_tmp.img /tmp/notas.txt ::/documentos/notas.txt && sync && cp /tmp/disco_fat32_tmp.img build/disco_usb_prueba.img && sync && rm -f /tmp/disco_fat32_tmp.img /tmp/leeme.txt /tmp/notas.txt"
        }
    }
    $argsQemu += @(
        "-drive", "if=none,id=usbstick,format=raw,file=$usbImg",
        "-device", "usb-storage,bus=xhci.0,id=stick1,drive=usbstick"
    )
}

if ($TraceXhci) {
    Write-Host "  [+] Habilitando trazas de depuración de xHCI: usb_xhci_*" -ForegroundColor Yellow
    $argsQemu += @("-trace", "usb_xhci_*")
}

& $qemu @argsQemu
