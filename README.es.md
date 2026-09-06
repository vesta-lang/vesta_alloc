# vesta_alloc

*[English version](README.md)*

Un asignador de memoria del anfitrion: listas libres por clase de tamano y por
hilo, reservas grandes servidas por tramos, una arena de golpe para el trabajo
de una fase, y un mecanismo de etiquetas para averiguar **para que** reserva de
verdad un programa.

Escrito para el compilador de VestaVM, donde `malloc` y `free` eran el **18,5%
del tiempo de compilar** y estaban repartidos por todos los sitios de llamada --
el mayor de ellos era el 8,7% de esa cifra, asi que ningun arreglo puntual los
movia --.  Cambiar el asignador los mueve todos a la vez.

No depende de nada mas que del sistema operativo.  Ni de bibliotecas de
terceros, ni de ninguna parte del compilador del que salio.

```
     este asignador      3,2 ns/op
     malloc del sistema 45,4 ns/op          ~14x, medido por tests/test_host_allocator
```

## Que hay aqui

| | |
| :--- | :--- |
| `include/util/host_allocator.h` | El asignador.  Reemplaza `operator new`/`delete` globales. |
| `include/util/host_allocator_c.h` | Lo mismo con enlace C, para librerias en C y sus ganchos de asignador. |
| `include/util/host_allocator_layout.h` | La geometria: region, trozos, clases de tamano.  La comparten todos los de abajo. |
| `include/util/alloc_tag.h` | Etiqueta de proposito con dos ejes (vida x fijo-o-creciente). |
| `include/util/scratch_arena.h` | Arena de golpe para memoria que muere junta. |
| `include/util/os_memory.h` | El unico sitio que habla de memoria con el sistema.  Apalabrar y entregar van por separado. |
| `include/util/thread_slot.h` | Un puntero por hilo que NO pasa por la TLS emulada. |
| `include/util/vesta_memcpy.h` | Copiar (y mover con solape) sin llamar a la biblioteca C. |
| `include/util/vesta_memset.h` | Rellenar, igual. |
| `include/util/mem/` | Las implementaciones: **una carpeta por arquitectura, un fichero por micro-ISA**.  Ver abajo. |

### Las primitivas de memoria

`memcpy` y `memset` no se le piden a la biblioteca C.  La razon de peso es que
eran los **dos ultimos simbolos** que quedaban sin resolver: sin ellos, el
asignador funciona donde no hay libc.  La segunda es el tamano pequeno, que en
un asignador es el caso comun: por debajo de 16 bytes esto no llama a nadie
-- bloques solapados, sin bucle -- y ahi gana entre 2x y 4,5x.

Estan repartidas asi, y el reparto es el punto: **anadir NEON es crear
`mem/arm/` y una rama en el despachador**, sin tocar nada de x86.

```
util/vesta_memcpy.h          <- lo unico que se incluye desde fuera
util/vesta_memset.h
util/mem/mem_config.h        que hay compilado, y por que
util/mem/mem_inline.h        menos de 16 bytes: sin ISA, sin bucle, sin llamada
util/mem/x86/x86_vec.h       tipos vectoriales
util/mem/x86/x86_cpuid.h     CPUID y XGETBV, envueltas y nada mas
util/mem/x86/x86_cpu.h       que sabe hacer esta CPU (interpreta lo anterior)
util/mem/x86/sse2_memcpy.h   camino base, el unico que se puede meter en linea
util/mem/x86/sse2_memset.h
util/mem/x86/avx2_memcpy.h   solo si la CPU lo admite
util/mem/x86/avx2_memset.h
util/mem/x86/erms_memcpy.h   `rep movsb`: lo resuelve el microcodigo
util/mem/x86/erms_memset.h
util/mem/generic/scalar_*.h  donde todavia no hay carpeta propia
```

El despacho va de mas barato a mas caro:

| tamano | que hace |
| :--- | :--- |
| < 16 B | bloques solapados: sin bucle, sin llamada |
| 16 - 128 B | hasta ocho movimientos direccionados desde los dos extremos, **sin bucle** |
| 128 B - 2 KiB | bucle vectorial, **con el destino alineado antes de entrar** |
| > 2 KiB | `rep movsb` / `rep stosb`, que lo resuelve el microcodigo |

**Los umbrales salen de medir**, y cada uno lleva su tabla en el fichero donde
vive; no se copiaron de nadie.

Dos de esas decisiones no salieron de un banco sino de **desensamblar glibc**, y
valen la pena por separado: que hasta 128 bytes no haya bucle, y que el bucle
alinee el destino antes de empezar.  Lo segundo es lo que mas pesa -- una
escritura sin alinear que cruza linea de cache se parte en dos, y en un bucle
eso se paga cada vuelta --: en una copia de 1 KiB son 9,6 ns contra 5,2.

