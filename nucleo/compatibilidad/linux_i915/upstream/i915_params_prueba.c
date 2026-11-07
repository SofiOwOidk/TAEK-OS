#include <linux/slab.h>
#include "i915_params.h"
#include "../upstream_params.h"
static void comprobar(struct taek_upstream_params_resultado *r,int ok,unsigned bit){r->checks++;if(!ok)r->fallos|=bit;}
static unsigned strings(struct i915_params *p,char ***out){
    out[0]=&p->vbt_firmware;out[1]=&p->force_probe;out[2]=&p->guc_firmware_path;
    out[3]=&p->huc_firmware_path;out[4]=&p->dmc_firmware_path;out[5]=&p->gsc_firmware_path;return 6;
}
int taek_upstream_params_probar(struct taek_upstream_params_resultado *r){
    if(!r)return -EINVAL;
    *r=(struct taek_upstream_params_resultado){0};
    struct taek_upstream_heap_estado baseline,st;taek_upstream_heap_estado(&baseline);
    if(baseline.activos || baseline.bytes)return -EBUSY;
    struct i915_params dest={0};
    i915_params_copy(&dest,&i915_modparams);
    comprobar(r,dest.enable_guc==i915_modparams.enable_guc && dest.reset==i915_modparams.reset &&
        dest.enable_hangcheck==i915_modparams.enable_hangcheck,1);
    comprobar(r,dest.force_probe && i915_modparams.force_probe && dest.force_probe!=i915_modparams.force_probe &&
        !strcmp(dest.force_probe,i915_modparams.force_probe),2);
    i915_params_free(&dest);taek_upstream_heap_estado(&st);
    comprobar(r,!st.activos && !st.bytes && st.errores==baseline.errores,4);
    struct i915_params src=i915_modparams;
    char **members[6];strings(&src,members);
    char textos[6][32]={"vbt-test.bin","5917","guc-test.bin","huc-test.bin","dmc-test.bin","gsc-test.bin"};
    for(unsigned i=0;i<6;i++)*members[i]=textos[i];
    i915_params_copy(&dest,&src);char **copies[6];strings(&dest,copies);
    for(unsigned i=0;i<6;i++){
        int distinto=*copies[i] && *copies[i]!=*members[i] && !strcmp(*copies[i],*members[i]);
        comprobar(r,distinto,8);if(distinto)r->privados++;
    }
    if(*copies[0] && *copies[0]!=*members[0])(*copies[0])[0]='X';
    comprobar(r,textos[0][0]=='v',16);
    i915_params_free(&dest);taek_upstream_heap_estado(&st);
    comprobar(r,!st.activos && !st.bytes && st.errores==baseline.errores,32);
    /* params_copy es void y puede dejar NULL en un miembro por OOM.
     * Su consumidor debe detectar esa copia parcial antes de publicarla. */
    for(unsigned fail=1;fail<=6;fail++){
        unsigned antes=st.fallos;taek_upstream_heap_fallar_en(fail);
        i915_params_copy(&dest,&src);strings(&dest,copies);
        unsigned nulos=0;for(unsigned i=0;i<6;i++)if(!*copies[i])nulos++;
        taek_upstream_heap_estado(&st);
        comprobar(r,nulos==1 && st.fallos==antes+1 && st.activos==5,64);
        i915_params_free(&dest);taek_upstream_heap_estado(&st);
        comprobar(r,!st.activos && !st.bytes && st.errores==baseline.errores,128);r->oom_probados++;
    }
    taek_upstream_heap_fallar_en(0);
    for(unsigned i=0;i<6;i++)*members[i]=NULL;
    i915_params_copy(&dest,&src);strings(&dest,copies);
    unsigned nulos=0;for(unsigned i=0;i<6;i++)if(!*copies[i])nulos++;
    comprobar(r,nulos==6,256);i915_params_free(&dest);
    taek_upstream_heap_estado(&st);
    r->limpio=!st.activos && !st.bytes && st.errores==baseline.errores;
    comprobar(r,r->limpio,512);return r->fallos?-EIO:0;
}
