CC      = clang
LD      = ld.lld
NASM    = nasm
BUILD_DIR ?= build

FECHA_BUILD = $(shell date +'%Y-%m-%d')
HORA_BUILD  = $(shell date +'%H:%M:%S')
REVISION_CODIGO := $(shell if [ -f herramientas/identificar_fuentes.py ]; then python3 herramientas/identificar_fuentes.py --source-root . --manifest $(BUILD_DIR)/fuentes.json; else echo "release"; fi)
# en todos los fuentes del núcleo.
.DEFAULT_GOAL := all
H264_INTER_ESCALAR ?= 0
H264_PERFIL_INTER ?= 0
H264_INTER_SSE2 ?= 1
# A4: 1 = perfil detallado (relojes por macrobloque); 0 = perfil de rendimiento.
H264_TELEMETRIA_DETALLADA ?= 1
# Incluye GEM/VCS/VA locales en el enlace. Esta opcion no acredita hardware:
# i915_driver_iniciar mantiene la secuencia activa bloqueada (M01/M11-M15).
TAEK_I915_HW ?= 1

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
          -DCOMPILACION_HORA="\"$(HORA_BUILD)\"" \
          -DTAEK_REVISION_CODIGO="\"$(REVISION_CODIGO)\"" \
          -DH264_PERFIL_INTER=$(H264_PERFIL_INTER) \
          -DH264_INTER_SSE2=$(H264_INTER_SSE2) \
          -DH264_TELEMETRIA_DETALLADA=$(H264_TELEMETRIA_DETALLADA) \
          -I./nucleo -I. \
          -I./nucleo/controladores/multimedia/mp4 \
          -I./nucleo/controladores/multimedia/h264 \
          -I./nucleo/controladores/multimedia/aac \
          -I./nucleo/controladores/multimedia/reproductor

LDFLAGS = -nostdlib -static -m elf_x86_64 -z max-page-size=0x1000 -T linker.ld

ifeq ($(H264_INTER_ESCALAR),1)
CFLAGS += -DH264_INTER_ESCALAR
endif

RECURSOS  = recursos/assets

# Fragmento 1080p <= 32 MiB recortado desde el primer IDR. Es el mismo archivo
# que exige el diagnóstico RAM del reproductor (rechaza > 32 MiB) y sirve para
# comparar la misma fuente en RAM y en USB. Se regenera si se borra el archivo.
FRAGMENTO_1080P = $(BUILD_DIR)/video_1080p_ram.mp4

C_SRCS    = nucleo/principal.c \
            nucleo/arquitectura/x86_64/serial.c \
            nucleo/arquitectura/x86_64/gdt.c \
            nucleo/arquitectura/x86_64/idt.c \
            nucleo/arquitectura/x86_64/fpu.c \
            nucleo/arquitectura/x86_64/smp.c \
            nucleo/arquitectura/x86_64/apic.c \
            nucleo/arquitectura/x86_64/pci.c \
            nucleo/arquitectura/x86_64/vmx.c \
            nucleo/arquitectura/x86_64/triple_fault.c \
            nucleo/base/huevo.c \
            nucleo/base/energia.c \
            nucleo/base/utf8.c \
            nucleo/base/tiempo.c \
            nucleo/base/trabajos.c \
            nucleo/base/version.c \
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
            nucleo/compatibilidad/linux_i915/tareas.c \
            nucleo/compatibilidad/linux_i915/memoria_i915.c \
            nucleo/controladores/video/nvidia/core/nvidia_core.c \
            nucleo/controladores/video/nvidia/firmware/gsp_firmware.c \
            nucleo/controladores/video/nvidia/gsp/gsp_rpc.c \
            nucleo/controladores/video/intel/intel_info.c \
            nucleo/controladores/video/intel/intel_diagnostico_ring0.c \
            nucleo/controladores/video/intel/i915_drv.c \
            nucleo/controladores/xhci.c \
            nucleo/controladores/usb_msc.c \
            nucleo/controladores/usb_msc_telemetria.c \
            nucleo/controladores/particiones.c \
            nucleo/controladores/fat_lector.c \
            nucleo/controladores/fat32.c \
            nucleo/controladores/exfat.c \
            nucleo/controladores/ntfs.c \
            nucleo/controladores/ext4.c \
            nucleo/controladores/vfs.c \
            nucleo/controladores/red/e1000.c \
            nucleo/controladores/red/pila.c \
            nucleo/controladores/red/telemetria.c \
            nucleo/controladores/red/mensajes.c \
            nucleo/controladores/terminal.c

