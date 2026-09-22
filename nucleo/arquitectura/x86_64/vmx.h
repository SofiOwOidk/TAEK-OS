#ifndef ARQUITECTURA_X86_64_VMX_H
#define ARQUITECTURA_X86_64_VMX_H

#include <stdint.h>

int  vmx_soportado(void);
int  vmx_iniciar(uint64_t base_fisica_kernel, uint64_t base_virtual_kernel);
int  vmx_esta_activo(void);

#endif // ARQUITECTURA_X86_64_VMX_H
