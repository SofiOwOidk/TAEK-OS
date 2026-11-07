#include "navegador.h"
#include "tls.h"
#include "red.h"
#include "web_url.h"
#include "web_historial.h"
#include "http_respuesta.h"
#include "../consola.h"
#include "../puntero.h"
#include "../teclado.h"
#include "../../base/tiempo.h"
#include "../../base/memoria.h"

#define MAX_WEB_BUF 262144
#define MAX_LINKS   24

struct web_link {
    char url[WEB_URL_MAX];
    char texto[64];
};

static char s_web_buf[MAX_WEB_BUF];
static struct web_link s_links[MAX_LINKS];
static int s_num_links = 0;
static struct web_historial s_historial;
struct web_boton {uint64_t linea,fin;int col,end;const char *comando;};
static struct web_boton s_botones[6];
static unsigned s_total_botones;

static int web_str_igual_sin_caso(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c1 = s1[i], c2 = s2[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return 0;
        if (!c1) return 1;
    }
    return 1;
}

static size_t web_str_len(const char *s) {
    size_t l = 0;
    while (s && s[l]) l++;
    return l;
}

static void web_str_copiar(char *dst, const char *src, size_t max) {
    if (!dst || !max) return;
    size_t i = 0;
    while (src && src[i] && i + 1 < max) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int web_tag(const char *tag,const char *literal) {
    size_t n=web_str_len(literal);
    return web_str_len(tag)==n && web_str_igual_sin_caso(tag,literal,n);
}

static void web_decodificar_entidad(const char **p, char *salida, size_t *out_len) {
    const char *s = *p;
    if (web_str_igual_sin_caso(s, "&nbsp;", 6)) { salida[(*out_len)++] = ' '; *p += 5; }
    else if (web_str_igual_sin_caso(s, "&amp;", 5)) { salida[(*out_len)++] = '&'; *p += 4; }
    else if (web_str_igual_sin_caso(s, "&lt;", 4)) { salida[(*out_len)++] = '<'; *p += 3; }
    else if (web_str_igual_sin_caso(s, "&gt;", 4)) { salida[(*out_len)++] = '>'; *p += 3; }
    else if (web_str_igual_sin_caso(s, "&quot;", 6)) { salida[(*out_len)++] = '"'; *p += 5; }
    else if (web_str_igual_sin_caso(s, "&#39;", 5) || web_str_igual_sin_caso(s, "&apos;", 6)) {
        salida[(*out_len)++] = '\'';
        *p += (s[1] == '#' ? 4 : 5);
    } else {
        salida[(*out_len)++] = '&';
    }
}

// Renderiza un texto formateado con ajuste de línea en columna ~76
static void web_imprimir_palabra(const char *palabra, size_t len, int *col, uint32_t color) {
    if (!len) return;
    if (*col + (int)len + 1 > 76 && *col > 4) {
        consola_imprimir_linea("");
        consola_imprimir("  ");
        *col = 2;
    } else if (*col > 2) {
        consola_imprimir(" ");
        (*col)++;
    }
    for (size_t i = 0; i < len; i++) {
        consola_escribir_caracter(palabra[i]);
    }
    *col += (int)len;
}

static void web_renderizar_html(const char *html, const char *url_actual) {
    // 1. Extraer título <title>...</title>
    char titulo[96] = "Página Web";
    const char *pt = html;
    while (*pt) {
        if (web_str_igual_sin_caso(pt, "<title>", 7)) {
            pt += 7;
            size_t ti = 0;
            while (*pt && !web_str_igual_sin_caso(pt, "</title>", 8) && ti + 1 < sizeof(titulo)) {
                if (*pt != '\r' && *pt != '\n') titulo[ti++] = *pt;
                pt++;
            }
            titulo[ti] = '\0';
            break;
        }
        pt++;
    }

    // Cabecera del Navegador
    consola_imprimir_linea_color("============================================================================", COLOR_AVISO_DEFAULT);
    consola_imprimir_color("  TAEK-OS Web / ", COLOR_AVISO_DEFAULT);
    consola_imprimir_color(titulo, COLOR_PROMPT_DEFAULT);
    consola_imprimir_linea("");
    consola_imprimir_color("  URL: ", COLOR_TEXTO_DEFAULT);
    consola_imprimir_linea_color(url_actual, COLOR_USUARIO_DEFAULT);
    consola_imprimir_linea_color("----------------------------------------------------------------------------", COLOR_AVISO_DEFAULT);

    s_num_links = 0;
    const char *p = html;
    int en_script = 0;
    int en_style  = 0;
    int col = 2;
    consola_imprimir("  ");

    char palabra[128];
    size_t pal_len = 0;

    int en_enlace = 0;
    char enlace_href[WEB_URL_MAX] = {0};
    char enlace_txt[64] = {0};
    size_t enlace_txt_len = 0;

    uint32_t color_actual = COLOR_TEXTO_DEFAULT;

    while (*p) {
        if (consola_sondear_cancelacion()) break;

        // Comentarios HTML <!-- ... -->
        if (p[0] == '<' && p[1] == '!' && p[2] == '-' && p[3] == '-') {
            p += 4;
            while (*p && !(p[0] == '-' && p[1] == '-' && p[2] == '>')) p++;
            if (*p) p += 3;
            continue;
        }

        // Etiquetas HTML <tag ...>
        if (*p == '<') {
            // Vaciar palabra acumulada
            if (pal_len > 0) {
                web_imprimir_palabra(palabra, pal_len, &col, color_actual);
                pal_len = 0;
            }

            p++;
            int cierre = 0;
            if (*p == '/') { cierre = 1; p++; }

            char tag[16];
            size_t ti = 0;
            while (*p && *p != '>' && *p != ' ' && *p != '/' && ti + 1 < sizeof(tag)) {
                char c = *p++;
                if (c >= 'A' && c <= 'Z') c += 32;
                tag[ti++] = c;
            }
            tag[ti] = '\0';

            // Capturar atributos (como href="...")
            char attr_href[WEB_URL_MAX] = {0};
            while (*p && *p != '>') {
                while (*p==' ' || *p=='\t' || *p=='\r' || *p=='\n' || *p=='/') p++;
                const char *key=p;
                while (*p && *p!='=' && *p!='>' && *p!=' ' && *p!='\t' && *p!='\r' && *p!='\n') p++;
                size_t keylen=(size_t)(p-key);
                while (*p==' ' || *p=='\t' || *p=='\r' || *p=='\n') p++;
                if (*p!='=') { if (*p && *p!='>') p++;continue; }
                p++;while (*p==' ' || *p=='\t' || *p=='\r' || *p=='\n') p++;
                char delim=(*p=='"' || *p=='\'')?*p++:0;
                const char *value=p;
                while (*p && (delim?*p!=delim:(*p!='>' && *p!=' ' && *p!='\t' && *p!='\r' && *p!='\n'))) p++;
                size_t len=(size_t)(p-value);
                if(keylen==4 && web_str_igual_sin_caso(key,"href",4) && len<sizeof(attr_href) && (!delim || *p==delim)) {
                    memcpy(attr_href,value,len);attr_href[len]=0;
                }
                if(delim && *p==delim)p++;
            }
            if (*p == '>') p++;

            // Procesar etiquetas de control
            if (web_tag(tag, "script")) {
                en_script = !cierre;
                continue;
            }
            if (web_tag(tag, "style")) {
                en_style = !cierre;
                continue;
            }
            if (en_script || en_style) continue;

            // Párrafos y saltos
            if (web_tag(tag, "p") || web_tag(tag, "div") ||
                web_tag(tag, "section") || web_tag(tag, "article")) {
                if (cierre) {
                    consola_imprimir_linea("");
                    consola_imprimir("  ");
                    col = 2;
                }
            } else if (web_tag(tag, "br")) {
                consola_imprimir_linea("");
                consola_imprimir("  ");
                col = 2;
            } else if (web_tag(tag, "hr")) {
                consola_imprimir_linea("");
                consola_imprimir_linea_color("  ──────────────────────────────────────────────────────────────────────", COLOR_AVISO_DEFAULT);
                consola_imprimir("  ");
                col = 2;
            } else if (web_tag(tag, "h1") || web_tag(tag, "h2") ||
                       web_tag(tag, "h3")) {
                consola_imprimir_linea("");
                if (!cierre) {
                    consola_establecer_color_texto(COLOR_AVISO_DEFAULT);
                    consola_imprimir("  ▶ ");
                    color_actual = COLOR_AVISO_DEFAULT;
                    col = 4;
                } else {
                    consola_establecer_color_texto(COLOR_TEXTO_DEFAULT);
                    color_actual = COLOR_TEXTO_DEFAULT;
                    consola_imprimir_linea("");
                    consola_imprimir("  ");
                    col = 2;
                }
            } else if (web_tag(tag, "li")) {
                if (!cierre) {
                    consola_imprimir_linea("");
                    consola_imprimir_color("   • ", COLOR_PROMPT_DEFAULT);
                    col = 5;
                }
            } else if (web_tag(tag, "a")) {
                if (!cierre) {
                    en_enlace = 1;
                    enlace_txt_len = 0;
                    enlace_txt[0] = '\0';
                    web_str_copiar(enlace_href, attr_href, sizeof(enlace_href));
                    consola_establecer_color_texto(COLOR_USUARIO_DEFAULT);
                    color_actual = COLOR_USUARIO_DEFAULT;
                } else {
                    en_enlace = 0;
                    consola_establecer_color_texto(COLOR_TEXTO_DEFAULT);
                    color_actual = COLOR_TEXTO_DEFAULT;
                    if (s_num_links < MAX_LINKS && enlace_href[0] && !web_url_resolver(url_actual,enlace_href,s_links[s_num_links].url,sizeof(s_links[0].url))) {
                        enlace_txt[enlace_txt_len]=0;
                        s_num_links++;
                        char numbuf[8];
                        numbuf[0] = '[';
                        numbuf[1] = '#';
                        int idx = s_num_links;
                        int npos = 2;
                        if (idx >= 10) numbuf[npos++] = (char)('0' + (idx / 10));
                        numbuf[npos++] = (char)('0' + (idx % 10));
                        numbuf[npos++] = ']';
                        numbuf[npos] = '\0';
                        consola_imprimir_color(numbuf, COLOR_PROMPT_DEFAULT);
                        col += npos;

                        web_str_copiar(s_links[s_num_links - 1].texto, enlace_txt[0] ? enlace_txt : enlace_href, sizeof(s_links[0].texto));
                    }
                }
            }
            continue;
        }

        if (en_script || en_style) { p++; continue; }

        // Caracteres de espacio
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            if (pal_len > 0) {
                web_imprimir_palabra(palabra, pal_len, &col, color_actual);
                pal_len = 0;
            }
            if(en_enlace && enlace_txt_len && enlace_txt[enlace_txt_len-1]!=' ' && enlace_txt_len+1<sizeof(enlace_txt)) enlace_txt[enlace_txt_len++]=' ';
            p++;
            continue;
        }

        // Decodificación de entidades &...;
        if (*p == '&') {
            char ent[2] = {0};
            size_t el = 0;
            web_decodificar_entidad(&p, ent, &el);
            for (size_t k = 0; k < el; k++) {
                if (pal_len + 1 < sizeof(palabra)) palabra[pal_len++] = ent[k];
                if (en_enlace && enlace_txt_len + 1 < sizeof(enlace_txt)) enlace_txt[enlace_txt_len++] = ent[k];
            }
            p++;
            continue;
        }

        // Carácter normal
        char c = *p++;
        if (pal_len + 1 < sizeof(palabra)) palabra[pal_len++] = c;
        if (en_enlace && enlace_txt_len + 1 < sizeof(enlace_txt)) enlace_txt[enlace_txt_len++] = c;
    }

    if (pal_len > 0) {
        web_imprimir_palabra(palabra, pal_len, &col, color_actual);
    }

    consola_imprimir_linea("");
    consola_establecer_color_texto(COLOR_TEXTO_DEFAULT);

    // Pie de página y lista de enlaces
    if (s_num_links > 0) {
        consola_imprimir_linea("");
        consola_imprimir_linea_color("─── ENLACES EN LA PÁGINA ───────────────────────────────────────────────────", COLOR_AVISO_DEFAULT);
        for (int i = 0; i < s_num_links; i++) {
            consola_imprimir("  [#");
            consola_imprimir_dec((uint64_t)(i + 1));
            consola_imprimir("] ");
            consola_imprimir_color(s_links[i].texto, COLOR_USUARIO_DEFAULT);
            consola_imprimir(" -> ");
            consola_imprimir_linea_color(s_links[i].url, COLOR_PROMPT_DEFAULT);
        }
    }

    consola_imprimir_linea_color("============================================================================", COLOR_AVISO_DEFAULT);
}

