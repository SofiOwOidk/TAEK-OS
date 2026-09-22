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

Write-Host "==> Lanzando TAEK OS en QEMU UEFI con Audio AC97 y Pantalla Five Nights..." -ForegroundColor Green
$qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$ovmf = "C:\Program Files\qemu\share\edk2-x86_64-code.fd"
$img  = "$directorioActual\build\taek-os.img"

& $qemu -drive if=pflash,format=raw,readonly=on,file="$ovmf" `
        -drive file="$img",format=raw `
        -m 512M `
        -M q35 `
        -audiodev dsound,id=snd0 `
        -device AC97,audiodev=snd0 `
        -serial stdio
