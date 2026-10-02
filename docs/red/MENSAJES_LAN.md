# Prueba de mensajes TAEK â†” Windows

Fecha: 2026-10-02. Alcance fÃ­sico: laptop i7 de octava generaciÃ³n.
Windows Ethernet: 192.168.18.146; laptop TAEK: 192.168.18.135.

ISO: `build/taek-os-h96-2026-10-02.iso`.
SHA-256: `fecf820f7893264cf60f8c784bb7ccc2f94e59b5660a8b9ec767792fd96a3fee`.
Las ISO anteriores se conservan. Estos comandos requieren arrancar esta ISO nueva.

## Uso

En el PC receptor (Windows / Linux), ejecutar un listener TCP en el puerto 9151 (o un script receptor configurado para escuchar en ese puerto con `SO_EXCLUSIVEADDRUSE`):

El receptor escucha en TCP 9151, admite la IP de la laptop TAEK y procesa
cada mensaje entrante bajo el protocolo `TAEKMSG1`.

En TAEK:

```text
red dhcp
transmitir hola desde taek
```

En Windows:

```text
transmitir hola i7, recibido
```

En TAEK:

```text
recibir
```

Debe aparecer `Windows: hola i7, recibido`. Una segunda consulta debe mostrar
`No hay respuestas nuevas de Windows`. Repetir otro mensaje en ambos sentidos.
En Windows `salir` termina la terminal y el receptor.

Si cambia la direcciÃ³n de Windows, configurar en TAEK:

```text
transmitir destino 192.168.18.146 9151
```

Si DHCP cambia la IP de la laptop o del PC receptor, configurar el receptor permitiendo la nueva IP del emisor y actualizar el destino en TAEK (`transmitir destino <IP-PC> 9151`).

## Comportamiento y validaciÃ³n

Mensajes de una lÃ­nea, mÃ¡ximo 240 bytes UTF-8. Cola Windows de 100 respuestas
y 100 mensajes entrantes pendientes de revisiÃ³n; todo en RAM. Reiniciar la
terminal descarta sus colas. `recibir` consulta explÃ­citamente, sin espera indefinida.
Timeout TCP total de 5 segundos por intercambio; recibir usa consulta y ACK.
Un ACK perdido conserva el texto en TAEK y permite confirmar al repetir `recibir`.
Enviar otra vez manualmente un mensaje puede duplicarlo si su confirmaciÃ³n se perdiÃ³.

Protocolo: cabecera big-endian `!8sIIQ` de 24 bytes y campo de texto de 240 bytes,
rellenado con ceros. Request `TAEKMSG1`, operaciones enviar=1, consultar=2,
confirmar=3. Response `TAEKRSP1`: recibido=1, vacÃ­o=2, mensaje=3,
confirmado=4, cola llena=5. ID de respuesta de 64 bits; el servidor conserva
la cabeza de la cola hasta ACK y acepta el Ãºltimo ACK repetido.

Verificado: cliente C real con ASan/UBSan; sockets Windows con fragmentaciÃ³n,
texto UTF-8, FIFO/lÃ­mites, peer ajeno, trama invÃ¡lida/truncada y ACK repetido.
QEMU UEFI con kernel real y e1000e: dos mensajes hacia Windows, dos respuestas
hacia TAEK y consulta vacÃ­a. Los logs y resultado se guardan junto a la ISO.
Ensayo fÃ­sico i7 â†” PC confirmado el 2026-10-02: Windows dejÃ³ en cola
`Hola SofiOwOidk, te saludo desde windows`, TAEK 192.168.18.135 consultÃ³ y
confirmÃ³ la respuesta #1; el usuario confirmÃ³ que se mostrÃ³. DespuÃ©s el receptor
registrÃ³ `hola windows` desde TAEK. Evidencia preservada en
`docs/red/h96-evidencia/`.

Se encontraron dos receptores simultÃ¡neos en 9151. El receptor corregido usa
SO_EXCLUSIVEADDRUSE en Windows y rechaza un segundo arranque. Se verificÃ³
con sockets reales que otro receptor no puede apropiarse del mismo puerto.
Se fijÃ³ ademÃ¡s `transmitir destino 192.168.18.146 9151` antes del ensayo exitoso.
Los eventos del receptor quedan en `build/mensajes-lan/terminal-*.jsonl`.

No se aÃ±adiÃ³ telemetrÃ­a continua ni carga de drivers. El receptor anterior
de snapshots en 9150 se detuvo para centrarse en esta prueba.