C_SRCS   += $(wildcard nucleo/controladores/multimedia/mp4/*.c) \
            $(wildcard nucleo/controladores/multimedia/h264/*.c) \
            $(wildcard nucleo/controladores/multimedia/aac/*.c) \
            $(wildcard nucleo/controladores/multimedia/mp3/*.c) \
            $(wildcard nucleo/controladores/multimedia/imagen/*.c) \
            $(wildcard nucleo/controladores/multimedia/reproductor/*.c)

# Objetos locales: TAEK_I915_HW=1 no integra las fuentes Linux upstream.
ifeq ($(TAEK_I915_HW),1)
CFLAGS += -DTAEK_I915_HW
C_SRCS += nucleo/controladores/video/intel/i915_gem.c \
          nucleo/controladores/video/intel/i915_vcs.c \
          nucleo/controladores/video/intel/i915_va.c
endif

MULTIMEDIA_HEADERS = $(wildcard nucleo/controladores/multimedia/*/*.h)
LECTURA_HEADERS = nucleo/controladores/particiones.h nucleo/controladores/fat_lector.h nucleo/controladores/vfs.h nucleo/controladores/red/red.h
$(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SRCS)): $(LECTURA_HEADERS)
$(BUILD_DIR)/nucleo/controladores/fat_lector.o: nucleo/controladores/fat_oem850.h
$(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SRCS)): nucleo/controladores/multimedia/mp4/mp4.h

S_SRCS    = nucleo/arquitectura/x86_64/trampas.s \
            nucleo/arquitectura/x86_64/smp_entrada.s \
            nucleo/compatibilidad/linux_i915/abi_test_asm.s

OBJS      = $(patsubst %.c, $(BUILD_DIR)/%.o, $(C_SRCS)) \
            $(patsubst %.s, $(BUILD_DIR)/%.o, $(S_SRCS)) \
            $(BUILD_DIR)/imagen_arranque.o \
            $(BUILD_DIR)/audio_arranque.o \
            $(BUILD_DIR)/cangrejo_video.o \
            $(BUILD_DIR)/cangrejo_audio.o \
            $(BUILD_DIR)/duelo_audio.o

IMG       = $(BUILD_DIR)/taek-os.img
KERNEL    = $(BUILD_DIR)/nucleo.elf
ISO_FECHA := $(shell date +'%Y-%m-%d_%H-%M-%S')
ISO       = $(BUILD_DIR)/taek-os-$(ISO_FECHA).iso

all: $(IMG) $(ISO)

.PHONY: iso
iso: $(ISO)

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

$(BUILD_DIR)/nucleo/controladores/multimedia/mp3/minimp3_impl.o: nucleo/controladores/multimedia/mp3/minimp3_impl.c nucleo/controladores/multimedia/mp3/minimp3.h
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -mno-80387 -mno-sse -mno-sse2,$(CFLAGS)) -mno-80387 -mno-mmx -msse2 -mfpmath=sse -c $< -o $@

$(BUILD_DIR)/nucleo/controladores/multimedia/h264/inter_sse2.o: nucleo/controladores/multimedia/h264/inter_sse2.c
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -mno-sse -mno-sse2,$(CFLAGS)) -msse2 -mno-avx -mno-avx2 -c $< -o $@

$(BUILD_DIR)/nucleo/controladores/multimedia/h264/etapas_sse2.o: nucleo/controladores/multimedia/h264/etapas_sse2.c
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -mno-sse -mno-sse2,$(CFLAGS)) -msse2 -mno-avx -mno-avx2 -c $< -o $@

.PHONY: forzar_version
$(BUILD_DIR)/nucleo/base/version.o: forzar_version

# Cambiar una ruta de comparación debe recompilar el despacho y el informe,
# incluso si los fuentes no cambiaron. El resto del kernel conserva sus flags.
.PHONY: forzar_config_h264
$(BUILD_DIR)/h264-config: forzar_config_h264
	@mkdir -p $(BUILD_DIR)
	@printf '%s\n' 'escalar=$(H264_INTER_ESCALAR) perfil=$(H264_PERFIL_INTER) sse2=$(H264_INTER_SSE2) detallada=$(H264_TELEMETRIA_DETALLADA) revision=$(REVISION_CODIGO)' > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv -f $@.tmp $@

