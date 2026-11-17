#ifndef TAEK_ANILLO3_ABI_H
#define TAEK_ANILLO3_ABI_H
/* Contrato entre el nucleo y los programas de Ring 3: lo incluyen ambos lados. */
#include <stdint.h>

enum {
    SYS_SALIR = 0,
    SYS_ESCRIBIR = 1,
    SYS_TIEMPO = 2,
    SYS_CEDER = 3,
    SYS_SUPERFICIE = 4,  /* rdi: struct taek_superficie*        */
    SYS_PRESENTAR = 5,   /* rdi: id, rsi: x, rdx: y, rcx: ancho, r8: alto (dentro de la superficie) */
    SYS_MOVER = 6,       /* rdi: id, rsi: x, rdx: y (en pantalla) */
    SYS_EVENTO = 7,      /* rdi: struct taek_evento*; devuelve 1 si hay tecla nueva */
    SYS_PANTALLA = 8,    /* rdi: struct taek_pantalla*          */
    SYS_ESPERAR = 9,     /* rdi: milisegundos, hasta 100; sigue atendiendo audio y red */
    SYS_INFO = 10,       /* rdi: struct taek_info*, rsi: 1 para leer tambien la bateria */
    SYS_CAPTURA = 11,    /* rdi: nombre, rsi: largo. Solo si el nucleo lanzo el programa en modo prueba */
    SYS_EQUIPO = 12,     /* rdi: struct taek_equipo*; lee tambien la bateria, puede tardar decenas de ms */
    SYS_ELEVAR = 13,     /* rdi: id; la superficie pasa delante de todas las demas */
    SYS_WEB = 14,        /* rdi: struct taek_web*; descarga una pagina con el cliente HTTP/HTTPS del nucleo (H143) */
    SYS_MEMORIA = 15,    /* rdi: bytes (con signo). Mueve el final del monton del programa, como sbrk de Unix (H144) */
    SYS_ARCHIVOS = 16,   /* rdi: struct taek_archivos*; discos, montar, listar y leer (H145, solo lectura) */
    SYS_LANZAR = 17,     /* rdi: struct taek_lanzar*; al salir este programa, el nucleo abre otro (H145) */
    SYS_ARGUMENTO = 18,  /* rdi: bufer, rsi: capacidad; copia el argumento con el que se abrio este programa (H145) */
    SYS_AUDIO = 19,      /* rdi: struct taek_audio*; sonido PCM hacia la tarjeta (H146) */
    SYS_TAMANO = 20,     /* rdi: id, rsi: ancho, rdx: alto, rcx: x, r8: y. Tamano visible y posicion juntos (H148) */
    SYS_SENSORES = 21,   /* rdi: struct taek_sensores*; temperatura, red, stream LAN, monton del nucleo (H150) */
    SYS_ORDEN = 22,      /* rdi: struct taek_orden*; corre una orden de texto de la terminal y devuelve lo que imprimio (H151) */
    SYS_RED = 23,        /* rdi: struct taek_red*; sockets TCP y TLS 1.3 para el programa (H183) */
    SYS_VIDEO = 24,      /* rdi: datos, rsi: bytes. Entrega un MP4 al nucleo para TAEK_PROGRAMA_ABRIR "mem:..." (H185) */
    SYS_FLUJO = 25,      /* rdi: struct taek_flujo*. Decodificacion H.264 + AAC en flujo para el programa (H187) */
    SYS_HILO = 26,       /* rdi: TAEK_HILO_*, rsi: argumento. Hilos del programa en las CPU 1 a 3 (H213, Fase D) */
    SYS_FUTEX = 27,      /* rdi: TAEK_FUTEX_*, rsi: direccion (uint32_t), rdx: valor o cuantos, rcx: ms (H213) */
};

/* H213, Fase D del gestor de hilos: hilos del programa.
 *  - El hilo principal sigue en la CPU 0. Los demas (hasta TAEK_HILOS_MAX) corren en las CPU 1 a 3, comparten la
 *    memoria del programa y el nucleo los desaloja con su tictac de 1 ms. Prioridad de fondo: los hilos de servicio
 *    del nucleo (USB, audio, red, lotes de video) siempre pasan antes.
 *  - Desde un hilo secundario solo valen SYS_SALIR (termina ese hilo), SYS_ESCRIBIR, SYS_TIEMPO, SYS_CEDER,
 *    SYS_ESPERAR, SYS_HILO y SYS_FUTEX. Lo demas (pantalla, archivos, red, audio, memoria...) devuelve
 *    -TAEK_ENOPERMITIDA: esos subsistemas los usa solo el principal. malloc de usuario/memoria.h no es seguro entre
 *    hilos: pedir la memoria (pilas incluidas) desde el principal.
 *  - Un fallo en cualquier hilo termina el programa entero. Al terminar el principal, el nucleo detiene a los demas
 *    antes de soltar la memoria. */
