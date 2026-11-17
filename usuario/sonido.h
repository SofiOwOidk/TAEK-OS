/* H151: los sonidos del prototipo "Interfaz taek Os v1" (P5Audio, que alli usa Web Audio). Aqui se sintetizan en el
 * programa: cada efecto es una lista de voces (forma de onda, frecuencia que barre de f0 a f1, volumen que cae de g0 a g1)
 * que se mezclan en PCM de 16 bits estereo a 44 100 Hz y se encolan al nucleo con SYS_AUDIO, como la musica.
 * Reglas: nunca encima de la musica del escritorio (Musica en ventana), y un efecto nuevo corta al anterior.
 * Se incluye desde menu.c despues de escritorio.h (usa vm_sonando de ventanas.h). */

enum { SND_TICK, SND_BARRIDO, SND_REVERSO, SND_AERO, SND_TONO, SND_TAMBOR, SND_DISPARO, SND_GLITCH, SND_OLEADA,
       SND_LASER, SND_WARP, SND_CAD, SND_LINEA, SND_SIRENA, SND_N };
enum { F_SENO, F_TRIANGULO, F_SIERRA, F_CUADRADA, F_RUIDO };
struct voz { uint8_t forma, lineal; float t0, dur, f0, f1, g0, g1; };

/* Las recetas, copiadas de P5Audio: tiempos en segundos, frecuencias en Hz, volumen como en Web Audio */
static const struct voz v_tick[] = { { F_TRIANGULO, 0, 0, 0.035f, 800, 1900, 0.2f, 0.01f } };
static const struct voz v_barrido[] = { { F_SIERRA, 0, 0, 0.35f, 260, 30, 0.45f, 0.001f }, { F_SENO, 0, 0, 0.25f, 900, 3400, 0.3f, 0.01f } };
static const struct voz v_reverso[] = { { F_SIERRA, 0, 0, 0.15f, 650, 110, 0.25f, 0.01f } };
static const struct voz v_aero[] = { { F_SENO, 0, 0.00f, 0.55f, 523.25f, 523.25f, 0.2f, 0.001f }, { F_SENO, 0, 0.05f, 0.55f, 659.25f, 659.25f, 0.2f, 0.001f },
                                     { F_SENO, 0, 0.10f, 0.55f, 783.99f, 783.99f, 0.2f, 0.001f }, { F_SENO, 0, 0.15f, 0.55f, 1046.5f, 1046.5f, 0.2f, 0.001f } };
static const struct voz v_tono[] = { { F_SENO, 1, 0, 0.8f, 440, 440, 0.25f, 0.2f } };
static const struct voz v_tambor[] = { { F_TRIANGULO, 0, 0.000f, 0.035f, 800, 1900, 0.2f, 0.01f }, { F_TRIANGULO, 0, 0.045f, 0.035f, 800, 1900, 0.2f, 0.01f },
                                       { F_TRIANGULO, 0, 0.090f, 0.035f, 800, 1900, 0.2f, 0.01f }, { F_TRIANGULO, 0, 0.135f, 0.035f, 800, 1900, 0.2f, 0.01f },
                                       { F_TRIANGULO, 0, 0.180f, 0.035f, 800, 1900, 0.2f, 0.01f }, { F_TRIANGULO, 0, 0.225f, 0.035f, 800, 1900, 0.2f, 0.01f },
                                       { F_TRIANGULO, 0, 0.270f, 0.035f, 800, 1900, 0.2f, 0.01f } };
static const struct voz v_disparo[] = { { F_RUIDO, 0, 0, 0.4f, 850, 45, 0.85f, 0.001f } };   /* f: corte del filtro */
static const struct voz v_glitch[] = { { F_SIERRA, 1, 0, 0.12f, 140, 2200, 0.5f, 0.2f }, { F_SIERRA, 1, 0.12f, 0.18f, 2200, 50, 0.2f, 0.01f } };
static const struct voz v_oleada[] = { { F_SENO, 0, 0, 0.3f, 520, 1320, 0.35f, 0.001f }, { F_TRIANGULO, 0, 0.08f, 0.28f, 1040, 2080, 0.2f, 0.001f } };
static const struct voz v_laser[] = { { F_SIERRA, 0, 0, 0.2f, 1800, 220, 0.4f, 0.01f }, { F_SENO, 0, 0.05f, 0.27f, 440, 1760, 0.3f, 0.01f } };
static const struct voz v_warp[] = { { F_SENO, 0, 0.00f, 0.5f, 659.25f, 659.25f, 0.28f, 0.001f }, { F_SENO, 0, 0.06f, 0.5f, 830.61f, 830.61f, 0.28f, 0.001f },
                                     { F_SENO, 0, 0.12f, 0.5f, 987.77f, 987.77f, 0.28f, 0.001f }, { F_SENO, 0, 0.18f, 0.5f, 1318.51f, 1318.51f, 0.28f, 0.001f } };