Hay **dos entradas por operacion**, y la diferencia es si puede haber una
llamada:

| | |
| :--- | :--- |
| `vesta_memcpy` / `vesta_memset` | Despachan por CPU.  Con AVX2 en la maquina pagan una llamada, que a partir de 32 bytes sale a cuenta. |
| `vesta_memcpy_inline` / `vesta_memset_inline` | **No llaman a nadie, nunca.**  Se quedan en el camino base -- una funcion con `target("avx2")` no se puede meter en linea en otra que no lo lleve -- y con tamano constante lo expande el compilador. |
| `vesta_memcpy_noinline` / `vesta_memset_noinline` | Una llamada y ya.  Con tamanos grandes la expansion en linea son cientos de instrucciones EN CADA SITIO; aqui el sitio de llamada es minimo.  Estan para poder elegir, no para sustituir. |

### Y en C++, la version con TIPO

```cpp
util::vesta_memcopy(&destino, &origen);       // UN objeto
util::vesta_memcopy(v_dst, v_src, cuantos);   // `cuantos` OBJETOS
util::vesta_memfill(&cabecera, 0);
```

`memcpy`/`memset` cuentan **bytes**; `memcopy`/`memfill` cuentan **objetos**.
Los nombres son distintos a proposito: con el mismo nombre, la deduccion de
plantilla elegiria la version con tipo sin que nadie lo escriba y el tercer
argumento cambiaria de unidad en silencio.

Lo que gana con saber el tipo: `sizeof(T)` hace constante el tamano y
`alignof(T)` **quita el prologo que alinea el destino**.  Ese prologo calcula un
desplazamiento en ejecucion, y eso convierte un tamano constante en variable, con
lo que el bucle deja de desenrollarse.  Desensamblado, un relleno de 256 bytes
pasa de 66 instrucciones con 4 ramas a **19 en linea recta**.

Y esto **no se afirma con un cronometro sino con el desensamblado**, porque un
banco tiene ruido y el codigo emitido no.  Instrucciones y ramas de la misma
operacion, con el MISMO numero de bytes, en las dos secciones de
`tests`/`validate`:

| | GCC: C -> C++ | Clang: C -> C++ |
| :--- | :--- | :--- |
| relleno de 256 B | 94/12 ramas -> **12/1** | 49/9 -> **12/1** |
| copia de 256 B | 92/9 -> **17/1** | 63/9 -> **16/1** |
| relleno de 4 KiB | 204/38 -> **123/26** | 137/18 -> **97/10** |
| copia de 1 KiB | 141/24 -> **65/14** | 108/14 -> **58/6** |
| por debajo de 256 B | identico | identico |
| tipo sin alineacion declarada | **identico** | **identico** |

Las tres condiciones se cumplen en los dos compiladores: nunca emite mas, de
256 en adelante emite bastante menos, y cuando el tipo no promete nada sale
**exactamente el mismo codigo** -- el envoltorio no anade nada, que era el
requisito.

Por debajo de 256 las dos son identicas porque la cascada ya se plegaba sola:
no habia nada que ganar, y eso es un resultado, no una decepcion.

**Una trampa que costo encontrar**: un tipo sobrealineado NO puede tener un
tamano que no sea multiplo de su alineacion -- `sizeof` se redondea hacia
arriba, asi que un `alignas(32)` de 50 bytes mide 64 --.  Comparar "50 bytes por
C contra un tipo de 50 declarado" es comparar 50 contra 64, y eso no mide nada.

**Son cabeceras de C**, no de C++.  Para que una dependencia en C no pague una
llamada, su compilador tiene que ver el cuerpo; una capa en C++ con envoltorio
en C daria justo el coste que se esta quitando.  En C++ estan ademas como
`util::vesta_memcpy` y companyia.  `examples/c_mem_ops.c` se compila **como C**
y es lo que comprueba que siga siendo cierto.

Lo que cuesta cada camino, medido contra la libc: `bench_memcpy` y
`bench_memset`.  Las medidas NO se copian a este documento -- una tabla de
nanosegundos aqui no dice en que maquina, en que nucleo ni con que compilador
salio, y esas tres cosas la mueven mas que el propio codigo --: viven en
[`bench/baseline/`](bench/baseline/), una tanda por compilador, con la ficha de
la CPU al lado.  Ahi esta tambien lo que sigue MAL: siete filas donde la version
con tipo pierde contra la de C, que por como esta construida no deberia poder
pasar.

## Construir

Como parte de otro proyecto CMake:

```cmake
add_subdirectory(ruta/a/vesta_alloc)
target_link_libraries(tu_objetivo PRIVATE vesta_alloc)
```

