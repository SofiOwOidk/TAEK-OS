# Configurar la consola de Windows para UTF-8 nativo (¡Soporte total para la Ñ y tildes!)
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
[Console]::InputEncoding  = [System.Text.Encoding]::UTF8
$OutputEncoding           = [System.Text.Encoding]::UTF8
chcp 65001 > $null

Write-Host "==> Compilando TAEK OS en WSL (Arch Linux)..." -ForegroundColor Cyan
$wslDir = (wsl wslpath -u "$PSScriptRoot").Trim()
wsl bash -c "cd '$wslDir' && make"
if ($LASTEXITCODE -ne 0) {
    Write-Host "Error en la compilación." -ForegroundColor Red
    exit 1
}

Write-Host "==> Lanzando TAEK OS en QEMU UEFI con Audio AC97 y Pantalla Five Nights..." -ForegroundColor Green
$qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$ovmf = "C:\Program Files\qemu\share\edk2-x86_64-code.fd"
$img  = "$PSScriptRoot\build\taek-os.img"

& $qemu -drive if=pflash,format=raw,readonly=on,file="$ovmf" `
        -drive file="$img",format=raw `
        -m 512M `
        -M q35 `
        -audiodev dsound,id=snd0 `
        -device AC97,audiodev=snd0 `
        -serial stdio
