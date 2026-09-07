# vesta_alloc

*[English version](README.md)*

Un asignador de memoria del anfitrion para C y C++: listas de libres por clase
de tamano y por hilo, reservas grandes servidas con tramos, una arena de golpe
para trabajo acotado a una fase, y un mecanismo de etiquetas que contesta *para
que* esta reservando un programa.

Reemplaza `operator new` y `operator delete` globales y, si se le pide,
`malloc` y su familia tambien, de forma que el codigo de terceros que uno no
escribio ni puede recompilar reserve tambien de aqui.

No depende de nada mas que del sistema operativo: ni de librerias de terceros,
ni de ninguna parte del proyecto para el que se escribio.

> **Estado.** En desarrollo. Las interfaces de abajo funcionan y tienen
> pruebas, pero todavia no ha habido una version estable: los nombres pueden
> cambiar, y no se publican cifras de rendimiento porque describirian un blanco
> movil. `bench/` las reproduce en tu maquina, que es el unico sitio donde
> significan algo.

## Empezar

```cpp
#include "util/alloc/host_allocator.h"

int main() {
    // No hay nada que inicializar: `new` y `delete` ya vienen aqui.
    auto *p = new int[1024];
    delete[] p;

    // O la interfaz explicita, que tambien puede usar C.
    void *raw = util::host_alloc(64);
    util::host_free(raw);
}
```

Desde C:

```c
#include "util/alloc/host_allocator_c.h"

void *p = vesta_host_alloc(64);
vesta_host_free(p);
```

## Instalar

Como parte de otro proyecto CMake:

```cmake
add_subdirectory(ruta/a/vesta_alloc)
target_link_libraries(tu_objetivo PRIVATE vesta_alloc)
```

Suelto:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Construido suelto genera ademas `libvesta_alloc.a`, los ejemplos y los bancos
de medida. `-DVESTA_ALLOC_BUILD_SHARED=ON` anade la libreria dinamica; lee las
dos notas de abajo antes de usar cualquiera de las dos.

### Enlazar el archivo estatico

**Lee esta.** De un archivo estatico el enlazador solo saca los objetos que
alguien referencia *por su nombre*, y nadie referencia `operator new` por su
nombre: la llamada la genera el compilador. El objeto que lo define no se
extrae, el programa se queda en silencio con el asignador del sistema y **no
falla nada**: sale un binario que funciona y va mas lento, que es el peor modo
de fallo que hay.

Por eso el objetivo por defecto es una **libreria de objetos**, cuyos objetos
entran siempre. Si enlazas el archivo, forzalo:

| herramientas | bandera |
| :--- | :--- |
| GNU ld, lld | `-Wl,--whole-archive libvesta_alloc.a -Wl,--no-whole-archive` |
| MSVC | `/WHOLEARCHIVE:vesta_alloc.lib` |
| Apple ld | `-force_load libvesta_alloc.a` |

Para confirmar que surtio efecto, llama a `util::host_alloc_active()`, o ejecuta
con `VESTA_HOST_ALLOC_STATS=1` y mira si el resumen de salida cuenta alguna
reserva.

### La libreria dinamica

Un ejecutable trae su propio `operator new` y no lo cede a una libreria cargada
despues, asi que **el reemplazo no es fiable desde un objeto compartido**. La
interfaz en C (`vesta_host_alloc` y companyia) funciona en los dos casos. Por
eso la construccion dinamica viene apagada.

## El asignador

| | |
| :--- | :--- |
| `util::host_alloc(n)` | Reservar. Los tamanos pequenos salen de una lista por hilo. |
| `util::host_alloc_zeroed(n)` | Lo mismo, a cero, sin escribir memoria que el sistema ya puso a cero. |
| `util::host_alloc_aligned(n, a)` | Sobre-alineada. Se suelta con `host_free_aligned`. |
| `util::host_realloc(p, n)` | Crece en el sitio cuando la forma lo permite. |
| `util::host_free(p)` | Soltar, desde cualquier hilo, incluido uno que no reservo. |
| `util::host_usable_size(p)` | Cuanto del bloque se puede usar de verdad. |
| `util::host_alloc_stats()` | Contadores: reservas, bytes, trozos, reparto por tamano. |

Las peticiones de hasta unos pocos kibibytes salen de listas por hilo sin
sincronizar nada. Las mayores vienen de tramos -- rachas de trozos seguidos que
se parten y se juntan --, y por encima de eso del sistema operativo.

Que un bloque sea nuestro se sabe con dos comparaciones contra los limites de
la region: sin tablas, sin cerrojos y sin anadir nada al camino de liberar.

