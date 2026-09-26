#include "aac.h"
#include "aacdec.h"
#include "aac_memoria.h"

struct aac_decodificador {
    HAACDecoder dec;
    aac_servicios serv;
    int canales;
    int frecuencia;
};

static aac_servicios g_servicios_actuales;

static void *asignar_puente(size_t bytes) {
    if (g_servicios_actuales.asignar) {
        return g_servicios_actuales.asignar(g_servicios_actuales.usuario, bytes);
    }
    return 0;
}

static void liberar_puente(void *ptr) {
    if (g_servicios_actuales.liberar) {
        g_servicios_actuales.liberar(g_servicios_actuales.usuario, ptr);
    }
}

aac_decodificador *aac_crear(const aac_servicios *servicios) {
    if (!servicios || !servicios->asignar || !servicios->liberar) return 0;

    g_servicios_actuales = *servicios;
    aac_fijar_asignador(asignar_puente, liberar_puente);

    aac_decodificador *d = servicios->asignar(servicios->usuario, sizeof(*d));
    if (!d) return 0;

    d->serv = *servicios;
    d->canales = 2;
    d->frecuencia = 44100;
    d->dec = AACInitDecoder();
    if (!d->dec) {
        servicios->liberar(servicios->usuario, d);
        return 0;
    }

    return d;
}

void aac_destruir(aac_decodificador *d) {
    if (!d) return;
    g_servicios_actuales = d->serv;
    aac_fijar_asignador(asignar_puente, liberar_puente);

    if (d->dec) AACFreeDecoder(d->dec);
    d->serv.liberar(d->serv.usuario, d);
}

int aac_configurar(aac_decodificador *d, int canales, int frecuencia_muestreo) {
    if (!d || !d->dec) return -1;
    d->canales = canales;
    d->frecuencia = frecuencia_muestreo;

    AACFrameInfo info;
    uint8_t *p = (uint8_t *)&info;
    for (size_t i = 0; i < sizeof(info); i++) p[i] = 0;
    info.nChans = canales;
    info.sampRateCore = frecuencia_muestreo;
    info.profile = AAC_PROFILE_LC;

    return AACSetRawBlockParams(d->dec, 0, &info);
}

int aac_decodificar(aac_decodificador *d, const uint8_t **datos, int *bytes_restantes, int16_t *pcm_salida) {
    if (!d || !d->dec || !datos || !*datos || !bytes_restantes || *bytes_restantes <= 0 || !pcm_salida) {
        return -1;
    }

    g_servicios_actuales = d->serv;
    aac_fijar_asignador(asignar_puente, liberar_puente);

    unsigned char *inptr = (unsigned char *)*datos;
    int err = AACDecode(d->dec, &inptr, bytes_restantes, (short *)pcm_salida);
    *datos = inptr;

    if (err != ERR_AAC_NONE) {
        return err;
    }

    AACFrameInfo info;
    AACGetLastFrameInfo(d->dec, &info);
    return info.outputSamps;
}
