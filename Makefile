CC      = clang
LD      = ld.lld
NASM    = nasm

FECHA_BUILD = $(shell date +'%Y-%m-%d %H:%M:%S')

CFLAGS  = -target x86_64-unknown-none-elf \
          -std=c11 \
          -Wall -Wextra \
          -O2 -pipe \
          -nostdlib -ffreestanding \
          -fno-stack-protector -fno-stack-check \
          -fno-lto -fPIE \
          -m64 -march=x86-64 \
          -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone \
          -mcmodel=kernel \
          -DCOMPILACION_FECHA="\"$(FECHA_BUILD)\"" \
          -I./nucleo -I.

LDFLAGS = -nostdlib -static -m elf_x86_64 -z max-page-size=0x1000 -T linker.ld

BUILD_DIR = build
RECURSOS  = recursos

C_SRCS    = nucleo/principal.c \
            nucleo/arquitectura/x86_64/serial.c \
            nucleo/arquitectura/x86_64/gdt.c \
            nucleo/arquitectura/x86_64/idt.c \
            nucleo/arquitectura/x86_64/apic.c \
            nucleo/arquitectura/x86_64/pci.c \
            nucleo/arquitectura/x86_64/vmx.c \
            nucleo/base/huevo.c \
            nucleo/base/energia.c \
            nucleo/base/utf8.c \
            nucleo/base/tiempo.c \
            nucleo/base/memoria.c \
            nucleo/base/dma.c \
            nucleo/base/paginacion.c \
            nucleo/controladores/pantalla.c \
            nucleo/controladores/audio_ac97.c \
            nucleo/controladores/audio_hda.c \
            nucleo/controladores/animacion_cangrejo.c \
            nucleo/controladores/teclado.c \
            nucleo/controladores/consola.c \
            nucleo/controladores/gpu.c \
            nucleo/controladores/iommu.c \
            nucleo/compatibilidad/linux.c \
            nucleo/compatibilidad/nv_os_interface.c \
            nucleo/controladores/video/nvidia/core/nvidia_core.c \
            nucleo/controladores/video/nvidia/firmware/gsp_firmware.c \
            nucleo/controladores/video/nvidia/gsp/gsp_rpc.c \
            nucleo/controladores/xhci.c \
            nucleo/controladores/usb_msc.c \
            nucleo/controladores/fat32.c \
            nucleo/controladores/exfat.c \
            nucleo/controladores/ntfs.c \
            nucleo/controladores/ext4.c \
            nucleo/controladores/vfs.c \
            nucleo/controladores/terminal.c

S_SRCS    = nucleo/arquitectura/x86_64/trampas.s

OBJS      = $(patsubst %.c, $(BUILD_DIR)/%.o, $(C_SRCS)) \
            $(patsubst %.s, $(BUILD_DIR)/%.o, $(S_SRCS)) \
            $(BUILD_DIR)/imagen_arranque.o \
            $(BUILD_DIR)/audio_arranque.o \
            $(BUILD_DIR)/cangrejo_video.o \
            $(BUILD_DIR)/cangrejo_audio.o \
            $(BUILD_DIR)/duelo_audio.o

IMG       = $(BUILD_DIR)/taek-os.img
KERNEL    = $(BUILD_DIR)/nucleo.elf
ISO       = $(BUILD_DIR)/taek-os.iso

all: $(IMG) $(ISO)

$(BUILD_DIR)/imagen_arranque.bin: $(RECURSOS)/fivenights.png
	@mkdir -p $(BUILD_DIR)
	@echo "==> Convirtiendo imagen Five Nights a BGRA32 crudo..."
	ffmpeg -y -i "$<" -pix_fmt bgra -f rawvideo $@

$(BUILD_DIR)/audio_arranque.bin: $(RECURSOS)/damonte.mp3
	@mkdir -p $(BUILD_DIR)
	@echo "==> Convirtiendo cancion a PCM 44.1kHz 16-bit..."
	ffmpeg -y -i "$<" -ac 2 -ar 44100 -f s16le $@