#define TAEK_HILOS_MAX 8
#define TAEK_HILO_CREAR 1    /* rsi: struct taek_hilo*; devuelve el id (1 a TAEK_HILOS_MAX). Solo el principal */
#define TAEK_HILO_UNIR 2     /* rsi: struct taek_hilo* (id y limite_ms; devuelve fin, valor...). 0 o -TAEK_ETIEMPO */
#define TAEK_HILO_SALIR 3    /* rsi: valor. Termina el hilo que llama (en el principal es SYS_SALIR) */
#define TAEK_HILO_YO 4       /* Devuelve el id del hilo que llama (0 el principal) */
#define TAEK_HILO_CPU 5      /* Devuelve la CPU en la que corre el que llama */
#define TAEK_HILO_TLS 6      /* rsi: base de FS (TLS) del hilo que llama; 0 la quita */
#define TAEK_HILO_INFO 7     /* rsi: struct taek_hilos_info* */
#define TAEK_HILO_FIN_SALIO 1
#define TAEK_HILO_FIN_FALLO 2
#define TAEK_HILO_FIN_DETENIDO 3   /* Lo detuvo el nucleo porque el programa terminaba */
struct taek_hilo {
    uint64_t entrada;      /* CREAR: rdi = arg y rsi = id al entrar. No debe volver: termina con TAEK_HILO_SALIR */
    uint64_t arg;          /* CREAR */
    uint64_t pila;         /* CREAR: final (direccion mas alta) de su pila, alineado a 16 */
    uint64_t tls;          /* CREAR: base de FS del hilo, 0 sin TLS */
    uint32_t cpu;          /* CREAR: 1 a 3; 0 deja elegir al nucleo. Salida: la CPU elegida */
    uint32_t id;           /* UNIR: entrada */
    uint32_t limite_ms;    /* UNIR: 0 sin limite (en el principal, cada llamada espera como mucho 100 ms) */
    int32_t fin;           /* UNIR: TAEK_HILO_FIN_* */
    int64_t valor;         /* UNIR: lo que paso a TAEK_HILO_SALIR */
    uint64_t vector, rip, cr2;   /* UNIR: si fin es TAEK_HILO_FIN_FALLO */
};
struct taek_hilos_info {
    uint32_t vivos, creados;       /* Hilos secundarios de este programa */
    uint32_t servicios_cpu0;       /* Veces que el reloj de CPU 0 atendio el puntero con el programa sin llamar al sistema */
    uint32_t hueco_cpu0_max_ms;    /* Mayor tiempo sin atender el puntero mientras el programa estaba en Anillo 3 */
    uint64_t llamadas_hilos;       /* Llamadas al sistema desde hilos secundarios */
    uint64_t rechazadas;           /* De esas, las no permitidas fuera del principal */
    uint64_t futex_esperas, futex_despertados;
};
#define TAEK_FUTEX_ESPERAR 1     /* Duerme si *dir == valor; 0 al despertar, -TAEK_EAGAIN si ya no valia, -TAEK_ETIEMPO */
#define TAEK_FUTEX_DESPERTAR 2   /* Despierta a lo sumo rdx hilos que esperan en dir; devuelve cuantos */

/* H151: la Terminal, el Oraculo y los paneles de la interfaz corren ordenes reales de la terminal del nucleo. Solo las de
 * una lista cerrada que unicamente imprimen; lo que imprimen vuelve al programa, con los cambios de color marcados como
 * 0x01 R G B (un canal 0 viaja como 1, para que el texto nunca lleve un byte 0). Las que se adueñan de la pantalla (ruleta, cangrejo, estres...) se piden con SYS_LANZAR y
 * TAEK_PROGRAMA_ORDEN: el menu termina con TAEK_ACCION_ORDEN, el nucleo la corre en la consola y vuelve al menu. */
#define TAEK_ORDEN_LINEA 256
#define TAEK_EPANTALLA 77      /* La orden existe pero necesita la pantalla completa */
#define TAEK_ENOPERMITIDA 78   /* No esta en la lista de la interfaz */
struct taek_orden {
    char linea[TAEK_ORDEN_LINEA];  /* Entrada */
    uint64_t destino;              /* Entrada: bufer para el texto */
    uint32_t capacidad;            /* Entrada */
    uint32_t bytes;                /* Salida */
    uint32_t truncado, ms;         /* Salida: 1 si no cupo todo; cuanto tardo */
};
#define TAEK_PROGRAMA_ORDEN "taek:orden"

/* H150: lo que el panel de Configuracion muestra en vivo. Todo sale de registros o contadores reales; si el equipo no
 * tiene un dato, su bandera *_ok queda en 0 y el programa debe decir "sin dato", nunca inventarlo. Sin direcciones del
 * nucleo: un programa no las necesita. */