Por su cuenta:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build          # 5 binarios de prueba
./build/vesta_alloc_bench_allocator          # este asignador contra el del sistema
./build/vesta_alloc_bench_reserve_cost       # que cuesta apalabrar direcciones aqui
```

Construido solo genera `libvesta_alloc.a`, las pruebas, tres ejemplos y dos
bancos.  Con `-DVESTA_ALLOC_BUILD_SHARED=ON` sale tambien la biblioteca
dinamica; antes de usarla, leer el aviso de abajo.

## Lo unico que te va a morder

**Esta biblioteca reemplaza `operator new` y `operator delete` globales.**  De
un archivo estatico (`.a`, `.lib`) el enlazador solo saca los objetos que
alguien referencia POR SU NOMBRE, y a `operator new` no lo nombra nadie: la
llamada la genera el compilador.  Asi que el objeto que lo define no se extrae,
tu programa se queda con el asignador del sistema, y **no falla nada**.  Sale un
binario que funciona y va mas lento, que es el peor modo de fallo posible.

Por eso el objetivo por defecto es una **biblioteca de objetos**, que entran
siempre.  Si enlazas el archivo estatico, hay que forzarlo:

```
GNU ld / lld    -Wl,--whole-archive libvesta_alloc.a -Wl,--no-whole-archive
MSVC            /WHOLEARCHIVE:vesta_alloc.lib
Apple ld        -force_load libvesta_alloc.a
```

Para comprobar que de verdad entro: `util::host_alloc_active()`, o ejecutar con
`VESTA_HOST_ALLOC_STATS=1` y mirar si el resumen del final cuenta alguna
reserva.

**La biblioteca dinamica es otra cosa.**  Un ejecutable trae su propio
`operator new` y no lo cede a una DLL o un `.so` que se cargue despues, asi que
ahi el reemplazo no es fiable.  La interfaz en C (`vesta_host_alloc` y
companyia) funciona igual en los dos casos.  Por eso la version dinamica esta
apagada por defecto.

## Donde PIERDE, y por que

Al ejecutar `vesta_alloc_bench_vs_malloc` salen filas marcadas
`<- system wins`.  Estan ahi a proposito: un asignador que solo publica los
casos que gana no esta diciendo nada.  En Linux contra glibc:

| caso | nuestro | glibc | por que |
| :--- | ---: | ---: | :--- |
| `hot` de 1 MiB | ~14 ns | ~12 ns | Camino de tramos: un cerrojo y un recorrido de lista, contra el `mmap` cacheado de glibc. |
| `churn` de 64 KiB | ~11 ns | ~10 ns | Lo mismo, y **cambia de signo entre corridas**: minutos antes marcaba 1,32x a favor y despues 0,91x en contra.  A ese tamano la medida tiene mas ruido que la diferencia. |
| `calloc` de 64 KiB en adelante | ~430 ns / ~7 us | igual | Empate POR CONSTRUCCION: ahi el coste es materializar paginas, que es identico para los dos.  Ninguno puede ser mas rapido en eso. |

Todo lo demas lo ganamos, entre 1,0x y 36x.  Tres cosas cerraron casi todo el
hueco, y cada una esta explicada donde vive:

- **Las clases de tamano llegan ya a 16 KiB.**  Se paraban en 2 KiB, asi que una
  peticion de 4 KiB se llevaba un trozo entero de 64 KiB -- dieciseis veces el
  desperdicio -- y pasaba por el camino de tramos, con cerrojo.  `hot` de 4 KiB
  fue de 6,17 a 1,63 ns, y `churn` de 10,8 a 3,66, por un +6% de memoria.
- **Los tramos se parten y se juntan.**  Con listas de ajuste exacto, un tramo de
  diecisiete trozos no podia servir una peticion de uno, asi que un bufer que
  crece consumia region nueva en cada vuelta.  `realloc` creciendo hasta 1 MiB
  fue de 3.583 a 74 ns, y el pico de 144 a 21 MiB.
- **La ranura por hilo lee el puntero de hilo con una instruccion.**  Antes
  llamaba a `pthread_getspecific` en CADA reserva, y por eso los tamanos
  pequenos perdian por 0,83-0,98x.

En Windows el cuadro es otro: ganamos en todo entre 2x y 500x, porque el
asignador de msvcrt es mucho mas flojo.  Las filas de arriba son un resultado de
LINUX, y glibc es un rival duro.

**Lo que NO es explicacion.**  Ninguno de esos casos es ruido de medida, y
ninguno se arregla diciendo que el banco es injusto.  Dos de ellos son huecos de
diseno de verdad, con arreglo conocido y escrito ahi arriba.

## Seguridad entre hilos

Cada funcion lleva una seccion `@par Hilos` que dice cual de las tres es, porque
la distincion ES el diseno:

- **Segura** -- se llama desde donde sea.
- **Segura por particion** -- no sincroniza nada *porque* cada hilo solo toca lo
  suyo.  `host_alloc` y `host_free` son de estas: el camino rapido no sincroniza
  absolutamente nada.
- **NO segura, a proposito** -- `pop_block`, `push_block` y todo lo de
  `ScratchArena`.  Reservar son dos lecturas y una escritura; un cerrojo ahi
  costaria mas que el trabajo que protege.  Quien llama garantiza la
  exclusividad en su lugar.

Liberar desde un hilo que no reservo esta plenamente soportado: el bloque va a
una pila sin cerrojos del hilo que lo reservo, y ese se la lleva entera de un
golpe la proxima vez que se quede sin bloques.

## Variables de entorno

| | |
| :--- | :--- |
| `VESTA_NO_HOST_SLAB=1` | Apaga el asignador; todo va al del sistema.  Existe para que haya con que comparar -- sin eso no hay forma de saber si un asignador mejora algo. |
| `VESTA_HOST_ALLOC_STATS=1` | Imprime un resumen al salir: cuentas, bytes comprometidos, reparto por tamano pedido y reparto por proposito. |

## Etiquetas de proposito

Una arena de golpe sirve una reserva cinco veces mas rapido que un asignador
general, pero **solo** para las que mueren pronto y no crecen.  Equivocarse ahi
sale caro y en silencio: meter un analisis de rangos en una arena -- donde las
vidas encajaban perfectamente -- llevo el pico de memoria de 2.414 MB a 5.455 MB
(**+126%**) a cambio de un 4% de velocidad, porque los contenedores que crecen
abandonan su bufer viejo y una arena no reclama nada.

Por eso la etiqueta tiene dos ejes, y el `0` significa *no se* en los dos:

```cpp
util::AllocScope fase{{util::AllocUse::Medium, util::AllocShape::Growing}};
// todo lo que reserve este hilo hasta que acabe el ambito se cuenta ahi,
// incluidos los std::string y los std::vector, que no pueden declarar nada
```

Que el `0` sea "no se" no es decoracion: el estado por hilo es un POD puesto a
cero al arrancar, asi que el valor por defecto no cuesta ninguna inicializacion
**y** es honesto -- dice "no lo se" en vez de suponer --.  Lo que el informe
saque como `unknown` es, literalmente, la lista de lo que falta por clasificar.

Un ambito vale para **su** hilo.  Si el trabajo se reparte, hay que leer la
etiqueta en el hilo que reparte y volver a ponerla en el que trabaja;
`examples/purpose_tags.cpp` ensena el patron.

## Usarlo desde librerias en C

Casi todas las librerias en C dejan cambiarles el asignador, y asi su memoria
entra en los mismos contadores que la tuya:

```c
cs_opt_mem mem = { vesta_host_alloc, vesta_host_calloc,
                   vesta_host_realloc, vesta_host_free, vsnprintf };
