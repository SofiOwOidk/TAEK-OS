#ifndef XHCI_BULK_PLAN_H
#define XHCI_BULK_PLAN_H
#include <stdint.h>

#define XHCI_BULK_PLAN_MAX 8
struct xhci_bulk_tramo {
    uint64_t fisica;
    uint32_t longitud, estado, control;
};
// Un unico TD. Nunca cruzar 64 KiB por TRB. El marcador solicitado se
// redondea al Max Packet Size del EP, sin dividir la transferencia USB.
// Solo IOC final: controladores como QEMU entregan un evento por TD completo.
static inline int xhci_bulk_planificar(uint64_t fisica,uint32_t longitud,
        uint32_t primero,uint16_t paquete,int es_in,struct xhci_bulk_tramo plan[XHCI_BULK_PLAN_MAX]) {
    if(!plan||!longitud||!paquete||paquete>1024||fisica>UINT64_MAX-longitud)return -3;
    uint32_t marca=0;
    if(primero&&primero<longitud) {
        uint64_t redondeado=((uint64_t)primero+paquete-1)/paquete*paquete;
        if(redondeado<longitud)marca=(uint32_t)redondeado;
    }
    uint32_t offset=0,n=0;
    while(offset<longitud) {
        if(n==XHCI_BULK_PLAN_MAX)return -3;
        uint64_t phys=fisica+offset;
        uint32_t len=65536U-(uint32_t)(phys&65535U);
        if(len>longitud-offset)len=longitud-offset;
        if(marca>offset&&len>marca-offset)len=marca-offset;
        uint32_t fin=offset+len;
        uint32_t paquetes=((uint64_t)longitud+paquete-1)/paquete-fin/paquete;
        if(paquetes>31)paquetes=31;
        if(fin==longitud)paquetes=0;
        plan[n].fisica=phys;plan[n].longitud=len;
        plan[n].estado=len|(paquetes<<17);
        plan[n].control=(1U<<10)|(es_in?(1U<<2):0)|
            (fin<longitud?(1U<<4):0)|(fin==longitud?(1U<<5):0);
        offset=fin;++n;
    }
    return (int)n;
}

// Error y short incompleto terminan el TD: no esperar eventos inexistentes.
static inline int xhci_bulk_resultado_evento(uint8_t codigo,uint32_t residuo) {
    if(codigo!=1&&codigo!=13)return codigo?-(int)codigo:-1;
    if(codigo==13&&residuo)return -13;
    return 0;
}
#endif
