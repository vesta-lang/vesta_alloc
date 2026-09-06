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

GPLv2 -- ver `LICENSE`.  **Ojo**: a diferencia de la licencia de VestaVM, aqui
NO hay excepcion de salida del compilador (esto no compila nada) ni de OpenSSL
(no se enlaza con el).  Y la GPLv2 contagia al enlazar, asi que si la intencion
es que esta biblioteca se pueda reusar libremente hace falta anadir de forma
explicita una excepcion de enlace o pasar a LGPL.  Esta escrito en `LICENSE`
como decision pendiente, no como algo dado por supuesto.