cs_option(handle, CS_OPT_MEM, (size_t)&mem);
```

La misma forma vale para `sqlite3_config(SQLITE_CONFIG_MALLOC, ...)`,
`CRYPTO_set_mem_functions` y los ganchos `zalloc`/`zfree` al estilo de zlib.
Comprobar que expone la version que llevas vendorizada en vez de darlo por
hecho.

`examples/c_basic.c` y `examples/c_library_hook.c` se compilan **como C**, no
como C++.  Es a proposito: son lo unico que comprueba que
`host_allocator_c.h` sea C de verdad, en vez de limitarse a decirlo.

**Una trampa al llamar desde un programa en C**: la biblioteca es C++, asi que
el enlace final necesita la biblioteca estandar de C++.  Si tu ejecutable solo
tiene fuentes en C, tu sistema de construccion elegira el enlazador de C y
saldran simbolos `std::` sin resolver.  En CMake:

```cmake
set_target_properties(tu_programa_c PROPERTIES LINKER_LANGUAGE CXX)
```

o enlazar con `g++`/`clang++` en vez de con `gcc`/`clang`.

## Licencia

**MIT** -- ver `LICENSE`.  Usala, distribuyela, cambiala, vendela; solo hay que
conservar el aviso de copyright.

Forma parte de la familia de VestaVM, pero con una licencia mas permisiva que la
del compilador, que es GPLv2.  Es deliberado: una biblioteca de proposito
general con licencia copyleft no es reutilizable de verdad, porque arrastraria a
la misma licencia a todo programa que la enlace.  Y en la otra direccion no hay
friccion ninguna: codigo MIT entra en un proyecto GPLv2 sin problema, que es
justo lo que hace VestaVM con esto.