static const struct voz v_cad[] = { { F_CUADRADA, 0, 0.00f, 0.04f, 1200, 1200, 0.18f, 0.08f }, { F_CUADRADA, 0, 0.04f, 0.04f, 2400, 2400, 0.08f, 0.035f },
                                    { F_CUADRADA, 0, 0.08f, 0.12f, 1600, 1600, 0.035f, 0.001f } };
static const struct voz v_linea[] = { { F_SIERRA, 0, 0, 0.45f, 480, 45, 0.45f, 0.001f } };
static const struct voz v_sirena[] = { { F_SIERRA, 0, 0.00f, 0.15f, 880, 880, 0.35f, 0.01f }, { F_SIERRA, 0, 0.16f, 0.15f, 660, 660, 0.35f, 0.01f },
                                       { F_SIERRA, 0, 0.32f, 0.15f, 880, 880, 0.35f, 0.01f }, { F_SIERRA, 0, 0.48f, 0.15f, 660, 660, 0.35f, 0.01f } };
#define VOCES(v) v, (int)(sizeof(v) / sizeof(v[0]))
static const struct { const struct voz *v; int n; } efectos[SND_N] = {
    { VOCES(v_tick) }, { VOCES(v_barrido) }, { VOCES(v_reverso) }, { VOCES(v_aero) }, { VOCES(v_tono) }, { VOCES(v_tambor) },
    { VOCES(v_disparo) }, { VOCES(v_glitch) }, { VOCES(v_oleada) }, { VOCES(v_laser) }, { VOCES(v_warp) }, { VOCES(v_cad) },
    { VOCES(v_linea) }, { VOCES(v_sirena) } };

/* Sin biblioteca matematica: logaritmo y exponencial lo bastante buenos para barrer frecuencias. */
static float ln_aprox(float x) {
    if (x <= 0) return -30.0f;
    int e = 0;
    while (x > 2.0f) { x *= 0.5f; e++; }
    while (x < 1.0f) { x *= 2.0f; e--; }
    float y = (x - 1.0f) / (x + 1.0f), y2 = y * y;
    return 2.0f * y * (1.0f + y2 / 3.0f + y2 * y2 / 5.0f + y2 * y2 * y2 / 7.0f) + (float)e * 0.69314718f;
}
static float exp_aprox(float x) {
    if (x < -30.0f) return 0;
    int n = 0;
    while (x > 0.5f) { x -= 0.69314718f; n++; }
    while (x < -0.5f) { x += 0.69314718f; n--; }
    float r = 1.0f + x * (1.0f + x * (0.5f + x * (1.0f / 6.0f + x * (1.0f / 24.0f + x / 120.0f))));
    while (n > 0) { r *= 2.0f; n--; }
    while (n < 0) { r *= 0.5f; n++; }
    return r;
}

static void sonido_bombear(void);
static int vm_sonando;   /* Declaracion tentativa: la definicion (con su -1) esta en ventanas.h, que se incluye despues */
static int16_t *snd_pcm;
static uint32_t snd_bytes, snd_i, snd_capacidad;
static struct taek_audio snd_au;
static int snd_activo = 1, snd_sonando, snd_iniciado, snd_sin_tarjeta, snd_ultimo = -1;
static long snd_t0;
static long snd_audio(uint32_t op) { snd_au.op = op; return taek_audio(&snd_au); }

static void sonido_callar(void) {
    if (snd_sonando) snd_audio(TAEK_AU_DETENER);
    snd_sonando = 0; snd_iniciado = 0; snd_bytes = snd_i = 0;
}