/* Location se lee exclusivamente dentro de las cabeceras ya validadas. */
static int web_location(const char *buf,size_t end,char *out,size_t cap) {
    size_t i=0;out[0]=0;int visto=0;
    while(i+1<end) {
        size_t inicio=i;while(i+1<end && !(buf[i]=='\r' && buf[i+1]=='\n'))i++;
        size_t fin=i;i+=2;
        if(fin-inicio>=9 && web_str_igual_sin_caso(buf+inicio,"Location:",9)) {
            if(visto++)return -1;inicio+=9;
            while(inicio<fin && (buf[inicio]==' ' || buf[inicio]=='\t'))inicio++;
            while(fin>inicio && (buf[fin-1]==' ' || buf[fin-1]=='\t'))fin--;
            size_t n=fin-inicio;if(!n || n>=cap)return -1;
            memcpy(out,buf+inicio,n);out[n]=0;
        }
    }return visto;
}

static void web_boton(const char *texto,const char *comando){
    struct web_boton *b=&s_botones[s_total_botones++];b->comando=comando;
    consola_obtener_posicion(&b->col,&b->linea);consola_imprimir_color(texto,COLOR_PROMPT_DEFAULT);consola_obtener_posicion(&b->end,&b->fin);
}
static void web_barra(const char *url,unsigned estado){
    s_total_botones=0;
    consola_imprimir_linea_color("+--------------------------- TAEK WEB ---------------------------+",COLOR_USUARIO_DEFAULT);
    consola_imprimir_color("  Direccion: ",COLOR_AVISO_DEFAULT);consola_imprimir_linea(url);
    consola_imprimir("  ");web_boton(s_historial.total && s_historial.actual?"[ atras ]":"[ atras: -- ]","atras");
    consola_imprimir("  ");web_boton(s_historial.total && s_historial.actual+1<s_historial.total?"[ adelante ]":"[ adelante: -- ]","adelante");
    consola_imprimir("  ");web_boton("[ recargar ]","recargar");consola_imprimir("  ");web_boton("[ ir URL ]","ir ");
    consola_imprimir_linea("");consola_imprimir("  ");web_boton("[ enlaces ]","enlaces");consola_imprimir("  ");web_boton("[ q salir ]","q");consola_imprimir_linea("");
    if(estado){consola_imprimir("  HTTP ");consola_imprimir_dec(estado);consola_imprimir(" | ");
        consola_imprimir_linea_color(web_str_igual_sin_caso(url,"https://",8)?"HTTPS autenticado":"HTTP",COLOR_TEXTO_DEFAULT);}
    consola_imprimir_linea("  Ctrl+Arriba / Ctrl+Abajo: desplazar la consola. Historial: 16 paginas.");
}
static int web_leer_linea(char *entrada,size_t cap){
    size_t n=0;unsigned exceso=0;entrada[0]=0;uint64_t clicks=puntero_obtener_estado()->clicks;
    for(;;){
        char c=consola_leer_caracter();const struct puntero_estado *p=puntero_obtener_estado();
        if(p->clicks!=clicks){
            clicks=p->clicks;uint64_t linea=consola_linea_visible((unsigned)p->click_y/16);int col=p->click_x/8;
            for(unsigned i=0;i<s_total_botones;i++){
                const struct web_boton *b=&s_botones[i];
                int hit=b->linea==b->fin?(linea==b->linea && col>=b->col && col<b->end):
                    ((linea==b->linea && col>=b->col) || (linea==b->fin && col<b->end));
                if(!hit)continue;
                while(n){consola_escribir_caracter('\b');n--;}exceso=0;
                web_str_copiar(entrada,b->comando,cap);n=web_str_len(entrada);consola_imprimir(entrada);
                if(web_tag(b->comando,"ir "))break;
                consola_imprimir_linea(" [mouse]");return (int)n;
            }
        }
        if(c==27 || c==3){consola_imprimir_linea("");return 0;}
        if(c=='\n' || c=='\r'){entrada[n]=0;consola_imprimir_linea("");if(exceso){consola_imprimir_linea("  [!] Entrada demasiado larga.");return -1;}return (int)n;}
        if(c==TECLA_CTRL_ARRIBA)consola_desplazar(1);
        else if(c==TECLA_CTRL_ABAJO)consola_desplazar(-1);
        else if(c=='\b' || c==127){if(exceso)exceso--;else if(n){n--;entrada[n]=0;consola_escribir_caracter('\b');}}
        else if((unsigned char)c>=32){if(n+1<cap && !exceso){entrada[n++]=c;entrada[n]=0;consola_escribir_caracter(c);}else if(exceso<UINT32_MAX)exceso++;}
        esperar_milisegundos(1);
    }
}
static void web_enlaces(void){
    if(!s_num_links){consola_imprimir_linea("  Esta pagina no tiene enlaces catalogados; puedes escribir ir URL.");return;}
    for(int i=0;i<s_num_links;i++){
        consola_imprimir("  [#");consola_imprimir_dec((uint64_t)i+1);consola_imprimir("] ");
        consola_imprimir_color(s_links[i].texto,COLOR_USUARIO_DEFAULT);consola_imprimir(" -> ");consola_imprimir_linea(s_links[i].url);
    }
}
static int web_elegir_destino(char url[WEB_URL_MAX],int *indice){
    for(;;){
        consola_imprimir("  [WEB] enlace / URL / comando (o Enter para salir): ");
        char entrada[WEB_URL_MAX];int nread=web_leer_linea(entrada,sizeof(entrada));if(nread<0)continue;if(!nread)return 0;
        char *ep=entrada;while(*ep==' ')ep++;
        size_t len=web_str_len(ep);while(len && ep[len-1]==' ')ep[--len]=0;
        if(!len || (len==1 && (*ep=='q' || *ep=='Q')))return 0;
        if(web_tag(ep,"enlaces")){web_enlaces();continue;}
        if(web_tag(ep,"ayuda")){web_barra(url,0);continue;}
        int atras=web_tag(ep,"atras"),adelante=web_tag(ep,"adelante"),recarga=web_tag(ep,"recargar");
        if(atras || adelante || recarga){
            if(!s_historial.total || (atras && !s_historial.actual) || (adelante && s_historial.actual+1==s_historial.total)){
                consola_imprimir_linea("  [WEB] No hay pagina en esa direccion del historial.");continue;}
            *indice=(int)s_historial.actual+(adelante?1:atras?-1:0);
            web_str_copiar(url,s_historial.urls[*indice],WEB_URL_MAX);return 1;
        }
        if(len>3 && web_str_igual_sin_caso(ep,"ir ",3)){ep+=3;while(*ep==' ')ep++;}
        int bracket=*ep=='[';if(bracket)ep++;if(*ep=='#')ep++;
        if(*ep>='0' && *ep<='9'){
            unsigned elegido=0;while(*ep>='0' && *ep<='9'){
                if(elegido>MAX_LINKS){elegido=MAX_LINKS+1;break;}elegido=elegido*10+(unsigned)(*ep++-'0');}
            if(bracket && *ep==']')ep++;
            if(*ep || !elegido || elegido>(unsigned)s_num_links){consola_imprimir_linea("  [!] Numero de enlace no valido.");continue;}
            web_str_copiar(url,s_links[elegido-1].url,WEB_URL_MAX);
        }else if(bracket || !*ep || web_url_resolver(url,ep,url,WEB_URL_MAX)){
            consola_imprimir_linea("  [!] URL no valida.");continue;
        }
        *indice=-1;return 1;
    }
}
void navegador_abrir_url(const char *url_o_host) {
    char url_actual[WEB_URL_MAX];
    if(!url_o_host || web_str_len(url_o_host)>=sizeof(url_actual)) {
        consola_imprimir_linea("  [!] URL demasiado larga.");return;
    }
    web_str_copiar(url_actual,url_o_host,sizeof(url_actual));
    memset(&s_historial,0,sizeof(s_historial));s_num_links=0;
    int redirecciones=0,indice=-1;
    for(;;) {
        int es_https;char host[254],ruta[WEB_URL_MAX];uint16_t puerto;
        if(web_url_parsear(url_actual,&es_https,host,sizeof(host),&puerto,ruta,sizeof(ruta))) {
            consola_imprimir_linea("  [!] URL no valida. Uso: web <https://host[:puerto]/ruta>");goto interfaz_error;
        }
        consola_imprimir("  [NAVEGADOR] Conectando a ");consola_imprimir(es_https?"HTTPS: ":"HTTP: ");
        consola_imprimir(host);consola_imprimir_linea(ruta);
        int bytes=es_https?red_https_obtener(host,ruta,puerto,s_web_buf,sizeof(s_web_buf),30000):
                          red_http_obtener(host,ruta,puerto,s_web_buf,sizeof(s_web_buf),10000);
        if(bytes<=0) {
            consola_imprimir("  [!] Fallo de descarga web (codigo ");consola_imprimir_dec((uint64_t)(-bytes));consola_imprimir_linea(").");goto interfaz_error;
        }
        struct http_respuesta r;
        if(http_respuesta_validar(s_web_buf,(size_t)bytes,1,&r)!=1) {
            consola_imprimir_linea("  [!] Respuesta HTTP incompleta o invalida.");goto interfaz_error;
        }
        if(r.estado==301 || r.estado==302 || r.estado==303 || r.estado==307 || r.estado==308) {
            char location[WEB_URL_MAX],destino[WEB_URL_MAX];
            int found=web_location(s_web_buf,r.cuerpo,location,sizeof(location));
            if(found<0 || (found && web_url_resolver(url_actual,location,destino,sizeof(destino)))) {
                consola_imprimir_linea("  [!] Redireccion invalida.");goto interfaz_error;
            }
            if(found) {
                if(redirecciones==4){consola_imprimir_linea("  [!] Limite de cuatro redirecciones.");goto interfaz_error;}
                int seguro;char h[254],path[WEB_URL_MAX];uint16_t port;
                if(web_url_parsear(destino,&seguro,h,sizeof(h),&port,path,sizeof(path)) || (es_https && !seguro)) {
                    consola_imprimir_linea("  [!] Redireccion HTTPS a HTTP rechazada.");goto interfaz_error;
                }
                consola_imprimir("  Redirigiendo a: ");consola_imprimir_linea(destino);
                web_str_copiar(url_actual,destino,sizeof(url_actual));redirecciones++;continue;
            }
        }
        size_t n=(size_t)bytes;
        if(http_respuesta_desempaquetar(s_web_buf,&n,&r)) {
            consola_imprimir_linea("  [!] Cuerpo HTTP invalido.");goto interfaz_error;
        }
        if(web_historial_visitar(&s_historial,url_actual,indice))return;
        consola_limpiar();
        web_renderizar_html(s_web_buf+r.cuerpo,url_actual);web_barra(url_actual,r.estado);
        goto interfaz;
interfaz_error:
        if(s_historial.total)web_str_copiar(url_actual,s_historial.urls[s_historial.actual],sizeof(url_actual));
        else s_num_links=0;
        web_barra(url_actual,0);
interfaz:
        if(!web_elegir_destino(url_actual,&indice))return;
        redirecciones=0;
    }
}
