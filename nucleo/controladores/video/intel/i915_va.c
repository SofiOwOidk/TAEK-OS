#include "i915_va.h"
#include "i915_drv.h"
#include "i915_gem.h"
#include "i915_vcs.h"
#include "arquitectura/x86_64/serial.h"
#include <string.h>

#ifndef TAEK_HOST_TEST
#undef GFP_KERNEL
#undef GFP_ATOMIC
#include "base/memoria.h"
#include "base/tiempo.h"
#else
#include <stdlib.h>
void *asignar_memoria(uint64_t n);
void  liberar_memoria(void *p);
uint64_t tiempo_obtener_milisegundos(void);
#endif

#define i915_va_kmalloc(_n) asignar_memoria(_n)
#define i915_va_kfree(_p)   liberar_memoria(_p)

// Estado del backend VA
int i915_va_disponible(void) {
#ifdef TAEK_HOST_TEST
    return 1;
#else
    const struct i915_dispositivo *dev = i915_obtener_dispositivo();
    return (dev && dev->estado == I915_ESTADO_VCS_LISTO);
#endif
}

// Creación de sesión acelerada por hardware (M17)
int i915_va_crear_sesion(uint32_t ancho, uint32_t alto, i915_va_sesion_t **sesion_out) {
    if (!sesion_out || ancho == 0 || alto == 0) return -EINVAL;
    *sesion_out = NULL;

    i915_va_sesion_t *s = (i915_va_sesion_t *)i915_va_kmalloc(sizeof(i915_va_sesion_t));
    if (!s) return -ENOMEM;
    memset(s, 0, sizeof(*s));

    s->ancho = ancho;
    s->alto = alto;
    s->ancho_mb = (ancho + 15) / 16;
    s->alto_mb = (alto + 15) / 16;

    // Regla de alineación GmmLib (Gen 9 KBL / Gen 12 RPL):
    // Pitch alineado a 128 bytes, altura alineada a 32 líneas
    s->pitch = (ancho + 127) & ~127U;
    uint32_t alto_alineado = (alto + 31) & ~31U;
    s->tamano_cuadro_nv12 = s->pitch * (alto_alineado + alto_alineado / 2);

    // Asignar superficies DPB (8 superficies de video NV12)
    s->n_superficies = 8;
    for (uint32_t i = 0; i < s->n_superficies; i++) {
        struct drm_i915_gem_object *bo = i915_gem_crear_objeto(s->tamano_cuadro_nv12);
        if (!bo) {
            i915_va_destruir_sesion(s);
            return -ENOMEM;
        }

        uint64_t gpu_addr = 0;
        int ret = i915_gem_pin_en_ggtt(bo, &gpu_addr);
        if (ret != 0) {
            i915_gem_liberar_objeto(bo);
            i915_va_destruir_sesion(s);
            return -ENOMEM;
        }

        s->superficies[i].id = i + 1;
        s->superficies[i].ancho = ancho;
        s->superficies[i].alto = alto;
        s->superficies[i].pitch = s->pitch;
        s->superficies[i].bo = bo;
        s->superficies[i].gpu_addr = gpu_addr;
        s->superficies[i].cpu_vaddr = bo->dir_cpu;
        s->superficies[i].en_uso_dpb = false;
        s->superficies[i].es_referencia = false;
    }

    // Asignar buffers auxiliares de la GPU (Batch, Bitstream, RowStore, Canario)
    s->batch_bo = i915_gem_crear_objeto(16 * 1024);     // 16 KiB Batch buffer
    s->bitstream_bo = i915_gem_crear_objeto(512 * 1024); // 512 KiB Bitstream buffer
    s->rowstore_bo = i915_gem_crear_objeto(64 * 1024);   // 64 KiB Scratch buffer
    s->canary_bo = i915_gem_crear_objeto(4096);          // 4 KiB Canario de sincronización

    if (!s->batch_bo || !s->bitstream_bo || !s->rowstore_bo || !s->canary_bo) {
        i915_va_destruir_sesion(s);
        return -ENOMEM;
    }

    uint64_t dummy;
    if (i915_gem_pin_en_ggtt(s->batch_bo, &dummy) != 0 ||
        i915_gem_pin_en_ggtt(s->bitstream_bo, &dummy) != 0 ||
        i915_gem_pin_en_ggtt(s->rowstore_bo, &dummy) != 0 ||
        i915_gem_pin_en_ggtt(s->canary_bo, &dummy) != 0) {
        i915_va_destruir_sesion(s);
        return -ENOMEM;
    }

    s->iniciada = true;
    s->idx_superficie_actual = 0;
    s->idx_superficie_salida = -1;

    *sesion_out = s;
    return 0;
}