$(BUILD_DIR)/cangrejo_video.bin: $(RECURSOS)/cangrejo.mp4
	@mkdir -p $(BUILD_DIR)
	@echo "==> Extrayendo fotogramas de Don Cangrejo (288x360 BGRA32)..."
	ffmpeg -y -i "$<" -pix_fmt bgra -f rawvideo $@

$(BUILD_DIR)/cangrejo_audio.bin: $(RECURSOS)/cangrejo.mp4
	@mkdir -p $(BUILD_DIR)
	@echo "==> Extrayendo audio de explosion de Don Cangrejo a PCM..."
	ffmpeg -y -i "$<" -ac 2 -ar 44100 -f s16le $@

$(BUILD_DIR)/imagen_arranque.o: $(BUILD_DIR)/imagen_arranque.bin
	@echo "==> Enlazando imagen binaria como objeto ELF64..."
	@cd $(BUILD_DIR) && objcopy -I binary -O elf64-x86-64 -B i386:x86-64 imagen_arranque.bin imagen_arranque.o

$(BUILD_DIR)/audio_arranque.o: $(BUILD_DIR)/audio_arranque.bin
	@echo "==> Enlazando audio binario como objeto ELF64..."
	@cd $(BUILD_DIR) && objcopy -I binary -O elf64-x86-64 -B i386:x86-64 audio_arranque.bin audio_arranque.o

$(BUILD_DIR)/cangrejo_video.o: $(BUILD_DIR)/cangrejo_video.bin
	@echo "==> Enlazando video Don Cangrejo como objeto ELF64..."
	@cd $(BUILD_DIR) && objcopy -I binary -O elf64-x86-64 -B i386:x86-64 cangrejo_video.bin cangrejo_video.o

$(BUILD_DIR)/cangrejo_audio.o: $(BUILD_DIR)/cangrejo_audio.bin
	@echo "==> Enlazando audio Don Cangrejo como objeto ELF64..."
	@cd $(BUILD_DIR) && objcopy -I binary -O elf64-x86-64 -B i386:x86-64 cangrejo_audio.bin cangrejo_audio.o

$(BUILD_DIR)/duelo_audio.bin: $(RECURSOS)/duelo.mp3
	@mkdir -p $(BUILD_DIR)
	@echo "==> Convirtiendo audio de duelo completo a PCM 44.1kHz 16-bit..."
	ffmpeg -y -i "$<" -ac 2 -ar 44100 -f s16le $@

$(BUILD_DIR)/duelo_audio.o: $(BUILD_DIR)/duelo_audio.bin
	@echo "==> Enlazando audio de duelo como objeto ELF64..."
	@cd $(BUILD_DIR) && objcopy -I binary -O elf64-x86-64 -B i386:x86-64 duelo_audio.bin duelo_audio.o

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.s
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(KERNEL): $(OBJS) linker.ld
	@mkdir -p $(BUILD_DIR)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(IMG): $(KERNEL) boot/limine.conf
	@echo "==> Generando / Actualizando Imagen de Arranque UEFI FAT32 (128 MB)..."
	@if [ ! -f $(IMG) ]; then \
		dd if=/dev/zero of=$(IMG) bs=1M count=128 status=none && \
		mformat -i $(IMG) -F :: && \
		mmd -i $(IMG) ::/EFI && \
		mmd -i $(IMG) ::/EFI/BOOT && \
		mmd -i $(IMG) ::/boot && \
		mmd -i $(IMG) ::/boot/limine && \
		mcopy -i $(IMG) boot/limine/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI; \
	fi
	@mcopy -o -i $(IMG) boot/limine.conf ::/boot/limine/limine.conf
	@mcopy -o -i $(IMG) boot/limine.conf ::/limine.conf
	@mcopy -o -i $(IMG) $(KERNEL) ::/boot/nucleo.elf
	@echo "==> Imagen $(IMG) lista y sincronizada!"

