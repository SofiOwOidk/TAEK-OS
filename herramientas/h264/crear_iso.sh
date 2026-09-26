#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
kernel=${1:-build/nucleo.elf}
for archivo in "$kernel" 'Recursos Asets/Video 360p.mp4' 'Recursos Asets/Video 1080p.mp4'; do
    test -f "$archivo" || { echo "Falta: $archivo" >&2; exit 1; }
done
command -v xorriso >/dev/null

# Anti-BSOD: árbol e ISO se construyen en el sistema nativo de Linux.
# El nombre final es único: ninguna ISO existente se abre para escritura.
temporal=$(mktemp -d /tmp/taek-h264.XXXXXXXX)
case "$temporal" in /tmp/taek-h264.*) ;; *) exit 1 ;; esac
trap 'rm -rf -- "$temporal"' EXIT
raiz="$temporal/raiz"
mkdir -p "$raiz/boot/limine" "$raiz/EFI/BOOT" "$raiz/videos"
cp -- "$kernel" "$raiz/boot/nucleo.elf"
cp boot/limine/limine-bios-cd.bin boot/limine/limine-bios.sys boot/limine/limine-uefi-cd.bin "$raiz/boot/limine/"
cp boot/limine/BOOTX64.EFI "$raiz/EFI/BOOT/"
cp -- 'Recursos Asets/Video 360p.mp4' "$raiz/videos/360p.mp4"
cp -- 'Recursos Asets/Video 1080p.mp4' "$raiz/videos/1080p.mp4"
cat > "$raiz/limine.conf" <<'MENU'
timeout: 5

/TAEK OS - H.264 experimental - xHCI
    protocol: limine
    path: boot():/boot/nucleo.elf
    cmdline: modo=xhci
    module_path: boot():/videos/360p.mp4
    module_cmdline: h264:360p
    module_path: boot():/videos/1080p.mp4
    module_cmdline: h264:1080p

/TAEK OS - H.264 experimental - PS2
    protocol: limine
    path: boot():/boot/nucleo.elf
    cmdline: modo=ps2
    module_path: boot():/videos/360p.mp4
    module_cmdline: h264:360p
    module_path: boot():/videos/1080p.mp4
    module_cmdline: h264:1080p

/H.264 - Diagnostico automatico 360p - PS2
    protocol: limine
    path: boot():/boot/nucleo.elf
    cmdline: modo=ps2 h264=prueba360
    module_path: boot():/videos/360p.mp4
    module_cmdline: h264:360p

/H.264 - Diagnostico automatico 1080p - PS2
    protocol: limine
    path: boot():/boot/nucleo.elf
    cmdline: modo=ps2 h264=prueba1080
    module_path: boot():/videos/1080p.mp4
    module_cmdline: h264:1080p
MENU
# Sólo para las ejecuciones automáticas locales. Por defecto se abre el menú.
if [[ ${H264_ENTRADA:-1} != 1 ]]; then
    case "$H264_ENTRADA" in 2|3|4) ;; *) echo 'Entrada inválida (1..4)' >&2; exit 1 ;; esac
    sed -i "1i default_entry: $H264_ENTRADA" "$raiz/limine.conf"
fi
cp "$raiz/limine.conf" "$raiz/boot/limine/limine.conf"
xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin \
    -no-emul-boot -boot-load-size 4 -boot-info-table \
    --efi-boot boot/limine/limine-uefi-cd.bin \
    -efi-boot-part --efi-boot-image --protective-msdos-label \
    "$raiz" -o "$temporal/video.iso" > "$temporal/xorriso.log" 2>&1 || {
        cat "$temporal/xorriso.log" >&2; exit 1;
    }
if [[ -x boot/limine/limine ]]; then boot/limine/limine bios-install "$temporal/video.iso"; fi
sync
mkdir -p build/h264
destino="build/h264/taek-h264-$(date +%Y%m%d-%H%M%S)-${temporal##*.}.iso"
# noclobber reserva el nombre y evita sobrescribir un archivo preexistente.
(set -o noclobber; : > "$destino")
cp -- "$temporal/video.iso" "$destino"
sync
printf 'ISO experimental: %s\n' "$destino"
