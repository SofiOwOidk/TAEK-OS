/* Contrato entre el reproductor del kernel y el cliente H.264 del modulo i915. El modulo registra una interfaz de clase
 * TAEK_DRIVER_VIDEO y el reproductor le llama por control(orden, peticion), solo desde la CPU coordinadora y fuera de un lote.
 * Sin simbolos compartidos: solo esta estructura cruza la frontera. */
#ifndef TAEK_VIDEO_ABI_H
#define TAEK_VIDEO_ABI_H
#include <stdint.h>

enum taek_video_orden {
    TAEK_VIDEO_PREPARAR = 1, /* Levanta la GPU y el driver VA una vez por arranque. Sin peticion. */
    TAEK_VIDEO_ABRIR,        /* datos/n: registro avcC. Devuelve ancho, alto y motivo si no es compatible. */
    TAEK_VIDEO_MUESTRA,      /* datos/n/pts/longitud_nal: una unidad de acceso del MP4. Devuelve listos. */
    TAEK_VIDEO_TOMAR,        /* Copia el siguiente fotograma, en orden de presentacion, a y/u/v. */
    TAEK_VIDEO_DRENAR,       /* Fin del flujo: todo lo pendiente pasa a listos. */
    TAEK_VIDEO_CERRAR,       /* Libera superficies y contexto. El driver sigue iniciado para el siguiente video. */
    /* H195: presentacion por Iris, sin copias. TOMAR_GPU da la superficie NV12 del siguiente fotograma (handle para Iris, sin
     * copiar pixeles); PRESENTAR_IRIS la dibuja escalada en el plano de pantalla (doble bufer, espera del vblank);
     * IRIS_FIN repone el plano del firmware y suelta lo importado. */
    TAEK_VIDEO_TOMAR_GPU,
    TAEK_VIDEO_PRESENTAR_IRIS,
    TAEK_VIDEO_IRIS_FIN
};

enum taek_video_resultado {
    TAEK_VIDEO_OK = 0,
    TAEK_VIDEO_OTRA_VEZ = 1,       /* Sin superficie libre: tomar listos y repetir la misma muestra. */
    TAEK_VIDEO_INCOMPATIBLE = -1,  /* El flujo pide algo que este cliente no hace. La GPU sigue sana. */
    TAEK_VIDEO_FALLO = -2          /* Fallo del driver o de la GPU. */
};

/* Inyeccion de fallos para las pruebas del respaldo por software. */
enum { TAEK_VIDEO_INYECTAR_ERROR = 1, TAEK_VIDEO_INYECTAR_BLOQUEO = 2 };

struct taek_video_peticion {
    uint32_t orden;
    int32_t rc;
    /* Entrada */
    const uint8_t *datos;
    uint64_t n;
    int64_t pts;
    uint32_t longitud_nal;
    uint32_t inyectar_tipo, inyectar_muestra;
    uint8_t *y, *u, *v;
    uint32_t paso_y, paso_c;
    /* Salida */
    uint32_t listos;
    uint32_t ancho, alto;
    int32_t rango_completo, matriz_color, poc;
    int64_t pts_salida;
    uint64_t ns_gpu, ns_copia, decodificados, cortes;
    char motivo[96];
    /* H195: superficie para Iris (salida de TOMAR_GPU) y rectangulo destino en pantalla (entrada de PRESENTAR_IRIS; ancho 0 =
     * pantalla completa conservando la proporcion). */
    uint32_t iris_handle, iris_pitch, iris_off_y, iris_off_uv, recorte_x, recorte_y;
    uint64_t iris_bytes, iris_modificador;
    int32_t dest_x, dest_y, dest_ancho, dest_alto;
    uint64_t ns_iris;
    /* H196: capa del programa (OSD) para PRESENTAR_IRIS. ARGB premultiplicado, osd_w x osd_h en (osd_x, osd_y) de pantalla.
     * Iris la vuelve a subir solo si cambia osd_version. */
    const uint32_t *osd_px;
    uint32_t osd_x, osd_y, osd_w, osd_h, osd_version;
    int32_t osd_visible;
};
#endif