#define TAEK_SENSORES_VERSION 3   /* H151: 2 agrega carga por CPU y frecuencia efectiva al final; lo anterior no cambia */
#define TAEK_SEN_LEER_VOLUMEN 1u   /* op: ademas, leer ahora la region de arranque del volumen exFAT abierto (lento) */
/* H204: la version 3 crece al final. Un programa compilado antes tiene la estructura de la version 2 y el nucleo no
 * puede escribir mas alla: solo se entrega la version 3 a quien pone TAEK_SEN_V3 en op. Sin ese bit, version 2. */
#define TAEK_SEN_CERO_LATENCIA 2u  /* op: despues de leer, poner a cero los maximos y contadores de la version 3 */
#define TAEK_SEN_V3 0x100u         /* op: la estructura del programa es la de la version 3 */
struct taek_suceso_puerto {        /* Un suceso de un puerto raiz del xHCI (H204) */
    uint64_t ms;                   /* Milisegundos desde el arranque */
    uint32_t portsc;               /* PORTSC tal cual: bit 0 CCS, 1 PED, 3 OCA, 8:5 PLS, 17 CSC, 18 PEC, 20 OCC, 21 PRC, 22 PLC */
    uint8_t puerto;                /* 1 a 32 */
    uint8_t tipo;                  /* 1 evento de cambio, 2 conexion, 3 desconexion, 4 sin enlace, 5 recuperado,
                                    * 6 reintento fallido, 7 dado por perdido */
    uint8_t intento, reservado;    /* Numero de reintento (0 si no lo es) */
};
struct taek_hueco {                /* Un hueco de atencion de 250 ms o mas (H204) */
    uint64_t fin_ms;               /* Cuando se cerro, en ms desde el arranque */
    uint32_t ms;                   /* Cuanto duro */
    uint8_t que;                   /* 1 USB, 2 red */
    uint8_t sys_ini, sys_fin;      /* SYS_* en curso al abrirse y al cerrarse; 255 = ninguna */
    uint8_t reservado;
};
struct taek_sensores {
    uint32_t version;              /* Salida: TAEK_SENSORES_VERSION */
    uint32_t op;                   /* Entrada: 0 o TAEK_SEN_LEER_VOLUMEN */
    /* Temperatura por MSR (IA32_THERM_STATUS 0x19C del nucleo que atiende la llamada, IA32_PACKAGE_THERM_STATUS 0x1B1,
     * MSR_TEMPERATURE_TARGET 0x1A2). Solo Intel con sensor digital (CPUID 06h). */
    uint32_t temp_ok, temp_paquete_ok, tjmax_leido;
    int32_t tjmax_c, temp_nucleo_c, temp_paquete_c;
    uint32_t termico_estado, paquete_estado;   /* Los 16 bits bajos de cada MSR, tal cual */
    /* Red: contadores del controlador */
    uint32_t red_presente, red_enlace, red_mbps, red_duplex;
    uint16_t red_proveedor, red_dispositivo;
    uint8_t mac[6], por_dhcp, ip_ok;
    uint8_t ip[4], mascara[4], puerta[4];
    char red_modelo[32];
    uint64_t rx_paquetes, tx_paquetes, rx_bytes, tx_bytes, rx_descartados, tx_errores;
    /* Stream LAN de la consola (el que lee terminal_stream.py en el PC) */
    uint32_t stream_activo, stream_conectado, stream_puerto, reservado0;
    uint8_t stream_ip[4];
    uint32_t reservado1;
    uint64_t stream_enviado;       /* Bytes del registro de la consola ya entregados */
    /* Monton del nucleo y el huevo */
    uint64_t heap_capacidad, heap_en_uso, heap_bloques_activos, heap_bloques_libres;
    uint64_t canario_inicio, canario_fin, huevo_canario;
    uint32_t heap_canarios_ok, huevo_magico, huevo_salud, huevo_estado;
    uint32_t heap_nmapa, heap_total_bloques;   /* Bloques en heap_mapa y en la lista entera */
    uint8_t heap_mapa[64];         /* Primeros bloques del monton: 0 libre, 1 en uso, 2 canario roto */
    uint64_t paginas_totales, paginas_libres;
    /* Volumen exFAT abierto, solo con TAEK_SEN_LEER_VOLUMEN */
    uint32_t vol_leido;            /* 1 si se leyo ahora; 0 si no habia exFAT abierto o fallo la lectura */
    uint32_t vol_lba;              /* Sector de la region de arranque (el inicio de la particion) */
    uint16_t vol_banderas;         /* VolumeFlags, offset 0x6A: bit 1 sucio, bit 2 fallo de medio */
    uint8_t vol_bytes[16];         /* Bytes 0x60 a 0x6F de ese sector */
    uint16_t reservado2;
    /* Version 2 (H151). Contadores que solo crecen: el programa resta dos lecturas.
     * CPU 0 (la que corre las aplicaciones): ocupada = tsc - espera (ciclos dentro de SYS_ESPERAR).
     * CPU 1-3: ciclos trabajando en lotes (decodificar video); el resto del tiempo duermen con hlt. */
    uint32_t cpus;                 /* CPU arrancadas */
    uint32_t aperf_ok;             /* 1 si el procesador tiene APERF/MPERF (CPUID 06h ECX bit 0) */
    uint64_t tsc;                  /* rdtsc de la CPU 0 al leer */
    uint64_t espera_ciclos;        /* CPU 0: ciclos dentro de SYS_ESPERAR */
    uint64_t ocupado_ciclos[4];    /* [0] sin uso (se calcula); [1..3] ciclos de trabajo de cada CPU auxiliar */
    uint64_t ticks_por_ms[4];      /* Reloj de cada CPU, para pasar ciclos a tiempo */
    uint64_t aperf, mperf;         /* CPU 0: frecuencia efectiva = TSC * dAPERF / dMPERF */
    /* Version 3 (H204, Fase 0 del gestor de hilos), solo con TAEK_SEN_V3: cuanto tarda CPU 0 en volver a atender a lo
     * que hoy va por sondeo. Maximos desde el arranque o desde TAEK_SEN_CERO_LATENCIA. Un *_sys_ini es el numero de la
     * SYS_* en curso en la pasada que abrio el hueco y *_sys_fin el de la que lo cerro; 255 = ninguna (el programa o
     * la consola del nucleo). La que lo cierra suele ser la que vuelve a atender, no la culpable. */
    uint32_t usb_max_ms;           /* Mayor hueco entre dos pasadas por el anillo de eventos del xHCI */
    uint16_t usb_max_sys_ini, usb_max_sys_fin;
    uint64_t usb_max_cuando_ms;    /* Cuando se cerro, en milisegundos desde el arranque */
    uint64_t usb_pasadas;
    uint32_t usb_reentradas;       /* Pasadas que empezaron con otra en curso (llamadas anidadas) */
    uint32_t usb_eventos_max;      /* Mas eventos sacados en una pasada (el anillo tiene 64) */
    uint32_t usb_anillo_lleno;     /* Host Controller Events con Event Ring Full Error */
    uint32_t usb_cambios_puerto;   /* Port Status Change Events */
    uint32_t usb_reintentos, usb_recuperados, usb_perdidos;   /* Reset de puerto: reintentos, enlaces que volvieron, rendidos */
    uint32_t red_max_ms;           /* Mayor hueco entre dos pasadas por la pila de red */
    uint16_t red_max_sys_ini, red_max_sys_fin;
    uint32_t reservado3;
    uint64_t red_max_cuando_ms;
    uint64_t red_pasadas;
    uint32_t hda_max_ms;           /* El del controlador HDA: vuelve a cero cada vez que arranca el sonido */
    uint32_t hda_saltos_tardios;   /* H203: avances atrasados del DMA que se aceptaron */
    uint64_t raton_perdidos;       /* Reportes del raton USB que no cupieron en su cola */
    uint64_t teclas_perdidas;      /* Teclas USB que no cupieron en el bufer */
    uint32_t sys_max_us[32];       /* Por numero de SYS_*: la llamada mas larga, en microsegundos */
    uint32_t sucesos_total;        /* Sucesos de puerto desde el arranque */
    uint32_t sucesos_n;            /* Los ultimos que trae sucesos, del mas viejo al mas nuevo */
    struct taek_suceso_puerto sucesos[8];
    uint32_t huecos_total;         /* Huecos de 250 ms o mas (USB o red) desde el arranque; no se borran con el cero */
    uint32_t huecos_n;             /* Los ultimos que trae huecos, del mas viejo al mas nuevo */
    struct taek_hueco huecos[8];
};
#define TAEK_SENSORES_V2_TAM __builtin_offsetof(struct taek_sensores, usb_max_ms)
/* H148: SYS_TAMANO. Una superficie conserva los pixeles con que se creo (ancho x alto) y puede mostrarse con otro
 * tamano mientras ancho * alto no pase de esa capacidad. La fila pasa a medir el ancho nuevo: el programa dibuja
 * primero con la forma nueva y despues llama, asi la pantalla nunca ve la mezcla. Sirve para escalar ventanas. */

