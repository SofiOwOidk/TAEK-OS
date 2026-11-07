#ifndef TAEK_WEB_HISTORIAL_H
#define TAEK_WEB_HISTORIAL_H
#include "web_url.h"
#define WEB_HISTORIAL_MAX 16
struct web_historial {char urls[WEB_HISTORIAL_MAX][WEB_URL_MAX];unsigned total,actual;};
/* Call only after a complete successful response. index=-1 branches history. */
static inline int web_historial_visitar(struct web_historial *h,const char *url,int index){
    size_t n=0;if(!h || !url)return -1;while(url[n]){if(n+1>=WEB_URL_MAX)return -1;n++;}
    if(!n || index< -1 || (index>=0 && (unsigned)index>=h->total))return -1;
    if(index<0){
        if(h->total)h->total=h->actual+1;
        if(h->total==WEB_HISTORIAL_MAX){
            for(unsigned i=1;i<h->total;i++)for(size_t j=0;j<WEB_URL_MAX;j++)h->urls[i-1][j]=h->urls[i][j];
            h->total--;
        }
        h->actual=h->total++;
    }else h->actual=(unsigned)index;
    for(size_t i=0;i<=n;i++)h->urls[h->actual][i]=url[i];return 0;
}
#endif