$(patsubst %.c,$(BUILD_DIR)/%.o,$(wildcard nucleo/controladores/multimedia/h264/*.c)) $(BUILD_DIR)/nucleo/controladores/multimedia/reproductor/reproductor.o: $(BUILD_DIR)/h264-config Makefile

$(BUILD_DIR)/%.o: %.s
	@mkdir -p $(dir $@)
	$(NASM) -f elf64 $< -o $@

$(KERNEL): $(OBJS) linker.ld
	@mkdir -p $(BUILD_DIR)
	$(LD) $(LDFLAGS) $(OBJS) -o $@

$(FRAGMENTO_1080P):
	@if [ -f "recursos/assets/Video 1080p.mp4" ]; then \
		echo "==> Recortando fragmento 1080p <= 32 MiB (desde el primer IDR)..."; \
		rm -f "$(FRAGMENTO_1080P)"; \
		for t in 75 70 65 60 55 50 45 40 35 30; do \
			ffmpeg -v error -y -i "recursos/assets/Video 1080p.mp4" -t $$t -c copy -avoid_negative_ts make_zero "$(FRAGMENTO_1080P)"; \
			sz=$$(stat -c %s "$(FRAGMENTO_1080P)"); \
			if [ $$sz -le 33554432 ]; then echo "    fragmento=$${t}s bytes=$$sz"; break; fi; \
			rm -f "$(FRAGMENTO_1080P)"; \
		done; \
	else \
		echo "==> Sin 'recursos/assets/Video 1080p.mp4': no se generará fragmento 1080p RAM."; \
	fi

$(IMG): $(KERNEL) boot/limine.conf $(FRAGMENTO_1080P)
	@echo "==> Generando / Actualizando Imagen de Arranque UEFI FAT32 (128 MB)..."
	@if [ ! -f $(IMG) ] || [ $$(stat -c %s $(IMG) 2>/dev/null || echo 0) -gt 200000000 ] || [ $$(stat -c %s $(IMG) 2>/dev/null || echo 0) -lt 120000000 ]; then \
		rm -f $(IMG) && \
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
	@if [ -f "recursos/assets/Video 360p.mp4" ]; then \
		echo "==> Copiando Video 360p.mp4 a la imagen $(IMG)..."; \
		mcopy -o -i $(IMG) "recursos/assets/Video 360p.mp4" ::/boot/video_360p.mp4; \
	fi
	@if [ -f $(FRAGMENTO_1080P) ]; then \
		echo "==> Copiando fragmento 1080p RAM a la imagen $(IMG)..."; \
		mcopy -o -i $(IMG) $(FRAGMENTO_1080P) ::/boot/video_1080p.mp4; \
	fi
	@echo "==> Imagen $(IMG) lista y sincronizada!"

$(ISO): $(KERNEL) boot/limine.conf $(FRAGMENTO_1080P)
	@echo "==> Generando Imagen ISO Booteable UEFI/BIOS Híbrida..."
	@mkdir -p build/antigua
	@if ls $(BUILD_DIR)/taek-os-*.iso 1> /dev/null 2>&1; then \
		echo "==> Archivando compilaciones anteriores en build/antigua..."; \
		mv $(BUILD_DIR)/taek-os-*.iso build/antigua/; \
	fi
	@mkdir -p $(BUILD_DIR)/iso_root/boot/limine $(BUILD_DIR)/iso_root/EFI/BOOT
	@if [ -f $(FRAGMENTO_1080P) ]; then \
		echo "==> Copiando fragmento 1080p RAM a la ISO..."; \
		cp $(FRAGMENTO_1080P) $(BUILD_DIR)/iso_root/boot/video_1080p.mp4; \
	fi
	@cp boot/limine/limine-bios-cd.bin boot/limine/limine-bios.sys boot/limine/limine-uefi-cd.bin $(BUILD_DIR)/iso_root/boot/limine/
	@cp boot/limine/BOOTX64.EFI $(BUILD_DIR)/iso_root/EFI/BOOT/
	@FECHA_MENU=$$(date +'%Y-%m-%d_%H-%M-%S'); \
	sed "s|/TAEK OS|/TAEK OS ($${FECHA_MENU})|" boot/limine.conf > $(BUILD_DIR)/iso_root/boot/limine/limine.conf; \
	cp $(BUILD_DIR)/iso_root/boot/limine/limine.conf $(BUILD_DIR)/iso_root/limine.conf
	@cp $(KERNEL) $(BUILD_DIR)/iso_root/boot/nucleo.elf
	@if [ -f "recursos/assets/Video 360p.mp4" ]; then \
		echo "==> Copiando Video 360p.mp4 a la ISO..."; \
		cp "recursos/assets/Video 360p.mp4" $(BUILD_DIR)/iso_root/boot/video_360p.mp4; \
	fi
	@xorriso -as mkisofs -b boot/limine/limine-bios-cd.bin \
	        -no-emul-boot -boot-load-size 4 -boot-info-table \
	        --efi-boot boot/limine/limine-uefi-cd.bin \
	        -efi-boot-part --efi-boot-image --protective-msdos-label \
	        $(BUILD_DIR)/iso_root -o $@ > /dev/null 2>&1
	@if [ -f boot/limine/limine ]; then \
		boot/limine/limine bios-install $@ > /dev/null 2>&1; \
	fi
	@echo "==> Imagen ISO principal: $(ISO)"; \
	echo "==> Lista para grabar en USB con Rufus / Ventoy / Etcher!"

clean:
	@mkdir -p build/antigua
	@if ls $(BUILD_DIR)/taek-os-*.iso 1> /dev/null 2>&1; then \
		cp -u $(BUILD_DIR)/taek-os-*.iso build/antigua/; \
	fi
	rm -rf $(BUILD_DIR)

qemu: $(IMG)
	@echo "==> Lanzando QEMU con Intel HDA, controlador xHCI y teclado USB virtual..."
	@"/mnt/c/Program Files/qemu/qemu-system-x86_64.exe" \
		-drive if=pflash,format=raw,readonly=on,file="/mnt/c/Program Files/qemu/share/edk2-x86_64-code.fd" \
		-drive file="$(IMG)",format=raw \
		-m 512M \
		-M q35 \
		-audiodev dsound,id=snd0 \
		-device intel-hda \
		-device hda-output,audiodev=snd0 \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-serial stdio

qemu-trace: $(IMG)
	@echo "==> Lanzando QEMU con Intel HDA, xHCI, teclado USB y trazas activas..."
	@"/mnt/c/Program Files/qemu/qemu-system-x86_64.exe" \
		-drive if=pflash,format=raw,readonly=on,file="/mnt/c/Program Files/qemu/share/edk2-x86_64-code.fd" \
		-drive file="$(IMG)",format=raw \
		-m 512M \
		-M q35 \
		-audiodev dsound,id=snd0 \
		-device intel-hda \
		-device hda-output,audiodev=snd0 \
		-device qemu-xhci,id=xhci \
		-device usb-kbd,bus=xhci.0 \
		-trace "usb_xhci_*" \
		-serial stdio

# Arranque con tarjeta Intel e1000e (82574L) y red de usuario: DHCP en 10.0.2.x
qemu-red: $(IMG)
	@echo "==> Lanzando QEMU con Intel e1000e y red de usuario (DHCP + DNS + HTTP)..."
	@"/mnt/c/Program Files/qemu/qemu-system-x86_64.exe" \
		-drive if=pflash,format=raw,readonly=on,file="/mnt/c/Program Files/qemu/share/edk2-x86_64-code.fd" \
		-drive file="$(IMG)",format=raw \
		-m 1024M \
		-M q35 \
		-netdev user,id=red0 \
		-device e1000e,netdev=red0 \
		-serial stdio

$(filter $(BUILD_DIR)/nucleo/controladores/multimedia/%.o,$(OBJS)): $(MULTIMEDIA_HEADERS)

# Regenera docs/ESTRUCTURA_PROYECTO.md (metadatos e inventario autogenerado).
# Se ejecuta al cerrar cada hito terminado en 0 (70, 80, 90, 100, 110, ...).
estructura:
	@if [ -f herramientas/generar_estructura.py ]; then python3 herramientas/generar_estructura.py --os-root .; else echo "generar_estructura.py no disponible"; fi

.PHONY: all clean qemu qemu-trace qemu-red estructura