// Destrucción limpia y garantizada de sesión (M15, M20, M22)
int i915_va_destruir_sesion(i915_va_sesion_t *s) {
    if (!s) return -EINVAL;

    // 1. Garantizar reposo absoluto del motor de video
    i915_vcs_esperar_quiescencia(100);

    // 2. Liberar superficies del DPB
    for (uint32_t i = 0; i < s->n_superficies; i++) {
        if (s->superficies[i].bo) {
            i915_gem_liberar_objeto(s->superficies[i].bo);
            s->superficies[i].bo = NULL;
        }
    }

    // 3. Liberar buffers auxiliares
    if (s->batch_bo)     { i915_gem_liberar_objeto(s->batch_bo);     s->batch_bo = NULL; }
    if (s->bitstream_bo) { i915_gem_liberar_objeto(s->bitstream_bo); s->bitstream_bo = NULL; }
    if (s->rowstore_bo)  { i915_gem_liberar_objeto(s->rowstore_bo);  s->rowstore_bo = NULL; }
    if (s->canary_bo)    { i915_gem_liberar_objeto(s->canary_bo);    s->canary_bo = NULL; }

    i915_va_kfree(s);
    return 0;
}

// Desempaquetador RBSP: elimina bytes de prevención de emulación 0x03 (M18)
int i915_va_extraer_rbsp(const uint8_t *nal_src, size_t nal_src_len, uint8_t *rbsp_dst, size_t rbsp_max_len, size_t *rbsp_len_out) {
    if (!nal_src || !rbsp_dst || !rbsp_len_out || nal_src_len == 0) return -EINVAL;

    size_t i = 0, j = 0;
    while (i < nal_src_len && j < rbsp_max_len) {
        if (i + 2 < nal_src_len && nal_src[i] == 0x00 && nal_src[i + 1] == 0x00 && nal_src[i + 2] == 0x03) {
            rbsp_dst[j++] = 0x00;
            rbsp_dst[j++] = 0x00;
            i += 3; // Saltar el byte 0x03
        } else {
            rbsp_dst[j++] = nal_src[i++];
        }
    }

    *rbsp_len_out = j;
    return 0;
}

// Lector de bits auxiliar para SPS/PPS/Slice Header
typedef struct {
    const uint8_t *datos;
    size_t         tamano;
    size_t         pos_byte;
    uint32_t       pos_bit;
} va_bitstream_t;

static uint32_t va_bits_leer(va_bitstream_t *b, uint32_t n) {
    uint32_t val = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (b->pos_byte >= b->tamano) return val;
        uint32_t bit = (b->datos[b->pos_byte] >> (7 - b->pos_bit)) & 1U;
        val = (val << 1) | bit;
        b->pos_bit++;
        if (b->pos_bit == 8) {
            b->pos_bit = 0;
            b->pos_byte++;
        }
    }
    return val;
}

static uint32_t va_bits_leer_ue(va_bitstream_t *b) {
    uint32_t ceros = 0;
    while (va_bits_leer(b, 1) == 0 && ceros < 32) {
        ceros++;
    }
    if (ceros == 0) return 0;
    if (ceros >= 32) return 0xFFFFFFFFU; // secuencia ue(v) invalida: saturar
    return (1U << ceros) - 1U + va_bits_leer(b, ceros);
}

static int32_t va_bits_leer_se(va_bitstream_t *b) {
    uint32_t ue = va_bits_leer_ue(b);
    int32_t val = (int32_t)((ue + 1) >> 1);
    if ((ue & 1) == 0) val = -val;
    return val;
}

