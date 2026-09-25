param(
    [switch]$UsbDisk = $true,
    [switch]$TraceXhci = $false
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

Write-Host "==> Lanzando TAEK OS en QEMU UEFI con xHCI, Teclado USB, Disco USB Mass Storage y Audio AC97..." -ForegroundColor Green
$qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$ovmf = "C:\Program Files\qemu\share\edk2-x86_64-code.fd"
$img  = "$directorioActual\build\taek-os.img"

$argsQemu = @(
    "-drive", "if=pflash,format=raw,readonly=on,file=$ovmf",
    "-drive", "file=$img,format=raw",
    "-m", "512M",
    "-M", "q35",
    "-audiodev", "dsound,id=snd0",
    "-device", "AC97,audiodev=snd0",
    "-device", "qemu-xhci,id=xhci",
    "-device", "usb-kbd,bus=xhci.0,id=kbd1",
    "-device", "usb-kbd,bus=xhci.0,id=kbd2",
    "-serial", "stdio"
)

if ($UsbDisk) {
    $usbImg = "$directorioActual\build\disco_usb_prueba.img"
    if (-not (Test-Path $usbImg)) {
        Write-Host "  [+] Creando disco USB virtual FAT32 con árbol de carpetas de prueba..." -ForegroundColor Cyan
        wsl bash -c "dd if=/dev/zero of=build/disco_usb_prueba.img bs=1M count=64 status=none && mformat -i build/disco_usb_prueba.img -F -v 'TAEK_USB' :: && mmd -i build/disco_usb_prueba.img ::/boot && mmd -i build/disco_usb_prueba.img ::/boot/efi && mmd -i build/disco_usb_prueba.img ::/documentos && mmd -i build/disco_usb_prueba.img ::/musica && echo 'Hola desde TAEK OS! Archivo leido de un pendrive USB en FAT32.' > build/leeme.txt && echo 'Super secreto: El Huevo es inmortal.' > build/notas.txt && mcopy -i build/disco_usb_prueba.img build/leeme.txt ::/leeme.txt && mcopy -i build/disco_usb_prueba.img build/notas.txt ::/documentos/notas.txt"
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