/* Mezcla las voces de uno o dos efectos en el bufer, desde cero. */
static uint32_t sonido_mezclar(int a, int b) {
    float fin = 0;
    for (int k = 0; k < 2; k++) {
        int e = k ? b : a;
        if (e < 0) continue;
        for (int i = 0; i < efectos[e].n; i++) { float f = efectos[e].v[i].t0 + efectos[e].v[i].dur; if (f > fin) fin = f; }
    }
    uint32_t frames = (uint32_t)(fin * (float)TAEK_AUDIO_HZ) + 64;
    if (frames > TAEK_AUDIO_HZ * 2) frames = TAEK_AUDIO_HZ * 2;
    /* Del monton y no estatico: un bufer grande en la imagen del programa ya rompio el arranque una vez (H140) */
    static float *mezcla;
    if (frames * 4 > snd_capacidad) {
        if (snd_pcm) free(snd_pcm);
        if (mezcla) free(mezcla);
        snd_capacidad = frames * 4;
        snd_pcm = (int16_t *)malloc(snd_capacidad);
        mezcla = (float *)malloc(frames * sizeof(float));
        if (!snd_pcm || !mezcla) { snd_capacidad = 0; return 0; }
    }
    memset(mezcla, 0, frames * sizeof(float));
    uint32_t ruido = 0x2545F491u;
    for (int k = 0; k < 2; k++) {
        int e = k ? b : a;
        if (e < 0) continue;
        for (int i = 0; i < efectos[e].n; i++) {
            const struct voz *v = &efectos[e].v[i];
            uint32_t ini = (uint32_t)(v->t0 * (float)TAEK_AUDIO_HZ), n = (uint32_t)(v->dur * (float)TAEK_AUDIO_HZ);
            if (!n) continue;
            /* Barridos exponenciales (como exponentialRampToValueAtTime) o lineales: un factor por muestra */
            float f = v->f0, g = v->g0, fase = 0, filtro = 0;
            float kf = v->lineal ? (v->f1 - v->f0) / (float)n : exp_aprox(ln_aprox(v->f1 / v->f0) / (float)n);
            float kg = v->lineal ? (v->g1 - v->g0) / (float)n : exp_aprox(ln_aprox(v->g1 / v->g0) / (float)n);
            for (uint32_t s = 0; s < n && ini + s < frames; s++) {
                float m;
                fase += f / (float)TAEK_AUDIO_HZ;
                fase -= (float)(int)fase;
                switch (v->forma) {
                case F_SENO: m = sen(fase * 6.2831853f - 3.14159265f); break;
                case F_TRIANGULO: m = fase < 0.5f ? 4.0f * fase - 1.0f : 3.0f - 4.0f * fase; break;
                case F_SIERRA: m = 2.0f * fase - 1.0f; break;
                case F_CUADRADA: m = fase < 0.5f ? 1.0f : -1.0f; break;
                default:   /* Ruido por un filtro de un polo; su corte barre como el de un BiquadFilter pasabajos */
                    ruido = ruido * 1664525u + 1013904223u;
                    m = (float)(int32_t)ruido / 2147483648.0f;
                    filtro += (m - filtro) * limitar01(6.2831853f * f / (float)TAEK_AUDIO_HZ);
                    m = filtro * 2.5f;
                    break;
                }
                mezcla[ini + s] += m * g;
                if (v->lineal) { f += kf; g += kg; } else { f *= kf; g *= kg; }
            }
        }
    }
    for (uint32_t s = 0; s < frames; s++) {
        float x = mezcla[s] * 26000.0f;
        if (x > 32000.0f) x = 32000.0f;
        if (x < -32000.0f) x = -32000.0f;
        snd_pcm[2 * s] = snd_pcm[2 * s + 1] = (int16_t)x;
    }
    return frames * 4;
}

static int sonido_ocupado_por_musica(void) { return vm_sonando >= 0; }

static void sonar2(int a, int b) {
    if (!snd_activo || snd_sin_tarjeta || sonido_ocupado_por_musica()) return;
    sonido_callar();
    snd_bytes = sonido_mezclar(a, b);
    if (!snd_bytes) return;
    snd_i = 0; snd_sonando = 1; snd_t0 = taek_ms(); snd_ultimo = a;
    sonido_bombear();
}
static void sonar(int e) { sonar2(e, -1); }

/* Cada cuadro: encola lo que quepa; arranca enseguida (los efectos son cortos) y se calla al vaciarse. */
static void sonido_bombear(void) {
    if (!snd_sonando) return;
    if (snd_i < snd_bytes) {
        snd_au.datos = (uint64_t)(uintptr_t)((uint8_t *)snd_pcm + snd_i);
        snd_au.bytes = snd_bytes - snd_i;
        if (snd_audio(TAEK_AU_ENCOLAR) < 0 || !snd_au.hay_audio) { snd_sin_tarjeta = 1; snd_sonando = 0; return; }
        snd_i += snd_au.aceptados;
    }
    if (!snd_iniciado) { snd_audio(TAEK_AU_INICIAR); snd_iniciado = 1; }
    if (snd_i >= snd_bytes) {
        snd_audio(TAEK_AU_ESTADO);
        if (!snd_au.ocupada || taek_ms() - snd_t0 > 3000) sonido_callar();
    }
}
static int sonido_suena(void) { return snd_sonando; }

/* Volumen del sistema (el del nucleo, real), en dB */
static int sonido_volumen(int delta) {
    snd_audio(TAEK_AU_ESTADO);
    snd_au.volumen_db += delta;
    if (delta) snd_audio(TAEK_AU_VOLUMEN);
    return snd_au.volumen_db;
}