// Analizador sintáctico de NALs y llenado de parámetros VA (M18)
int i915_va_parsear_nal(i915_va_sesion_t *s, const uint8_t *nal_buf, size_t nal_len, bool *es_slice) {
    if (!s || !nal_buf || nal_len < 2 || !es_slice) return -EINVAL;
    *es_slice = false;

    // Saltar start codes si vienen incluidos
    size_t offset = 0;
    if (nal_len >= 4 && nal_buf[0] == 0 && nal_buf[1] == 0 && nal_buf[2] == 0 && nal_buf[3] == 1) {
        offset = 4;
    } else if (nal_len >= 3 && nal_buf[0] == 0 && nal_buf[1] == 0 && nal_buf[2] == 1) {
        offset = 3;
    }

    uint8_t nal_header = nal_buf[offset];
    uint8_t nal_unit_type = nal_header & 0x1FU;

    // Extraer RBSP para el análisis sintáctico
    static uint8_t s_rbsp[8192];
    size_t rbsp_len = 0;
    i915_va_extraer_rbsp(nal_buf + offset + 1, nal_len - offset - 1, s_rbsp, sizeof(s_rbsp), &rbsp_len);

    va_bitstream_t bs;
    bs.datos = s_rbsp;
    bs.tamano = rbsp_len;
    bs.pos_byte = 0;
    bs.pos_bit = 0;

    if (nal_unit_type == 7) { // SPS (Sequence Parameter Set)
        uint32_t perfil = va_bits_leer(&bs, 8);
        va_bits_leer(&bs, 8); // restricciones
        uint32_t nivel = va_bits_leer(&bs, 8);
        (void)perfil; (void)nivel;

        va_bits_leer_ue(&bs); // seq_parameter_set_id
        if (perfil == 100 || perfil == 110 || perfil == 122 || perfil == 244) {
            uint32_t chroma = va_bits_leer_ue(&bs);
            if (chroma == 3) va_bits_leer(&bs, 1);
            va_bits_leer_ue(&bs); // bit_depth_luma_minus8
            va_bits_leer_ue(&bs); // bit_depth_chroma_minus8
            va_bits_leer(&bs, 1);  // qpprime_y_zero_transform_bypass_flag
            if (va_bits_leer(&bs, 1)) { // seq_scaling_matrix_present_flag
                for (int i = 0; i < ((chroma != 3) ? 8 : 12); i++) {
                    if (va_bits_leer(&bs, 1)) {
                        // Saltar lista de escalado
                        uint32_t lastScale = 8, nextScale = 8;
                        int sizeOfScalingList = (i < 6) ? 16 : 64;
                        for (int j = 0; j < sizeOfScalingList; j++) {
                            if (nextScale != 0) {
                                int32_t delta = va_bits_leer_se(&bs);
                                nextScale = (lastScale + delta + 256) % 256;
                            }
                            lastScale = (nextScale == 0) ? lastScale : nextScale;
                        }
                    }
                }
            }
        }

        s->pic_params.log2_max_frame_num_minus4 = (uint8_t)va_bits_leer_ue(&bs);
        s->pic_params.pic_order_cnt_type = (uint8_t)va_bits_leer_ue(&bs);
        if (s->pic_params.pic_order_cnt_type == 0) {
            s->pic_params.log2_max_pic_order_cnt_lsb_minus4 = (uint8_t)va_bits_leer_ue(&bs);
        } else if (s->pic_params.pic_order_cnt_type == 1) {
            s->pic_params.delta_pic_order_always_zero_flag = (uint8_t)va_bits_leer(&bs, 1);
            va_bits_leer_se(&bs); // offset_for_non_ref_pic
            va_bits_leer_se(&bs); // offset_for_top_to_bottom_field
            uint32_t num_cycle = va_bits_leer_ue(&bs);
            for (uint32_t k = 0; k < num_cycle; k++) va_bits_leer_se(&bs);
        }

        s->pic_params.num_ref_frames = (uint8_t)va_bits_leer_ue(&bs);
        s->pic_params.gaps_in_frame_num_value_allowed_flag = (uint8_t)va_bits_leer(&bs, 1);
        s->pic_params.picture_width_in_mbs_minus1 = (uint16_t)va_bits_leer_ue(&bs);
        s->pic_params.picture_height_in_mbs_minus1 = (uint16_t)va_bits_leer_ue(&bs);
        s->pic_params.frame_mbs_only_flag = (uint8_t)va_bits_leer(&bs, 1);
        s->pic_params.direct_8x8_inference_flag = (uint8_t)va_bits_leer(&bs, 1);
        s->pic_params.chroma_format_idc = 1; // 4:2:0 YUV
        return 0;
    }

    if (nal_unit_type == 8) { // PPS (Picture Parameter Set)
        va_bits_leer_ue(&bs); // pic_parameter_set_id
        va_bits_leer_ue(&bs); // seq_parameter_set_id
        s->pic_params.entropy_coding_mode_flag = (uint8_t)va_bits_leer(&bs, 1);
        va_bits_leer(&bs, 1); // bottom_field_pic_order_in_frame_present_flag
        va_bits_leer_ue(&bs); // num_slice_groups_minus1
        s->slice_params.num_ref_idx_l0_active_minus1 = (uint8_t)va_bits_leer_ue(&bs);
        s->slice_params.num_ref_idx_l1_active_minus1 = (uint8_t)va_bits_leer_ue(&bs);
        s->pic_params.weighted_pred_flag = (uint8_t)va_bits_leer(&bs, 1);
        s->pic_params.weighted_bipred_idc = (uint8_t)va_bits_leer(&bs, 2);
        s->pic_params.pic_init_qp_minus26 = (int8_t)va_bits_leer_se(&bs);
        s->pic_params.pic_init_qs_minus26 = (int8_t)va_bits_leer_se(&bs);
        s->pic_params.chroma_qp_index_offset = (int8_t)va_bits_leer_se(&bs);
        s->pic_params.deblocking_filter_control_present_flag = (uint8_t)va_bits_leer(&bs, 1);
        s->pic_params.constrained_intra_pred_flag = (uint8_t)va_bits_leer(&bs, 1);
        s->pic_params.redundant_pic_cnt_present_flag = (uint8_t)va_bits_leer(&bs, 1);
        return 0;
    }

    if (nal_unit_type == 1 || nal_unit_type == 5) { // Slice (IDR o No-IDR)
        va_bits_leer_ue(&bs); // first_mb_in_slice
        s->slice_params.slice_type = (uint8_t)va_bits_leer_ue(&bs);
        va_bits_leer_ue(&bs); // pic_parameter_set_id
        s->slice_params.frame_num = va_bits_leer(&bs, s->pic_params.log2_max_frame_num_minus4 + 4);

        if (s->pic_params.pic_order_cnt_type == 0) {
            s->slice_params.curr_pic_order_cnt = (int32_t)va_bits_leer(&bs, s->pic_params.log2_max_pic_order_cnt_lsb_minus4 + 4);
        }

        if (nal_unit_type == 5) {
            va_bits_leer_ue(&bs); // idr_pic_id
        }

        s->slice_params.slice_qp_delta = (uint8_t)va_bits_leer_se(&bs);
        if (s->pic_params.deblocking_filter_control_present_flag) {
            s->slice_params.disable_deblocking_filter_idc = (uint8_t)va_bits_leer_ue(&bs);
            if (s->slice_params.disable_deblocking_filter_idc != 1) {
                s->slice_params.slice_alpha_c0_offset_div2 = (int8_t)va_bits_leer_se(&bs);
                s->slice_params.slice_beta_offset_div2 = (int8_t)va_bits_leer_se(&bs);
            }
        }

        *es_slice = true;
        return 0;
    }

    return 0;
}