/* H146: sonido. PCM de 16 bits con signo, estereo intercalado, 44 100 Hz: lo que espera el controlador HDA/AC97 del
 * nucleo. El programa decodifica (MP3, lo que sea) y encola; el nucleo lo lleva a la tarjeta. Al terminar el programa,
 * el sonido se detiene solo. */
enum { TAEK_AU_ESTADO = 1, TAEK_AU_ENCOLAR = 2, TAEK_AU_INICIAR = 3, TAEK_AU_DETENER = 4, TAEK_AU_VOLUMEN = 5 };
struct taek_audio {
    uint32_t op;
    int32_t volumen_db;        /* VOLUMEN: entrada; siempre: salida */
    uint64_t datos;            /* ENCOLAR: PCM */
    uint32_t bytes;            /* ENCOLAR: cuantos (multiplo de 4) */
    uint32_t aceptados;        /* Salida de ENCOLAR */
    uint32_t ocupada, libre;   /* Salida: bytes en la cola del nucleo y espacio que queda */
    uint32_t sonando, hay_audio;
    uint64_t tocado_ms;        /* Salida: milisegundos ya enviados a los altavoces desde INICIAR */
};
#define TAEK_AUDIO_HZ 44100u

/* H146: programa especial para SYS_LANZAR. El nucleo abre 'argumento' con su reproductor (video H.264, MP3, imagen)
 * a pantalla completa y, al terminar, vuelve a abrir al programa que lo pidio. */
