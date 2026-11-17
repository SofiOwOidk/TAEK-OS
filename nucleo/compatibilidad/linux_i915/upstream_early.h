#ifndef TAEK_UPSTREAM_EARLY_H
#define TAEK_UPSTREAM_EARLY_H
/* Frontera TaekOS bajo el probe original de i915. Tipos escalares para el consumidor.
 * Perfil: UP, PREEMPT_NONE, BSP cooperativo; IRQ por sondeo en raiz, sin AP. Hasta donde se llega
 * lo fija taek_early_modo. Lo que no tiene servicio es una trampa fatal, nunca un exito simulado. */
#define TAEK_EARLY_VERSION 1u
struct taek_early_ops {
    unsigned bytes,version;
    unsigned long (*paginas_asignar)(unsigned,unsigned long);
    int (*paginas_liberar)(unsigned long,unsigned);
    unsigned (*pci_leer)(unsigned char,unsigned char,unsigned char,unsigned char,unsigned char);
    void (*mensaje)(const char *);
    void (*colapsar)(const char *);
    int (*pci_barra)(unsigned char,unsigned char,unsigned char,unsigned,unsigned long *,unsigned long *);
    unsigned long (*mmio_mapear)(unsigned long,unsigned long,int);
    int (*mmio_desmapear)(unsigned long,unsigned long);
    unsigned long (*ciclos_por_ms)(void);
    int (*dma_directo_gpu)(void);
    unsigned long (*fb_fisica)(void);
    /* Objetos GEM: admiten mas de 32 bits. 0 si no hay esa arena; entonces se usa paginas_asignar. */
    unsigned long (*paginas_objeto_asignar)(unsigned);
    /* Opcional (H192): copia al framebuffer real (ver taek_kernel_api.pantalla_copiar). NULL con un kernel anterior. */
    int (*pantalla_copiar)(const void *,unsigned,unsigned,unsigned,int,int);
};
/* Solo raiz, antes de usar memoria o PCI. */
int taek_early_frontera_iniciar(const struct taek_early_ops *);
/* Identidad solo para QEMU: el nombre deja claro que no es la GPU real. */
struct taek_early_resultado {
    long fase,rc;
    unsigned vendor,device,revision,simulada;
    unsigned platform,subplatform,step_gt,pch_tipo,pch_id,gt_count,mem_regiones_ok;
    unsigned long avisos,trampas,paginas,paginas_pico,devres,registros;
    unsigned modo,mmio_fallos,mapeos,hrtimers;
    unsigned long dma_mapeados,dma_rechazados;
};
/* Hasta donde llega la secuencia del probe. Ninguno hace reset ni submission. */
enum taek_early_modo {
    TAEK_EARLY_SOLO_EARLY,    /* create + early_probe: sin registros */
    TAEK_EARLY_MMIO_LECTURA,  /* + BAR0 sin permiso de escritura: solo lecturas */
    TAEK_EARLY_MMIO_FW,       /* + uncore y forcewake originales: primera escritura */
    TAEK_EARLY_MMIO_INFO,     /* + fusibles, reloj y motores: hasta justo antes de sanitize_gpu */
    TAEK_EARLY_MMIO_RESET,    /* + reset de motores de sanitize_gpu: fin de i915_driver_mmio_probe */
    TAEK_EARLY_GGTT_LECTURA,  /* Etapa A mas las PTE de la GGTT, sin permiso de escritura */
    TAEK_EARLY_GGTT_PROBE,    /* Secuencia completa + hw_probe hasta antes de i915_ggtt_init_hw */
    TAEK_EARLY_HW_PROBE,      /* + resto de hw_probe: fences a cero, memoria robada, sin escribir PTE */
    TAEK_EARLY_GEM_GGTT,      /* + i915_gem_init hasta i915_init_ggtt: primeras PTE, pantalla reservada */
    TAEK_EARLY_GT,            /* Sin display + IRQ por sondeo + i915_gem_init completo: trabajos reales */
    TAEK_EARLY_GT_HOLA,       /* Repite la escritura de cada motor sobre el dispositivo retenido */
    TAEK_EARLY_GT_MAL,        /* Control negativo: el motor escribe otro valor y debe detectarse */
    TAEK_EARLY_GT_GUARDA,     /* Control negativo: el motor escribe fuera de su destino y debe detectarse */
    TAEK_EARLY_MEDIA_RT,      /* Constructores de Media Driver y autoprueba del runtime C++ */
    TAEK_EARLY_MEDIA_INIT,    /* Inicializa el driver VA real sobre el dispositivo retenido */
    TAEK_EARLY_MEDIA_CAPS,    /* Perfiles, punto de entrada, perfil ausente y superficies */
    TAEK_EARLY_MEDIA_IDR,     /* Primer fotograma real en la GPU, comparado con la referencia independiente */
    TAEK_EARLY_MEDIA_MAL,     /* Control negativo: flujo con un byte cambiado; debe detectarse la diferencia */
    TAEK_EARLY_MEDIA_VIDEO,   /* 300 fotogramas del video de 1080p */
    TAEK_EARLY_MEDIA_OP,      /* Una orden del reproductor: abrir, muestra, tomar, drenar o cerrar */
    TAEK_EARLY_GT_CARGA,      /* Miles de escrituras seguidas y espera a que la GPU quede ociosa */
    TAEK_EARLY_GT_LOTE,       /* Batch buffer en contexto propio con PPGTT, por motor */
    TAEK_EARLY_GT_CUELGUE,    /* Batch en bucle por motor: debe detectarlo y resetearlo i915 */
    TAEK_EARLY_GT_CERRAR,     /* Desmontaje original del dispositivo: despues se puede reabrir */
    TAEK_EARLY_IRIS_RT,       /* M27: constructores de Iris (Mesa) */
    TAEK_EARLY_IRIS_INIT,     /* M27: pantalla y contexto Gallium de Iris sobre el dispositivo retenido */
    TAEK_EARLY_IRIS_CLEAR,    /* M27: dos clear por el motor de render, leidos y comparados por la CPU */
    TAEK_EARLY_IRIS_TRIANGULO,/* M28: triangulo con shaders NIR propios, comparado con una rasterizacion de la CPU */
    TAEK_EARLY_IRIS_TEXTURA,  /* M28: textura muestreada (verificada) y animacion presentada en la pantalla real */
    TAEK_EARLY_IRIS_PANTALLA, /* H193: el plano de pantalla escanea una superficie de Iris (sin copia) y se restaura */
    TAEK_EARLY_IRIS_VIDEO,    /* H194: Iris importa el NV12 de Media Driver por handle y lo convierte a RGB en un shader */
    TAEK_EARLY_MEDIA_EXPONER, /* H194: Media Driver decodifica el IDR del vector y deja su superficie a Iris */
    TAEK_EARLY_IRIS_OP        /* H195: una orden del reproductor para Iris (presentar un fotograma, terminar) */
};
/* Solo identidad simulada: ruta DMA declarada directa para ejercitar el codigo. Sin ella, QEMU
 * no tiene DMAR, la ruta es desconocida y el probe debe fallar cerrado. */
