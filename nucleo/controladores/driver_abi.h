#ifndef TAEK_DRIVER_ABI_H
#define TAEK_DRIVER_ABI_H
#include <stddef.h>
#include <stdint.h>
#define TAEK_DRIVER_ABI 1u
#define TAEK_DRIVER_MAGIA UINT64_C(0x315652444b454154) /* TAEKDRV1, little endian */
enum taek_driver_clase {TAEK_DRIVER_RED=1,TAEK_DRIVER_BLOQUE=2,TAEK_DRIVER_PERIFERICO=3,TAEK_DRIVER_VIDEO=4};
#define TAEK_CONTROL_SONDEAR 0x54414b01u
struct taek_hid_mouse_reporte {
    uint64_t secuencia;
    uint32_t dispositivo;
    uint8_t longitud,reservado[3],datos[8];
};
struct taek_driver_ops {
    uint32_t bytes,version;
    uint64_t capacidad;
    int (*leer)(uint64_t,void *,uint32_t);
    int (*escribir)(uint64_t,const void *,uint32_t);
    int (*control)(uint32_t,uint64_t);
};
struct taek_kernel_api {
    uint32_t bytes,version;
    void (*imprimir)(const char *);
    void (*imprimir_linea)(const char *);
    void (*imprimir_dec)(uint64_t);
    uint64_t (*obtener_ms)(void);
    void *(*asignar)(size_t);
    int (*liberar)(void *);
    int (*registrar)(const char *,uint32_t,const struct taek_driver_ops *);
    int (*retirar)(const char *);
    /* Optional appended services. Check bytes before using them. One owner,
     * BSP polling, no callback stored in the USB controller. */
    int (*hid_mouse_tomar)(void);
    int (*hid_mouse_leer)(struct taek_hid_mouse_reporte *);
    int (*hid_mouse_liberar)(void);
    int (*puntero_publicar)(int32_t,int32_t,uint32_t,uint32_t);
    /* Opcional: fibras cooperativas BSP, maximo ocho, TLS estatico del kernel.
     * Handles generacionales; el cargador retiene el modulo con fibras vivas.
     * crear/ejecutar/liberar son operaciones de raiz; yield/bloquear de fibra.
     * No se admiten IRQ, AP ni ejecucion reentrante. comprobar bytes primero. */
    int (*fibra_crear)(void (*)(void *),void *,uint64_t *);
    int (*fibra_ejecutar)(int);
    int (*fibra_yield)(void);
    int (*fibra_bloquear)(void);
    int (*fibra_despertar)(uint64_t);
    int (*fibra_liberar)(uint64_t);
    int (*fibra_estado)(uint64_t);
    uint64_t (*fibra_actual)(void);
    /* Opcional: identidad del consumidor antes de cambiar pila/FS, con CLI.
     * Solo raiz sin fibras vivas. Callback breve/infalible, sin cambiar FS/IF,
     * sin bloqueo ni llamadas mutantes al API. Destino cero significa raiz.
     * FX preservado; no es un callback de IRQ ni cubre NMI. */
    int (*fibra_observar)(void (*)(uint64_t,void *),void *);
    /* 0: rechazado; 1: callback ordinario BSP; 2: observer de transicion BSP.
     * Distingue raiz real de fibra_actual()==0 por rechazo de contexto. */
    int (*fibra_contexto)(void);
    /* Opcional: servicios del shim i915, solo raiz o fibra BSP ordinaria.
     * mapa_lineal_alias publica la misma traduccion HHDM en otra entrada PML4 alineada a
     * 512 GiB (Linux espera su mapa lineal en una direccion fija) y dura hasta reiniciar.
     * Las paginas son propiedad del modulo y se liberan al descargarlo. pci_leer es de solo
     * lectura; bytes=1,2,4. colapsar no vuelve: conserva evidencia y sigue la via del Huevo. */
    uint64_t (*hhdm_offset)(void);
    int (*mapa_lineal_alias)(uint64_t);
    uint64_t (*paginas_asignar)(uint32_t,uint64_t);
    int (*paginas_liberar)(uint64_t,uint32_t);
    uint32_t (*pci_leer)(uint8_t,uint8_t,uint8_t,uint8_t,uint8_t);
    void (*colapsar)(const char *);
    /* Opcional: acceso a registros. mmio_mapear solo admite BARs de memoria de la GPU 00:02.0,
     * sin cache; sin escritura la pagina es de solo lectura para la CPU y escribir provoca #PF.
     * Los mapeos son propiedad del modulo y se retiran al descargarlo. */
    int (*pci_barra)(uint8_t,uint8_t,uint8_t,uint32_t,uint64_t *,uint64_t *);
    uint64_t (*mmio_mapear)(uint64_t,uint64_t,int);
    int (*mmio_desmapear)(uint64_t,uint64_t);
    uint64_t (*ciclos_por_ms)(void);
    /* Opcional: ruta DMA de la GPU 00:02.0 leyendo ahora el estado de su unidad VT-d.
     * 1: traduccion inactiva, 0: traduccion activa, -1: desconocido. Nunca se presume. */
    int (*dma_directo_gpu)(void);
    /* Direccion fisica del framebuffer que entrego el firmware; 0 si no la hay. */
    uint64_t (*fb_fisica)(void);
    /* Paginas contiguas para objetos de la GPU, fuera del alcance de 32 bits; se liberan con
     * paginas_liberar. 0 si este equipo no tiene esa arena o no queda hueco. */
    uint64_t (*paginas_gpu_asignar)(uint32_t);
};
#define TAEK_KERNEL_API_BASE_BYTES offsetof(struct taek_kernel_api,hid_mouse_tomar)
#define TAEK_KERNEL_API_HID_BYTES offsetof(struct taek_kernel_api,fibra_crear)
#define TAEK_KERNEL_API_FIBRAS_BYTES (offsetof(struct taek_kernel_api,fibra_actual)+sizeof(((struct taek_kernel_api *)0)->fibra_actual))
#define TAEK_KERNEL_API_OBSERVADOR_BYTES (offsetof(struct taek_kernel_api,fibra_observar)+sizeof(((struct taek_kernel_api *)0)->fibra_observar))
#define TAEK_KERNEL_API_CONTEXTO_BYTES (offsetof(struct taek_kernel_api,fibra_contexto)+sizeof(((struct taek_kernel_api *)0)->fibra_contexto))
#define TAEK_KERNEL_API_SHIM_BYTES (offsetof(struct taek_kernel_api,colapsar)+sizeof(((struct taek_kernel_api *)0)->colapsar))
#define TAEK_KERNEL_API_MMIO_BYTES (offsetof(struct taek_kernel_api,ciclos_por_ms)+sizeof(((struct taek_kernel_api *)0)->ciclos_por_ms))
#define TAEK_KERNEL_API_DMA_BYTES (offsetof(struct taek_kernel_api,fb_fisica)+sizeof(((struct taek_kernel_api *)0)->fb_fisica))
#define TAEK_KERNEL_API_GPU_BYTES (offsetof(struct taek_kernel_api,paginas_gpu_asignar)+sizeof(((struct taek_kernel_api *)0)->paginas_gpu_asignar))
struct taek_driver {
    uint64_t magia;
    uint32_t bytes,version;
    char nombre[32];
    int (*iniciar)(const struct taek_kernel_api *);
    int (*comando)(const char *);
    /* Also called after a failed/partial iniciar. Must tolerate partial state.
     * Returns 0 only once all external work has ceased; a nonzero result keeps
     * the module code and resources resident, and permits a later stop retry. */
    int (*detener)(void);
};
#endif