#define TAEK_PROGRAMA_ABRIR "taek:abrir"

/* H145: archivos para programas. Sobre el disco que este abierto (el mismo que 'seleccionar' en la terminal).
 * H147: tambien escritura, en FAT32 y exFAT: CREAR_CARPETA (ruta), ESCRIBIR (ruta; destino y capacidad son los datos,
 * hasta 64 MB; si existe, se reemplaza), MOVER (ruta -> ruta2, tambien sirve para renombrar) y BORRAR (ruta). Solo
 * archivos para mover y borrar. Nunca dentro de /ventoy ni sobre una .iso: eso hace arrancar el equipo (-TAEK_EPROTEGIDO). */
enum { TAEK_AR_DISCOS = 1, TAEK_AR_MONTAR = 2, TAEK_AR_LISTAR = 3, TAEK_AR_LEER = 4,
       TAEK_AR_CREAR_CARPETA = 5, TAEK_AR_ESCRIBIR = 6, TAEK_AR_MOVER = 7, TAEK_AR_BORRAR = 8,
       /* H188: lectura en secuencia sin reabrir. LEER abre la ruta y busca la posicion en cada llamada (en exFAT
        * ~100 ms fijos); con ABRIR_FD (ruta -> manejador y total), LEER_FD (manejador, destino, capacidad: sigue
        * donde quedo) y CERRAR_FD el costo se paga una vez. Hasta 4 abiertos; se cierran solos al salir. */
       TAEK_AR_ABRIR_FD = 9, TAEK_AR_LEER_FD = 10, TAEK_AR_CERRAR_FD = 11 };
#define TAEK_EPROTEGIDO 13
/* Errores del sistema de archivos (negativos): -26 no existe, -29 solo lectura, -31 ya existe, -32 ocupado,
 * -33 sin espacio, -34 nombre invalido, -21 no soportado (p. ej. mover una carpeta), -22 volumen danado. */
#define TAEK_RUTA 512
struct taek_disco_usb { char nombre[40]; uint64_t bytes; uint32_t numero, listo, montado, reservado; };
struct taek_entrada { char nombre[232]; uint64_t tam; uint32_t carpeta, reservado; };
struct taek_archivos {
    uint32_t op;
    uint32_t disco;            /* MONTAR: numero de disco. H148, el resto: 0 = el abierto; N + 1 = el disco N (se monta) */
    char ruta[TAEK_RUTA];      /* LISTAR, LEER */
    uint64_t destino;          /* DISCOS: taek_disco_usb[]; LISTAR: taek_entrada[]; LEER: bytes */
    uint64_t capacidad;        /* Elementos (DISCOS, LISTAR) o bytes (LEER) */
    uint64_t desde;            /* LISTAR: primera entrada; LEER: posicion en el archivo */
    uint64_t n;                /* Salida: elementos o bytes entregados */
    uint64_t total;            /* Salida: LISTAR, entradas de la carpeta; LEER, tamano del archivo */
    char fs[16];               /* Salida: sistema de archivos abierto ("FAT32", "exFAT"...) o "" */
    int32_t disco_abierto;     /* Salida: numero del disco abierto, -1 si ninguno */
    uint32_t manejador;        /* H188: ABRIR_FD (salida), LEER_FD y CERRAR_FD (entrada) */
    char ruta2[TAEK_RUTA];     /* H147: MOVER, destino */
};

/* H145: abrir otra aplicacion. Al terminar la que llama, el nucleo abre 'programa' con 'argumento'; cuando esa termina,
 * vuelve a abrir la que llamo con 'argumento_al_volver' (por ejemplo, la carpeta donde estaba). */