`util::ScratchArena` es la otra forma: una arena de golpe para memoria que
muere junta. No recicla, que es justamente por lo que es rapida, y esta
documentada como insegura de compartir entre hilos a proposito -- quien la usa
garantiza la exclusividad en vez de pagar un cerrojo en cada reserva.

## Etiquetas de proposito

Una arena sirve una reserva mucho mas barata que un asignador general, pero
solo para reservas que mueren pronto y no crecen. Equivocarse ahi sale caro y
callado: un contenedor que crece abandona sus buferes viejos, y una arena no
recupera nada.

Asi que un proposito tiene dos ejes, y `0` significa *no se* en los dos:

```cpp
util::AllocScope fase{{util::AllocUse::Medium, util::AllocShape::Growing}};
// todo lo que este hilo reserve hasta que acabe el ambito se cuenta ahi,
// incluidos `std::string` y `std::vector`, que no pueden declarar nada
```

Que lo desconocido sea el valor por defecto no es decoracion: el estado por
hilo es un POD puesto a cero al arrancar, asi que no cuesta inicializacion, y
ademas es honesto -- lo que el informe ensena como desconocido es, literalmente,
lo que todavia no se ha clasificado.

Un ambito vale para **su propio hilo**. Si el trabajo se reparte, hay que leer
la etiqueta en el hilo que reparte y volver a ponerla en el trabajador;
`examples/purpose_tags.cpp` ensena el patron.

## Reemplazar `malloc`

`operator new` esta reemplazado, asi que todo `new` del programa ya llega aqui.
Las entradas en C no tenian nada equivalente, y ese hueco nunca fue solo de C:
un `.cpp` que llama a `malloc` directamente se iba al sistema igual.

Asi que `malloc`, `calloc`, `realloc` y `free` se redirigen **al enlazar**, con
`-Wl,--wrap=`, y las banderas viajan en el objetivo como `INTERFACE`: quien
enlaza `vesta_alloc` las hereda sin tener que saber que existen. Del hecho de
renombrar en el enlace FINAL, y no libreria a libreria, salen dos propiedades:

- **Alcanza a todo objeto del enlace**, venga de donde venga. El codigo de
  terceros que uno no escribio queda cubierto sin tocarle una linea y sin
  comprobar si la version que uno tiene ofrece un gancho de asignador.
- **El lenguaje de quien llama da igual.** Un `.c` y un `.cpp` que llaman a
  `malloc` son la misma referencia pendiente cuando el enlazador los ve.

Con un enlazador sin `--wrap` (el de Apple, el de Microsoft) se avisa al
configurar, en vez de generar una linea de enlazado que falla por una opcion
desconocida.

Los bloques ajenos se tratan, no se suponen imposibles: cuando esto entra en
vigor el runtime de C ya ha reservado en su arranque, y todo lo que devuelve
para que lo suelte quien llama llega como un bloque que este asignador no hizo.
El `free` interpuesto lo reconoce y lo devuelve al suyo. La regla de dentro no
se mueve: `host_free` sigue tratando un puntero ajeno como el error duro que es.

### Entradas alineadas

`posix_memalign`, `aligned_alloc`, `memalign` y, en Windows, la familia
`_aligned_*` entera pasan por el mismo mecanismo. Una reserva alineada es una
reserva, y dejarla fuera daria un informe al que le falta justamente la memoria
de los tipos que piden linea de cache o pagina.

En POSIX ese bloque se suelta con el `free` de siempre, asi que tiene que ser
reconocible por si mismo: se sirve como un tramo, cuya cabecera `free` ya
encuentra enmascarando. Sin marca, sin tabla lateral y sin anadir nada al
camino de liberar.

### Alcanzar el interior del runtime de C

Lo que la libreria de C reserva *dentro de si misma* y devuelve -- `strdup`,
`getline`, `asprintf` -- nunca es una referencia pendiente, asi que ningun
enlazador puede renombrarlo. Cerrarlo necesita un mecanismo distinto en cada
sistema, y los dos vienen encendidos:

| | |
| :--- | :--- |
| **ELF** | `VESTA_ALLOC_DEFINE_MALLOC` -- definir el simbolo. Una definicion en el ejecutable gana a la de la libreria de C, y las llamadas internas de esta salen por la PLT. |
| **Windows** | `VESTA_ALLOC_HOOK_MSVCRT` -- escribir un salto en la entrada de `msvcrt!malloc`. Una DLL no tiene PLT y una llamada interna no sale de ella, asi que el codigo es el unico sitio que queda. Solo se toca el runtime de C. |

Ninguno convive con `--wrap` para el mismo simbolo: con los dos puestos, el
enlazador resuelve `__real_malloc` con la unica definicion que hay y la primera
reserva se llama a si misma. La construccion quita el renombrado justo de los
simbolos que el otro mecanismo define.