// Construcción del paquete MFX Batch y submission a hardware (M18, M19, M20)
int i915_va_decodificar_cuadro(i915_va_sesion_t *s, const uint8_t *bitstream, size_t bitstream_len, int64_t pts) {
    if (!s || !bitstream || bitstream_len == 0) return -EINVAL;

    // 1. Copiar bitstream al buffer GPU correspondiente
    if (bitstream_len > s->bitstream_bo->tamano) return -ENOMEM;
    memcpy(s->bitstream_bo->dir_cpu, bitstream, bitstream_len);

    // 2. Seleccionar superficie de destino en el DPB (Round-robin con respeto de referencias)
    uint32_t target_idx = s->idx_superficie_actual;
    i915_va_superficie_t *surf = &s->superficies[target_idx];
    surf->pts = pts;
    surf->poc = s->slice_params.curr_pic_order_cnt;
    surf->en_uso_dpb = true;

    // 3. Escribir comandos en el batch buffer
    volatile uint32_t *cmd = (volatile uint32_t *)s->batch_bo->dir_cpu;
    uint32_t dw = 0;

    // A. MFX_PIPE_MODE_SELECT (AVC, Decodificador VLD)
    cmd[dw++] = MFX_OP_PIPE_MODE_SELECT;
    cmd[dw++] = (1U << 0) /* AVC */ | (1U << 9) /* Post-Deblock Enable */;
    cmd[dw++] = 0;
    cmd[dw++] = 0;
    cmd[dw++] = 0;

    // B. MFX_SURFACE_STATE (NV12, Pitch, Dimensiones en macrobloques)
    cmd[dw++] = MFX_OP_SURFACE_STATE;
    cmd[dw++] = 0; // Surface ID = 0 (Destino)
    cmd[dw++] = (s->alto_mb << 19) | (s->ancho_mb << 4) | 4U /* NV12 Format */;
    cmd[dw++] = s->pitch;
    cmd[dw++] = 0;
    cmd[dw++] = 0;

    // C. MFX_PIPE_BUF_ADDR_STATE (Superficie destino y Rowstores)
    cmd[dw++] = MFX_OP_PIPE_BUF_ADDR_STATE;
    cmd[dw++] = 0; // Pre-deblock no usado
    cmd[dw++] = 0;
    cmd[dw++] = 0;
    cmd[dw++] = (uint32_t)(surf->gpu_addr & 0xFFFFFFFFU); // Post-deblock dest
    cmd[dw++] = (uint32_t)(surf->gpu_addr >> 32);
    cmd[dw++] = 0;
    cmd[dw++] = (uint32_t)(s->rowstore_bo->ggtt_offset & 0xFFFFFFFFU); // Intra RowStore
    cmd[dw++] = (uint32_t)(s->rowstore_bo->ggtt_offset >> 32);
    cmd[dw++] = 0;
    cmd[dw++] = (uint32_t)(s->rowstore_bo->ggtt_offset & 0xFFFFFFFFU); // Deblock RowStore
    cmd[dw++] = (uint32_t)(s->rowstore_bo->ggtt_offset >> 32);
    cmd[dw++] = 0;
    // Rellenar resto del estado de buffers de tubería hasta DW24
    while (dw < 10 + 25) {
        cmd[dw++] = 0;
    }

    // D. MFX_IND_OBJ_BASE_ADDR_STATE (Base del bitstream)
    cmd[dw++] = MFX_OP_IND_OBJ_BASE_ADDR_STATE;
    cmd[dw++] = (uint32_t)(s->bitstream_bo->ggtt_offset & 0xFFFFFFFFU);
    cmd[dw++] = (uint32_t)(s->bitstream_bo->ggtt_offset >> 32);
    while (dw < 35 + 11) {
        cmd[dw++] = 0;
    }

    // E. MFD_AVC_PICID_STATE
    cmd[dw++] = MFD_OP_AVC_PICID_STATE;
    cmd[dw++] = 0;
    while (dw < 46 + 10) {
        cmd[dw++] = 0;
    }

    // F. MFD_AVC_DPB_STATE
    cmd[dw++] = MFD_OP_AVC_DPB_STATE;
    while (dw < 56 + 11) {
        cmd[dw++] = 0;
    }

    // G. MFD_AVC_SLICE_STATE
    cmd[dw++] = MFD_OP_AVC_SLICE_STATE;
    cmd[dw++] = s->slice_params.slice_type;
    cmd[dw++] = 0;
    cmd[dw++] = (s->pic_params.pic_init_qp_minus26 + 26 + s->slice_params.slice_qp_delta) & 0x3FU;
    while (dw < 67 + 10) {
        cmd[dw++] = 0;
    }

    // H. MFD_AVC_BSD_OBJECT
    cmd[dw++] = MFD_OP_AVC_BSD_OBJECT;
    cmd[dw++] = (uint32_t)bitstream_len;
    cmd[dw++] = 0; // Offset dentro de indirect object
    while (dw < 77 + 6) {
        cmd[dw++] = 0;
    }

    // I. Canario de sincronización: MI_STORE_DWORD_IMM
    volatile uint32_t *canary_ptr = (volatile uint32_t *)s->canary_bo->dir_cpu;
    *canary_ptr = 0; // Limpiar canario antes del envío
    uint32_t canario_valor = 0x1915CAFEU + (uint32_t)s->cuadros_decodificados;

    cmd[dw++] = MI_STORE_DWORD_IMM;
    cmd[dw++] = (uint32_t)(s->canary_bo->ggtt_offset & 0xFFFFFFFFU);
    cmd[dw++] = (uint32_t)(s->canary_bo->ggtt_offset >> 32);
    cmd[dw++] = canario_valor;

    // J. Fin del batch buffer
    cmd[dw++] = MI_BATCH_BUFFER_END;

    // NOTA: el canario SOLO lo escribe la GPU. No se simula aquí: si no hay
    // hardware, la verificación posterior falla y se registra telemetría real.

    // 4. Enviar batch buffer al Video Command Streamer (VCS0)
    int ret = i915_vcs_enviar_batch(s->batch_bo->ggtt_offset, dw);
    if (ret != 0) {
        s->fallos_hardware++;
        return -EIO;
    }

    // 5. Esperar quiescencia del motor VCS
    ret = i915_vcs_esperar_quiescencia(100);
    if (ret != 0) {
        s->fallos_hardware++;
        serial_imprimir_linea("[I915_VA] ERROR: Timeout esperando finalización del fotograma en GPU.");
        return -ETIMEDOUT;
    }

    // 6. Verificar el canario de hardware
    if (*canary_ptr != canario_valor) {
        s->fallos_hardware++;
        serial_imprimir_linea("[I915_VA] ERROR: Firma canario no coincide tras ejecución GPU.");
        return -EIO;
    }

    s->cuadros_decodificados++;
    s->idx_superficie_salida = (int32_t)target_idx;
    s->idx_superficie_actual = (target_idx + 1) % s->n_superficies;

    return 0;
}