struct taek_lanzar { char programa[TAEK_RUTA]; char argumento[TAEK_RUTA]; char argumento_al_volver[TAEK_RUTA]; };

/* H144: el monton de cada programa. Empieza vacio en TAEK_MONTON y crece o encoge por paginas de 4 KiB, ya a cero.
 * SYS_MEMORIA(+n) agranda n bytes (redondeado a paginas) y devuelve el final anterior, que es donde empieza lo nuevo.
 * SYS_MEMORIA(-n) devuelve al sistema las paginas enteras del final; SYS_MEMORIA(0) solo consulta el final.
 * Negativo: -TAEK_ENOMEM si no queda RAM o se pasa del tope, -TAEK_EINVAL si se encoge mas de lo que hay. */
#define TAEK_MONTON     UINT64_C(0x0000008040000000)   /* ANILLO3_BASE + 1 GiB, detras de las superficies */
#define TAEK_MONTON_MAX UINT64_C(0x0000000400000000)   /* 16 GiB de direcciones; la RAM libre es el limite real */

/* H143: una descarga web. El nucleo sigue hasta 4 redirecciones (nunca de HTTPS a HTTP), quita la codificacion por
 * trozos y deja solo el cuerpo. Devuelve los bytes del cuerpo copiados (puede cortarse a 'capacidad') o un negativo. */
#define TAEK_WEB_URL 1024
struct taek_web {
    char url[TAEK_WEB_URL];        /* Entrada: http://... o https://..., terminada en 0. Salida: la URL final */
    uint64_t destino;              /* Entrada: bufer del programa para el cuerpo */
    uint32_t capacidad;            /* Entrada: su tamano */
    uint32_t bytes;                /* Salida: bytes copiados */
    uint32_t total;                /* Salida: bytes que tenia el cuerpo */
    int32_t estado;                /* Salida: codigo HTTP (200, 404...) */
    int32_t error;                 /* Salida: 0, o el codigo de la falla (de red, de TLS o de la respuesta) */
    uint32_t reservado;
};
#define TAEK_WEB_EURL 1      /* URL no valida */
#define TAEK_WEB_ERED 2      /* No se pudo descargar */
#define TAEK_WEB_EHTTP 3     /* Respuesta HTTP invalida */
#define TAEK_WEB_EREDIR 4    /* Redireccion invalida, insegura o demasiadas */

/* H183: sockets para los programas. HTTP, JSON, HLS, OAuth... viven en el programa; el nucleo da TCP y TLS 1.3
 * (verificacion X.509 obligatoria) y no pausa el stream de consola. Hasta 7 conexiones TCP, 4 de ellas TLS, a la
 * vez. El nucleo cierra las que el programa deje abiertas al salir. Devuelve >= 0 o -TAEK_RED_E*. */
#define TAEK_RED_HOST 256
struct taek_red {
    uint32_t op;                   /* TAEK_RED_ABRIR, _ENVIAR, _RECIBIR, _CERRAR, _ESTADO */
    int32_t  id;                   /* ABRIR: salida; el resto: entrada */
    char     host[TAEK_RED_HOST];  /* ABRIR: nombre DNS o IPv4 en texto, terminado en 0 (tambien el nombre del certificado) */
    uint16_t puerto;               /* ABRIR */
    uint16_t banderas;             /* ABRIR: TAEK_RED_TLS */
    uint32_t espera_ms;            /* ABRIR/ENVIAR: limite; RECIBIR: cuanto esperar al primer byte (0 = no esperar) */
    uint64_t datos;                /* ENVIAR/RECIBIR: bufer del programa */
    uint32_t largo;                /* ENVIAR: bytes; RECIBIR: capacidad (se atiende hasta 64 KiB por llamada) */
    uint32_t hechos;               /* Salida: bytes enviados o recibidos */
    int32_t  estado;               /* Salida: 1 abierta, 2 el otro lado cerro, 0 cerrada o caida */
    int32_t  error;                /* Salida: detalle (codigo de TCP o TLS) */
};
#define TAEK_RED_ABRIR   1
#define TAEK_RED_ENVIAR  2
#define TAEK_RED_RECIBIR 3       /* 0 bytes = nada en espera_ms; -TAEK_RED_ECERRADA = fin del flujo */
#define TAEK_RED_CERRAR  4
#define TAEK_RED_ESTADO  5
#define TAEK_RED_TLS     1
#define TAEK_RED_EPARAM    1
#define TAEK_RED_EDNS      2
#define TAEK_RED_ECONECTAR 3
#define TAEK_RED_ETLS      4
#define TAEK_RED_ELLENO    5       /* no quedan sockets */
#define TAEK_RED_ECERRADA  6
#define TAEK_RED_ETIEMPO   7