$(ISO): $(KERNEL) boot/limine.conf
	@echo "==> Generando Imagen ISO Booteable UEFI/BIOS Híbrida..."
	@mkdir -p "build antigua"
	@if ls $(BUILD_DIR)/taek-os-*.iso 1> /dev/null 2>&1; then \
		echo "==> Archivando compilaciones anteriores en 'build antigua'..."; \
		cp -u $(BUILD_DIR)/taek-os-*.iso "build antigua/" 2>/dev/null || true; \
	fi
	@mkdir -p $(BUILD_DIR)/iso_root/boot/limine $(BUILD_DIR)/iso_root/EFI/BOOT
	@cp boot/limine/limine-bios-cd.bin boot/limine/limine-bios.sys boot/limine/limine-uefi-cd.bin $(BUILD_DIR)/iso_root/boot/limine/
	@cp boot/limine/BOOTX64.EFI $(BUILD_DIR)/iso_root/EFI/BOOT/
	@FECHA_MENU=$$(date +'%Y-%m-%d_%H-%M-%S'); \
	sed "s|/TAEK OS|/TAEK OS ($${FECHA_MENU})|" boot/limine.conf > $(BUILD_DIR)/iso_root/boot/limine/limine.conf; \
	cp $(BUILD_DIR)/iso_root/boot/limine/limine.conf $(BUILD_DIR)/iso_root/limine.conf
	@cp $(KERNEL) $(BUILD_DIR)/iso_root/boot/nucleo.elf
	@xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin \
	        -no-emul-boot -boot-load-size 4 -boot-info-table \
	        --efi-boot boot/limine/limine-uefi-cd.bin \
	        -efi-boot-part --efi-boot-image --protective-msdos-label \
	        $(BUILD_DIR)/iso_root -o $@ > /dev/null 2>&1
	@if [ -f boot/limine/limine ]; then \
		boot/limine/limine bios-install $@ > /dev/null 2>&1; \
	fi
	@FECHA_ARCHIVO=$$(date +'%Y-%m-%d_%H-%M-%S'); \
	cp $@ $(BUILD_DIR)/taek-os-$${FECHA_ARCHIVO}.iso; \
	echo "==> Imagen ISO principal: $(ISO)"; \
	echo "==> Copia fechada creada: $(BUILD_DIR)/taek-os-$${FECHA_ARCHIVO}.iso"; \
	echo "==> Lista para grabar en USB con Rufus / Ventoy / Etcher!"

clean:
	@mkdir -p "build antigua"
	@if ls $(BUILD_DIR)/taek-os-*.iso 1> /dev/null 2>&1; then \
		cp -u $(BUILD_DIR)/taek-os-*.iso "build antigua/" 2>/dev/null || true; \
	fi
	rm -rf $(BUILD_DIR)

qemu: $(IMG)
	@echo "==> Lanzando QEMU con controlador xHCI y teclado USB virtual..."
	@"/mnt/c/Program Files/qemu/qemu-system-x86_64.exe" \
		-drive if=pflash,format=raw,readonly=on,file="/mnt/c/Program Files/qemu/share/edk2-x86_64-code.fd" \
		-drive file="$(IMG)",format=raw \
		-m 512M \
		-M q35 \
		-audiodev dsound,id=snd0 \
		-device AC97,audiodev=snd0 \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-serial stdio

qemu-trace: $(IMG)
	@echo "==> Lanzando QEMU con xHCI, teclado USB y trazas activas..."
	@"/mnt/c/Program Files/qemu/qemu-system-x86_64.exe" \
		-drive if=pflash,format=raw,readonly=on,file="/mnt/c/Program Files/qemu/share/edk2-x86_64-code.fd" \
		-drive file="$(IMG)",format=raw \
		-m 512M \
		-M q35 \
		-audiodev dsound,id=snd0 \
		-device AC97,audiodev=snd0 \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-trace "usb_xhci_*" \
		-serial stdio

.PHONY: all clean qemu qemu-trace