// Obtención de la última imagen decodificada (M21)
int i915_va_obtener_imagen(i915_va_sesion_t *s, h264_imagen *im_out) {
    if (!s || !im_out || s->idx_superficie_salida < 0) return -EINVAL;

    i915_va_superficie_t *surf = &s->superficies[s->idx_superficie_salida];
    uint32_t alto_alineado = (s->alto + 31) & ~31U;

    im_out->ancho = s->ancho;
    im_out->alto = s->alto;
    im_out->paso_y = s->pitch;
    im_out->paso_c = s->pitch;
    im_out->y = (const uint8_t *)surf->cpu_vaddr;
    im_out->u = (const uint8_t *)surf->cpu_vaddr + (s->pitch * alto_alineado);
    im_out->v = im_out->u + 1; // En NV12, U y V están entrelazados
    im_out->orden = surf->poc;
    im_out->marca_tiempo = surf->pts;
    im_out->rango_completo = 0;
    im_out->matriz_color = 2; // BT.709

    return 0;
}

// Drenaje completo del DPB
void i915_va_drenar_dpb(i915_va_sesion_t *s) {
    if (!s) return;
    for (uint32_t i = 0; i < s->n_superficies; i++) {
        s->superficies[i].en_uso_dpb = false;
        s->superficies[i].es_referencia = false;
    }
    s->idx_superficie_salida = -1;
}

// Conversión acelerada de superficie NV12 a RGB32 para el GOP (M22)
void i915_va_nv12_a_rgb32(const uint8_t *y_plane, const uint8_t *uv_plane,
                          uint32_t pitch, uint32_t ancho, uint32_t alto,
                          uint32_t *rgb_out, uint32_t rgb_pitch_pixels) {
    if (!y_plane || !uv_plane || !rgb_out) return;

    for (uint32_t j = 0; j < alto; j++) {
        const uint8_t *py = y_plane + (j * pitch);
        const uint8_t *puv = uv_plane + ((j >> 1) * pitch);
        uint32_t *prgb = rgb_out + (j * rgb_pitch_pixels);

        for (uint32_t i = 0; i < ancho; i++) {
            int y = py[i];
            int u = puv[(i & ~1U)] - 128;
            int v = puv[(i & ~1U) + 1] - 128;

            int c = y - 16;
            if (c < 0) c = 0;

            // Coeficientes estándar BT.709 entero
            int r = (298 * c + 409 * v + 128) >> 8;
            int g = (298 * c - 100 * u - 208 * v + 128) >> 8;
            int b = (298 * c + 516 * u + 128) >> 8;

            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;

            prgb[i] = (0xFFU << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}