/* H185: video descargado por el programa. SYS_VIDEO copia el MP4 (hasta TAEK_VIDEO_MAX) a memoria del nucleo; luego el
 * programa sale con taek_lanzar(TAEK_PROGRAMA_ABRIR, "mem:<titulo>", <argumento al volver>) y el nucleo lo reproduce a
 * pantalla completa (H.264 + AAC), lo libera y vuelve a abrir el programa. 0 = entregado; -TAEK_ENOMEM, -TAEK_EINVAL. */
#define TAEK_VIDEO_MAX (128u * 1024u * 1024u)
#define TAEK_VIDEO_MEM "mem:"

/* H187: reproduccion en flujo, estilo MediaCodec. El programa baja y demultiplexa (HLS, MP4 por rangos...) y empuja
 * muestras; el nucleo decodifica el H.264 (GPU Intel o software) y el AAC, sincroniza con el reloj del audio y
 * presenta a pantalla completa mientras la sesion este abierta. Una sesion a la vez; se cierra sola si el programa sale.
 *   ABRIR : datos/largo = avcC (AVCDecoderConfigurationRecord); escala = ticks por segundo de los PTS (90000 en TS);
 *           canales/frecuencia del AAC (0 = sin audio). Conviene mandar ~1 s de audio antes del primer cuadro.
 *   VIDEO : una unidad de acceso, NAL con prefijo de largo (el del avcC), en orden de decodificacion; pts en `escala`.
 *           Bloquea mientras haya dos cuadros listos esperando su hora: el ritmo lo pone el nucleo.
 *   AUDIO : una trama AAC cruda (sin ADTS) con su pts. Bloquea si la cola de audio esta llena.
 *   ESTADO: solo rellena la salida.
 *   CERRAR: banderas TAEK_FLUJO_DRENAR = mostrar y sonar lo pendiente antes de cerrar.
 * Devuelve 0, 1 si el usuario cancelo (Esc, q, Ctrl+C) o -TAEK_E*. La salida se rellena en todas las operaciones. */
#define TAEK_FLUJO_ABRIR  1
#define TAEK_FLUJO_VIDEO  2
#define TAEK_FLUJO_AUDIO  3
#define TAEK_FLUJO_ESTADO 4
#define TAEK_FLUJO_CERRAR 5
#define TAEK_FLUJO_OSD    6   /* H188: datos = ARGB premultiplicado osd_ancho x osd_alto, en (osd_x, osd_y) de la pantalla,
                                 mezclado sobre cada cuadro durante osd_ms (0 = quitarlo) */
#define TAEK_FLUJO_MARCO  7   /* H201: el video se dibuja en (marco_x, marco_y) con marco_ancho x marco_alto de pantalla, debajo
                                 de la capa (OSD), que puede ser de pantalla completa con un hueco transparente. Se pide ANTES de
                                 ABRIR: esa sesion presenta por Iris (GPU). marco_ancho 0 = pantalla completa. Vale hasta CERRAR.
                                 La salida `iris` dice si la sesion abierta presenta por Iris (si no, el marco no se cumple). */
#define TAEK_FLUJO_OSD_MAX (16u * 1024u * 1024u)   /* H201: capa de hasta 16 MiB (1920x1080 ARGB son 8,3 MB) */
#define TAEK_FLUJO_DRENAR 1
#define TAEK_FLUJO_DESDE  2   /* H188, ABRIR: los cuadros con PTS < inicio se decodifican sin mostrarse (saltos) */
#define TAEK_FLUJO_MUESTRA_MAX (4u * 1024u * 1024u)
struct taek_flujo {
    uint32_t op, banderas;
    uint64_t datos;
    uint32_t largo, escala;
    int64_t  pts;
    uint32_t canales, frecuencia;
    /* Salida */
    uint64_t reloj_ms;          /* posicion del reloj maestro (audio) desde el primer PTS */
    uint32_t cola_audio_ms;     /* audio decodificado aun por sonar */
    uint32_t cuadros, tardios;  /* presentados y descartados por llegar tarde */
    uint32_t motor;             /* 0 software, 1 GPU Intel, 2 GPU y luego software */
    int32_t  abierta;
    /* H188 */
    int64_t  inicio;            /* ABRIR con TAEK_FLUJO_DESDE */
    uint32_t osd_x, osd_y, osd_ancho, osd_alto, osd_ms;
    uint32_t teclas_n;          /* Salida: teclas pulsadas desde la llamada anterior (TAEK_TECLA_* o ASCII). Mientras
                                   la sesion esta abierta el nucleo no las usa, salvo Ctrl+C, que cancela. */
    uint8_t  teclas[16];
    /* H201 */
    int32_t  marco_x, marco_y, marco_ancho, marco_alto;   /* MARCO */
    uint32_t iris;              /* Salida: 1 si la sesion presenta por Iris (marco y capa completa en la GPU) */
    uint32_t reservado_h201;
};