Los dos se pueden apagar, y no por cortesia: sin algo con que comparar no hay
forma de saber si suman.

## Informes de reservas

Con el apuntado encendido, el asignador guarda quien reservo, cuanto y para
que, en una tabla por hilo, sin cerrojos y sin reservar para hacerlo.

```cpp
util::AllocSite sitios[64];
unsigned n = util::alloc_sites_snapshot(sitios, 64);
```

### Nombres, ficheros y modulos

Un desplazamiento no es una respuesta. La libreria lee su propia informacion de
depuracion y su propia tabla de simbolos, asi que un informe sale con nombres
sin que nadie enlace una libreria de simbolos:

```cpp
vesta_alloc_set_symbol_resolver(vesta_self_resolver);
vesta_alloc_write_csv("informe/");
```

Dos lineas, y las mismas dos desde C -- `examples/symbol_report.cpp` y
`examples/c_symbol_report.c` son el mismo programa escrito dos veces, porque
"la libreria sabe nombrar sus propias direcciones" valdria la mitad si fuera
una capacidad de C++.

El resolutor intenta tres cosas, en orden, y cada una dice lo que no pudo en
vez de inventarselo:

| | da |
| :--- | :--- |
| La informacion de depuracion (DWARF) | la cadena de inline entera: funcion, fichero, linea |
| La tabla de simbolos | un marco, un nombre, sin fichero |
| Los rangos de funcion (`.pdata`, `st_size`) | no es un nombre -- es donde empieza la funcion, que aun asi agrupa los sitios de una misma funcion |

Y pregunta primero **de quien** es la direccion. Los tres leen la imagen propia,
asi que una direccion de una libreria del sistema saldria si no con un nombre de
nuestra tabla -- y un nombre equivocado es peor que ninguno, porque el que falta
hace una pregunta y el equivocado la cierra. De un modulo ajeno los simbolos se
leen de su fichero, no solo de lo que exporta.

### El informe como dato

`vesta_alloc_write_csv` escribe seis ficheros CSV, y `tools/alloc_tree` los
convierte en una pagina: un arbol ordenable que pliega por pila de llamadas,
modulo, fichero o proposito, con el reparto de tamanos por sitio, dos idiomas
de interfaz, y filtros por ambito ("solo mis llamadas") y por lenguaje (C o
C++). En la pagina viaja siempre todo lo medido -- el filtrado ocurre ahi,
porque decidir que mirar es una forma de mirar y no una forma de exportar.

## Primitivas de memoria

`memcpy` y `memset` no se le piden a la libreria de C. Eran los dos ultimos
simbolos sin resolver: sin ellos el asignador corre donde no hay libc. La
segunda razon son los tamanos pequenos, que en un asignador son el caso comun.

La disposicion es lo importante: **anadir una arquitectura es crear una carpeta
y una rama en el despachador**, sin tocar nada mas.

```text
util/mem/vesta_memcpy.h      lo unico que se incluye desde fuera
util/mem/vesta_memset.h
util/mem/mem_config.h        que se compila y por que
util/mem/mem_inline.h        tamanos pequenos: sin ISA, sin bucle, sin llamada
util/mem/x86/                un fichero por micro-ISA (SSE2, AVX2, ERMS)
util/mem/generic/            donde todavia no hay carpeta propia
```

El despacho va de lo mas barato a lo mas caro: bloques solapados sin bucle en
los tamanos mas pequenos, luego un numero fijo de movimientos direccionados
desde los dos extremos, luego un bucle vectorial con el destino alineado antes
de entrar, y por ultimo `rep movsb`, donde el trabajo lo hace el microcodigo.

Hay dos entradas por operacion, y la diferencia es si puede haber una llamada:

| | |
| :--- | :--- |
| `vesta_memcpy`, `vesta_memset` | Despachan por CPU. Una llamada, que se amortiza a partir de cierto tamano. |
| `..._inline` | **No llaman a nadie nunca.** Se quedan en el camino base para que el compilador pueda meterlas en linea: una funcion compilada para una ISA mas ancha no se puede meter dentro de otra que no. |
| `..._noinline` | Una llamada y nada mas. En tamanos grandes la expansion en linea son cientos de instrucciones en CADA sitio de llamada. |

Son **cabeceras de C, no de C++**: para que una dependencia en C no pague una
llamada, su compilador tiene que ver el cuerpo. En C++ estan ademas como
`util::vesta_memcpy` y companyia, y como ayudantes con TIPO que reciben un
objeto en vez de una cuenta de bytes:

```cpp
util::vesta_memcopy(&dst, &src);          // UN objeto
util::vesta_memcopy(v_dst, v_src, count); // `count` objetos
util::vesta_memfill(&cabecera, 0);
```

