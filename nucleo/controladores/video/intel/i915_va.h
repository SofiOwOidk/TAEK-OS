#ifndef CONTROLADORES_VIDEO_INTEL_I915_VA_H
#define CONTROLADORES_VIDEO_INTEL_I915_VA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "controladores/multimedia/h264/h264.h"
#include "i915_gem.h"

#define I915_VA_MAX_REF_FRAMES 16
#define I915_VA_MAX_SURFACES   18

// Opcodes normativos de la tubería MFX de Intel (Gen 9 KBL / Gen 12 RPL)
#define MFX_OP_PIPE_MODE_SELECT       0x72000003U // Media Pipeline MFX Common, len 5-2 = 3
#define MFX_OP_SURFACE_STATE          0x72010004U // MFX Surface State, len 6-2 = 4
#define MFX_OP_PIPE_BUF_ADDR_STATE    0x72020017U // MFX Pipe Buf Addr State, len 25-2 = 23 (0x17)
#define MFX_OP_IND_OBJ_BASE_ADDR_STATE 0x72030009U // MFX Ind Obj Base Addr State, len 11-2 = 9
#define MFD_OP_AVC_PICID_STATE        0x72150008U // MFD AVC PicID State, len 10-2 = 8
#define MFD_OP_AVC_DPB_STATE          0x72160009U // MFD AVC DPB State, len 11-2 = 9
#define MFD_OP_AVC_SLICE_STATE        0x72100008U // MFD AVC Slice State, len 10-2 = 8
#define MFD_OP_AVC_BSD_OBJECT         0x72180004U // MFD AVC BSD Object, len 6-2 = 4

// Parámetros normalizados VA-API de cabecera de imagen H.264
typedef struct {
    uint32_t picture_width_in_mbs_minus1;
    uint32_t picture_height_in_mbs_minus1;
    uint8_t  bit_depth_luma_minus8;
    uint8_t  bit_depth_chroma_minus8;
    uint8_t  num_ref_frames;
    uint8_t  chroma_format_idc;
    uint8_t  residual_colour_transform_flag;
    uint8_t  gaps_in_frame_num_value_allowed_flag;
    uint8_t  frame_mbs_only_flag;
    uint8_t  mb_adaptive_frame_field_flag;
    uint8_t  direct_8x8_inference_flag;
    uint8_t  log2_max_frame_num_minus4;
    uint8_t  pic_order_cnt_type;
    uint8_t  log2_max_pic_order_cnt_lsb_minus4;
    uint8_t  delta_pic_order_always_zero_flag;
    int8_t   pic_init_qp_minus26;
    int8_t   pic_init_qs_minus26;
    int8_t   chroma_qp_index_offset;
    int8_t   second_chroma_qp_index_offset;
    uint8_t  entropy_coding_mode_flag;
    uint8_t  weighted_pred_flag;
    uint8_t  weighted_bipred_idc;
    uint8_t  transform_8x8_mode_flag;
    uint8_t  deblocking_filter_control_present_flag;
    uint8_t  constrained_intra_pred_flag;
    uint8_t  redundant_pic_cnt_present_flag;
} i915_va_pic_params_h264_t;

// Parámetros normalizados VA-API de slice H.264
typedef struct {
    uint32_t slice_data_size;
    uint32_t slice_data_offset;
    uint32_t slice_data_flag;
    uint8_t  slice_type;
    uint8_t  slice_qp_delta;
    uint8_t  disable_deblocking_filter_idc;
    int8_t   slice_alpha_c0_offset_div2;
    int8_t   slice_beta_offset_div2;
    uint8_t  num_ref_idx_l0_active_minus1;
    uint8_t  num_ref_idx_l1_active_minus1;
    uint8_t  cabac_init_idc;
    uint32_t frame_num;
    int32_t  curr_pic_order_cnt;
} i915_va_slice_params_h264_t;

// Estructura de superficie de video (NV12)
typedef struct {
    uint32_t id;
    uint32_t ancho;
    uint32_t alto;
    uint32_t pitch;
    uint64_t gpu_addr;
    void    *cpu_vaddr;
    struct drm_i915_gem_object *bo;
    int32_t  poc;
    int64_t  pts;
    bool     en_uso_dpb;
    bool     es_referencia;
} i915_va_superficie_t;

// Sesión del acelerador VA i915
typedef struct {
    bool     iniciada;
    uint32_t ancho;
    uint32_t alto;
    uint32_t ancho_mb;
    uint32_t alto_mb;
    uint32_t pitch;
    uint32_t tamano_cuadro_nv12;
    
    // DPB (Decoded Picture Buffer)
    i915_va_superficie_t superficies[I915_VA_MAX_SURFACES];
    uint32_t n_superficies;
    int32_t  idx_superficie_actual;
    int32_t  idx_superficie_salida;
    
    // Buffers de apoyo por hardware
    struct drm_i915_gem_object *batch_bo;
    struct drm_i915_gem_object *bitstream_bo;
    struct drm_i915_gem_object *rowstore_bo;
    struct drm_i915_gem_object *canary_bo;
    
    // Parámetros vigentes
    i915_va_pic_params_h264_t   pic_params;
    i915_va_slice_params_h264_t slice_params;
    
    // Métricas
    uint64_t cuadros_decodificados;
    uint64_t ciclos_sum_gpu;
    uint64_t fallos_hardware;
} i915_va_sesion_t;

// API del Backend Intel VA-API
int  i915_va_disponible(void);
int  i915_va_crear_sesion(uint32_t ancho, uint32_t alto, i915_va_sesion_t **sesion_out);
int  i915_va_destruir_sesion(i915_va_sesion_t *sesion);

// Desempaquetado y preparación de sintaxis
int  i915_va_extraer_rbsp(const uint8_t *nal_src, size_t nal_src_len, uint8_t *rbsp_dst, size_t rbsp_max_len, size_t *rbsp_len_out);
int  i915_va_parsear_nal(i915_va_sesion_t *s, const uint8_t *nal_buf, size_t nal_len, bool *es_slice);

// Decodificación y emisión al Video Command Streamer (VCS0)
int  i915_va_decodificar_cuadro(i915_va_sesion_t *s, const uint8_t *bitstream, size_t bitstream_len, int64_t pts);
int  i915_va_obtener_imagen(i915_va_sesion_t *s, h264_imagen *im_out);
void i915_va_drenar_dpb(i915_va_sesion_t *s);

// Conversión acelerada de superficie decodificada NV12 a RGB32
void i915_va_nv12_a_rgb32(const uint8_t *y_plane, const uint8_t *uv_plane,
                          uint32_t pitch, uint32_t ancho, uint32_t alto,
                          uint32_t *rgb_out, uint32_t rgb_pitch_pixels);

#endif // CONTROLADORES_VIDEO_INTEL_I915_VA_H