struct taek_disco { char nombre[28]; uint32_t gib; };

/* Lo que sale en Especificaciones y Bateria. Modelos si, numeros de serie y UUID no. */
struct taek_equipo {
    char fabricante[32], modelo[48], formato[16], firmware[48], fecha_firmware[16];
    char ram_tipo[12], red[32];
    uint32_t ram_mts, ram_modulos, ram_instalada_mib;
    uint32_t cpu_familia, cpu_modelo, cpu_paso;
    uint32_t ndiscos;
    struct taek_disco discos[4];
    /* Bateria. Valores en la unidad del firmware; 0xFFFFFFFF si no se sabe. */
    int32_t bat_pct, bat_estado, bat_cargador, bat_presente;
    uint32_t bat_unidad_mah;  /* 0: mWh y mW; 1: mAh y mA */
    uint32_t bat_resto, bat_lleno, bat_diseno, bat_tasa, bat_mv, bat_ciclos;
    char bat_modelo[24], bat_tipo[12];
};

/* Valor de salida de un programa que pide algo al nucleo. */
enum {
    TAEK_ACCION_TERMINAL = 1,
    TAEK_ACCION_REINICIAR = 2,
    TAEK_ACCION_APAGAR = 3,
    TAEK_ACCION_ESPECIFICACIONES = 4,
    TAEK_ACCION_BATERIA = 5,
    TAEK_ACCION_VIDEO = 6,
    TAEK_ACCION_DEMO = 7,
    TAEK_ACCION_PRUEBAS = 8,
    TAEK_ACCION_APP = 9,      /* H146: el menu pidio con SYS_LANZAR abrir una aplicacion; despues se vuelve al menu */
    TAEK_ACCION_ORDEN = 10,   /* H151: el menu pidio con SYS_LANZAR (taek:orden) una orden de pantalla completa */
};
/* H146: en la posicion que el menu devuelve (codigo >> 8), este bit pide volver directo al escritorio */
#define TAEK_POS_ESCRITORIO (1u << 9)

/* Sin numeros de serie ni identificadores unicos: un programa no los necesita. */
struct taek_info {
    char cpu[48];
    char hito[96];
    char capa[16];
    char gpu_proveedor[32];
    uint32_t cpus, gpu_proveedor_id, gpu_id, gpu_video_probada;
    uint64_t ram_total, ram_libre, tsc_mhz, activo_ms;
    uint16_t anio;
    uint8_t mes, dia, hora, minuto, segundo, fecha_ok;
    uint8_t ip[4];
    uint32_t red_presente, red_enlace, red_mbps, red_ip_ok;
    int32_t bateria_pct;     /* -1: sin dato */
    int32_t bateria_estado;  /* bit 0 descargando, bit 1 cargando, bit 2 critica */
    int32_t cargador;        /* -1 desconocido, 0 no, 1 si */
    uint32_t discos, smep, smap, ancho, alto;
};

/* Teclas especiales tal como llegan en taek_evento.tecla */
#define TAEK_TECLA_ARRIBA 17
#define TAEK_TECLA_ABAJO 18
#define TAEK_TECLA_IZQUIERDA 28   /* H146 */
#define TAEK_TECLA_DERECHA 29     /* H146 */
#define TAEK_TECLA_SUPRIMIR 30    /* H147 */
#define TAEK_TECLA_F2 31          /* H147 */

#define TAEK_EFAULT 14
#define TAEK_EINVAL 22
#define TAEK_EBADF 9
#define TAEK_ENOMEM 12
#define TAEK_ENODEV 19
#define TAEK_ENOSYS 38
#define TAEK_EAGAIN 11     /* H213: futex con el valor ya cambiado */
#define TAEK_EBUSY 16      /* H213: sin lugar para otro hilo */
#define TAEK_ETIEMPO 110   /* H213: se agoto el limite de espera */

/* Pixeles de 32 bits 0xAARRGGBB. Con TAEK_SUP_ALFA el color va premultiplicado por alfa. */
#define TAEK_SUP_ALFA 1u

struct taek_superficie {
    uint32_t ancho, alto;
    int32_t x, y;
    uint32_t banderas;
    uint32_t id;         /* Salida */
    uint64_t pixeles;    /* Salida: direccion en el programa, fila = ancho pixeles */
};

struct taek_evento {
    int32_t x, y;
    uint32_t botones;
    uint32_t tecla;      /* 0 si no hay tecla; 27 = Esc */
    uint32_t hay_puntero;
    uint32_t reservado;
};

struct taek_pantalla { uint32_t ancho, alto; };

#endif