`examples/c_mem_ops.c` se compila **como C**, que es lo que mantiene honestas a
las cabeceras en eso.

## Configuracion

### Opciones de CMake

| | por defecto | |
| :--- | :--- | :--- |
| `VESTA_ALLOC_INTERPOSE_MALLOC` | on | `malloc` y companyia son este asignador, via `-Wl,--wrap`. |
| `VESTA_ALLOC_DEFINE_MALLOC` | on (ELF) | Alcanzar tambien lo que la libreria de C reserva por dentro. |
| `VESTA_ALLOC_HOOK_MSVCRT` | on (Windows) | Lo mismo, parcheando las entradas del runtime de C. |
| `VESTA_ALLOC_BUILD_SHARED` | off | Construir tambien la libreria dinamica. Ver la nota de arriba. |
| `VESTA_ALLOC_SIZE_HISTOGRAM` | on | Compilar el reparto por tamano. |
| `VESTA_ALLOC_SPAN_CACHE_SLOTS` | 32 | Cuantos tamanos de tramo se guarda un hilo para si. |
| `VESTA_ALLOC_SPAN_CACHE_BYTES` | 2 MiB | Y cuanta memoria puede retener como mucho. |

### Variables de entorno

| | |
| :--- | :--- |
| `VESTA_HOST_ALLOC_STATS=1` | Imprimir un resumen al salir: cuentas, bytes comprometidos, reparto por tamano y por proposito. |
| `VESTA_HOST_ALLOC_SITES=1` | Apuntar ademas de donde viene cada reserva. Implica `..._STATS`. Es la que instala el salto sobre `operator new`; sin ella esas entradas no se tocan. |
| `VESTA_HOST_ALLOC_CSV=<dir>` | Escribir el informe como CSV en esa carpeta. |

Puesta, no vacia y distinta de `0` significa encendida.

Cada una se lee **una vez, en la primera reserva**, y del bloque de entorno que
el sistema operativo le dio al proceso -- no por `getenv`, que lee una copia que
el runtime de C monta al arrancar. La primera reserva puede ocurrir antes de que
esa copia exista: cualquier global cuyo constructor pida memoria llega antes que
`main`. Leyendo el bloque del sistema no hay un "demasiado pronto", y es tambien
por lo que esta libreria no necesita nada del runtime de C para contestar.

Como la respuesta se toma antes de reservar nada, ponerlas desde dentro del
programa mas tarde no tiene efecto. Encender la medicion a mitad dejaria en
silencio fuera del informe todo lo reservado en el arranque.

## Seguridad entre hilos

Cada funcion documenta cual de estas tres es, porque la distincion es el diseno:

- **Segura** -- llamala desde donde quieras.
- **Segura por reparto** -- no sincroniza *porque* cada hilo solo toca lo suyo.
  `host_alloc` y `host_free` son esto: el camino rapido no sincroniza nada.
- **No segura, a proposito** -- las primitivas de las listas de libres y todo lo
  de `ScratchArena`. Reservar ahi son dos cargas y un almacen; un cerrojo
  costaria mas que el trabajo que protege, asi que quien llama garantiza la
  exclusividad.

Soltar desde un hilo que no reservo esta soportado del todo: el bloque va a una
pila sin cerrojos del hilo que lo reservo, que recoge la pila entera en un solo
intercambio cuando se queda sin bloques.

## Sistemas soportados

| | |
| :--- | :--- |
| Linux, x86-64 | GCC y Clang. Probado. |
| Windows, x86-64 | MinGW (GCC). Probado. |
| macOS | No soportado: su enlazador no tiene `--wrap`, y el asignador no se ha construido ahi. |
| Otras arquitecturas | Las primitivas de memoria caen al camino generico; nada mas depende de la arquitectura. |

C++17 para la libreria; la interfaz en C es C99.

## Limitaciones

- **No ha habido ninguna version.** Los nombres y la disposicion pueden cambiar.
- **La libreria dinamica no puede reemplazar `operator new` de forma fiable**
  -- ver arriba.
- **macOS no esta soportado.**
- **Un informe necesita informacion de depuracion para poner nombres.** Sin
  ella un sitio sale como el desplazamiento donde empieza su funcion, lo que
  agrupa los sitios de una misma funcion pero no los nombra.
- **El asignador no le devuelve memoria al sistema.** Los trozos liberados se
  reusan, no se desmapean, asi que el pico de memoria es el pico que el
  programa alcanzo.
- Donde pierde contra un buen asignador del sistema, y por que, lo reproduce
  `bench_vs_malloc`, que imprime los casos que pierde igual
  que los que gana.

## Licencia

**MIT** -- ver `LICENSE`. Usalo, distribuyelo, cambialo, vendelo; solo conserva
el aviso de copyright.
