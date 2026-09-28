#include "fpu.h"
#include <stdint.h>

static int g_sse2_lista;
x86_64_capacidades_simd x86_64_fpu_capacidades_cpu(void) {
    uint32_t a,b,c,d,max; x86_64_capacidades_simd r={0};
    __asm__ volatile("cpuid":"=a"(max),"=b"(b),"=c"(c),"=d"(d):"a"(0),"c"(0));
    __asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(1),"c"(0));
    r.sse2=(d&(1u<<26))!=0;r.xsave=(c&(1u<<26))!=0;
    /* XGETBV sólo es legal con OSXSAVE. CPUID.AVX2 solo no autoriza YMM. */
    if((c&((1u<<27)|(1u<<28)))==((1u<<27)|(1u<<28))) {
        __asm__ volatile("xgetbv":"=a"(a),"=d"(d):"c"(0));r.ymm_habilitado=(a&7)==7;
    }
    if(max>=7){__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(7),"c"(0));r.avx2=(b&(1u<<5))!=0;}
    return r;
}
int x86_64_fpu_sse2_lista(void) {return __atomic_load_n(&g_sse2_lista,__ATOMIC_ACQUIRE);}

void x86_64_fpu_iniciar(void) {
    uint32_t a,b,c,d;
    __asm__ volatile("cpuid" : "=a"(a),"=b"(b),"=c"(c),"=d"(d) : "a"(1),"c"(0));
    const uint32_t requeridos=(1u<<24)|(1u<<25)|(1u<<26);
    if((d&requeridos)!=requeridos)return;
    uint64_t cr0, cr4;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~((uint64_t)1 << 2); /* EM: no emular x87 */
    cr0 &= ~((uint64_t)1 << 3); /* TS: no bloquear FPU tras cada IRQ */
    cr0 |=  ((uint64_t)1 << 1) | ((uint64_t)1 << 5); /* MP, NE */
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");

    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= ((uint64_t)1 << 9) | ((uint64_t)1 << 10); /* OSFXSR, OSXMMEXCPT */
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4) : "memory");

    const uint32_t mxcsr = 0x1F80;
    __asm__ volatile ("fninit; ldmxcsr %0" : : "m"(mxcsr) : "memory");
    __atomic_store_n(&g_sse2_lista,1,__ATOMIC_RELEASE);
}