void taek_early_dma_simulada(int directo);
enum taek_early_fase {
    TAEK_EARLY_NADA,TAEK_EARLY_WQ,TAEK_EARLY_DRM_CORE,TAEK_EARLY_MODULO,TAEK_EARLY_MATCH,
    TAEK_EARLY_CREATE,TAEK_EARLY_PROBE,TAEK_EARLY_RELEASE,TAEK_EARLY_DEVRES,TAEK_EARLY_FIN,
    TAEK_EARLY_ETAPA_A,TAEK_EARLY_ETAPA_B,TAEK_EARLY_ETAPA_C,TAEK_EARLY_ETAPA_D,TAEK_EARLY_ETAPA_E,TAEK_EARLY_ETAPA_F,TAEK_EARLY_ETAPA_G,TAEK_EARLY_ETAPA_H
};
/* Solo fibra supervisora. simular_5917 solo se admite si 00:02.0 no es una GPU Intel.
 * fallo_n distinto de cero hace fallar esa asignacion dentro de create/early_probe. */
int taek_early_ejecutar(int modo,int simular_5917,int soltar,unsigned long fallo_n);
void taek_early_media_peticion(void *peticion);
void taek_early_resultado_leer(struct taek_early_resultado *);
void taek_early_bombear(void);
#endif
