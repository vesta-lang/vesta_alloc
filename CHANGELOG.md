# Historial de cambios

*[English version below](#changelog-english)*

Formato inspirado en [Keep a Changelog](https://keepachangelog.com/es-ES/).
Las versiones siguen [SemVer](https://semver.org/lang/es/).

Cada entrada dice **por que**, no solo que.  Un historial que solo enumera es
un `git log` peor escrito; lo que hace falta saber es que problema habia.

---

## [Sin publicar]

### Anadido

- **`vesta_memcpy` y `vesta_memset`** (`util/vesta_memcpy.h`,
  `util/vesta_memset.h`, y las implementaciones en `util/mem/`).  Copiar y
  rellenar sin llamar a la biblioteca C: tipos vectoriales con despacho por
  capacidad de la CPU en tiempo de ejecucion, y por debajo de 16 bytes bloques
  solapados EN LINEA, sin llamada y sin bucle.

  **Una carpeta por arquitectura y un fichero por micro-ISA**
  (`mem/x86/sse2_memcpy.h`, `mem/x86/avx2_memset.h`, `mem/generic/scalar_*.h`).
  No es orden por el orden: con todo en un fichero, cada extension nueva lo
  ensucia mas y anadir ARM obliga a tocar el codigo de x86.  Asi, anadir NEON es
  crear `mem/arm/` y una rama en el despachador.

  **Dos entradas por operacion**: la que despacha por CPU, y
  `vesta_memcpy_inline` / `vesta_memset_inline`, que NO llaman a nadie nunca --
  se quedan en el camino base, porque una funcion con `target("avx2")` no se
  puede meter en linea en otra que no lo lleve, y con tamano constante lo
  expande el compilador --.

  **Son cabeceras de C**, valen en los dos lenguajes.  Para que una dependencia
  en C no pague una llamada, su compilador tiene que VER el cuerpo; una capa en
  C++ con envoltorio en C daria justo el coste que se esta quitando.
  `examples/c_mem_ops.c` se compila como C y es lo que lo comprueba.

  Con esto **la libreria ya no deja ni un simbolo de la libc sin resolver por
  memoria**: `memcpy` y `memset` eran los dos ultimos, y venian de `host_realloc`
  y de `host_alloc_zeroed`.  Eso es lo que hace falta para que el asignador
  pueda funcionar donde no hay libc.

  No es codigo nuevo: las dos implementaciones ya existian en el compilador de
  VestaVM, en dos sitios distintos y ninguno de los dos era el suyo -- la copia
  en un `simd_copy.h` suelto, y el relleno DENTRO del `.cpp` del interprete, sin
  cabecera, donde no lo podia usar nadie mas.  Un hecho, un productor.

## [1.0.0] -- 2026-09-06

Primera version como biblioteca separada.  Hasta aqui vivia dentro del
compilador de VestaVM, en `src/util/` e `include/util/`.

### Anadido

- **Reservas grandes servidas por nosotros.**  Todo lo que pasaba de 2 KiB se
  le pedia al asignador del sistema.  Ahora lo sirve un TRAMO de trozos de la
  propia region.  Medido el reparto antes de decidir la estructura: el 90% de
  las grandes son de 64 KiB o menos y la cola llega a 16 MiB, asi que no
  compensaba ni tratarlas como clases -- la fragmentacion se come la ganancia
  -- ni pedir cada una al sistema, que seria una llamada por reserva.
- **Etiquetas de proposito** (`AllocTag`, `AllocScope`) en dos ejes: cuanto vive
  y si crece.  Dos y no uno porque gobiernan decisiones distintas, y una arena
  gana en `(instantaneo, fijo)` y PIERDE en cualquier cosa que crezca.
  Contarlas sale a **coste cero**: el contador de totales que ya existia se
  sustituyo por una tabla indexada por la etiqueta, asi que es el mismo
  incremento de antes.
- **Capa en C** (`host_allocator_c.h`): `vesta_host_alloc`, `calloc`, `realloc`,
  `free`, `usable_size` y las etiquetas.  Es lo que hace falta para enchufarlo
  a los ganchos de asignador de Capstone, SQLite, OpenSSL o miniz, cuya memoria
  hasta ahora era invisible.
- **`os_memory.h`**: la unica capa que habla de memoria con el sistema, con
  apalabrar y entregar SEPARADOS.  Tambien contesta cuanto se puede apalabrar y
  cuanta memoria tiene la maquina, en vez de que cada sitio lo suponga -- habia
  **tres reimplementaciones** sueltas de esa misma pregunta.
- **Cache compartido con cerrojo** para los hilos que se quedan sin
  identificador propio.  Antes esos hilos se salian del asignador y se iban a
  `malloc` PARA SIEMPRE.
- **Se detecta y se avisa** de una liberacion de algo que no reconocemos
  (puntero malo, doble liberacion, memoria pisada).  Antes se volvia en
  silencio.
- Contador aparte de las reservas que hay que ceder al sistema **por no poder
  servirlas**, separado de las que van ahi por diseno.
- Tres modos (Release, Profile, Debug), cinco pruebas, tres ejemplos y dos
  bancos de medida.  El banco de reservas vuelve a derivar en cada maquina la
  tabla de costes con la que se eligio el tamano de la region.

### Cambiado

- **El camino rapido va EN LINEA**, y el lento fuera.  `operator new` es
  reemplazable y nunca se puede meter dentro de quien llama, pero si puede no
  llamar a nadie a su vez -- y no era el caso.  Medido: **-40%** por operacion
  (5,48 -> 3,28 ns, A/B intercalado en los dos ordenes, tres rondas).
- `ThreadSlot::get` ya no llama fuera para preguntar si el TEB esta validado.
  Estaba escondida: se consultaba en CADA acceso, asi que meter el asignador
  dentro de `operator new` no habria quitado la llamada, solo la habria
  cambiado de sitio.
- Las funciones del camino caliente llevan `[[gnu::always_inline]]`.  Con solo
  `inline`, GCC descartaba la sugerencia en `-O2` -- el `asm volatile` de la
  ranura por hilo infla su estimacion de tamano --, asi que la forma del codigo
  dependia del nivel de optimizacion.
- **La region se PIDE, no se supone**: se pide un maximo y se acepta lo que el
  sistema conceda.  Y paso de 1 GiB a 256 GiB, porque compilar 144k lineas ya
  comprometia 760,9 MiB -- el 74% -- y cruzar ese limite no daba un error, daba
  una degradacion muda.  El tamano sale de una tabla medida: apalabrar cuesta
  ~2,7 MiB y ~0,5 ms por TiB, y esos milisegundos caen en el arranque.
- La arena de fase ya no pide memoria por la capa del compilador, que arrastra
  `windows.h` y los flujos de C++ y, al fallar, RESERVABA memoria.
- El reparto de tamanos llega hasta 16 MiB.  Se cortaba en `>16K`, donde caia
  una cuarta parte de las grandes en un solo cajon: con eso no se podia decidir
  como servirlas.
- Los mensajes que imprime la biblioteca van en ingles, al reves que sus
  comentarios: los lee gente de fuera del proyecto.

### Quitado

- La dependencia del registro de mandos del compilador.  Era un `#include` que
  no se usaba, y era lo unico que ataba esto al arbol de VestaVM.

### Licencia

**MIT**, no la GPLv2 del resto de VestaVM.  Sigue siendo parte de la familia del
compilador; lo que cambia es que este componente esta pensado para llevarselo, y
con copyleft no se lo puede llevar casi nadie: arrastraria a la misma licencia a
todo programa que lo enlace.  Al reves no hay friccion -- codigo MIT entra en un
proyecto GPLv2 sin problema --, que es justo lo que hace VestaVM con esto.

De paso desaparecen las dos excepciones que se arrastraban de la licencia del
compilador y que aqui no aplicaban: la de la salida (esto no compila nada) y la
de OpenSSL (no se enlaza con el).  Una excepcion que no aplica no es inofensiva:
invita a creer que ampara.

---

<a name="changelog-english"></a>

# Changelog (English)

## [Unreleased]

### Added

- **`vesta_memcpy` and `vesta_memset`** (`util/vesta_memcpy.h`,
  `util/vesta_memset.h`, implementations under `util/mem/`).  Copy and fill
  without calling the C library: vector types with runtime dispatch on CPU
  capability, and under 16 bytes overlapping blocks INLINE -- no call, no loop.

  **One folder per architecture, one file per micro-ISA**
  (`mem/x86/sse2_memcpy.h`, `mem/x86/avx2_memset.h`, `mem/generic/scalar_*.h`).
  Not tidiness for its own sake: with everything in one file each new extension
  makes it worse, and adding ARM would mean touching the x86 code.  This way,
  adding NEON is creating `mem/arm/` plus one branch in the dispatcher.

  **Two entry points per operation**: the one that dispatches on CPU, and
  `vesta_memcpy_inline` / `vesta_memset_inline`, which never call anybody --
  they stay on the base path, because a function with `target("avx2")` cannot
  be inlined into one without it, and with a constant size the compiler expands
  it.

  **They are C headers**, valid in both languages.  For a C dependency to avoid
  paying a call, its compiler has to SEE the body; a C++ layer behind a C
  wrapper would hand it exactly the cost being removed.
  `examples/c_mem_ops.c` is compiled as C and is what checks it.

  With this the library **no longer leaves a single libc symbol unresolved for
  memory**: `memcpy` and `memset` were the last two, coming from `host_realloc`
  and `host_alloc_zeroed`.  That is what the allocator needs in order to run
  where there is no libc.

  Not new code: both implementations already existed in the VestaVM compiler,
  in two different places and neither was the right one -- the copy in a
  standalone `simd_copy.h`, the fill INSIDE the interpreter's `.cpp` with no
  header, where nobody else could use it.  One fact, one producer.

## [1.0.0] -- 2026-09-06

First release as a separate library.  Until now it lived inside the VestaVM
compiler, under `src/util/` and `include/util/`.

### Added

- **Large allocations served in-house.**  Anything over 2 KiB used to go to the
  system allocator; it is now served by a SPAN of chunks from our own region.
  The size distribution was measured before choosing the structure: 90% of
  large allocations are 64 KiB or under, with a tail reaching 16 MiB -- so
  neither pure size classes nor a per-allocation OS mapping was right.
- **Purpose tags** (`AllocTag`, `AllocScope`) on two axes: how long it lives and
  whether it grows.  Two rather than one because they drive different
  decisions -- an arena wins on `(instant, fixed)` and LOSES on anything that
  grows.  Counting them costs **nothing**: the existing total counter was
  replaced by a table indexed by the tag, so it is the same increment as before.
- **A C layer** (`host_allocator_c.h`): `vesta_host_alloc`, `calloc`, `realloc`,
  `free`, `usable_size` and the tags.  This is what you need to plug into the
  allocator hooks of Capstone, SQLite, OpenSSL or miniz, whose memory was
  invisible until now.
- **`os_memory.h`**: the only layer that talks to the OS about memory, with
  reserve and commit SEPARATE.  It also answers how much can be reserved and
  how much memory the machine has, instead of every caller assuming -- there
  were **three separate reimplementations** of that same question.
- **A shared, locked cache** for threads that run out of their own slot.  Those
  threads used to drop out of the allocator and go to `malloc` FOREVER.
- **Unrecognised frees are detected and reported** (bad pointer, double free,
  overwritten memory).  They used to return silently.
- A separate counter for allocations handed to the system **because we could not
  serve them**, distinct from those that go there by design.
- Three build modes (Release, Profile, Debug), five tests, three examples and
  two benchmarks.  The reservation benchmark re-derives, on any machine, the
  cost table the region size was chosen from.

### Changed

- **The fast path is now inline**, the slow path out of line.  `operator new` is
  replaceable and can never be inlined into its caller, but it can avoid calling
  out itself -- and it did not.  Measured: **-40%** per operation (5.48 -> 3.28
  ns, interleaved A/B in both orders, three rounds).
- `ThreadSlot::get` no longer calls out to ask whether the TEB read is
  validated.  That call was hidden on EVERY slot access, so inlining the
  allocator into `operator new` would have moved the call, not removed it.
- Hot-path functions carry `[[gnu::always_inline]]`.  With plain `inline`, GCC
  discarded the hint at `-O2` -- the `asm volatile` in the thread slot inflates
  its size estimate -- so the shape of the code depended on the optimisation
  level.
- **The region is ASKED for, not assumed**: request a maximum, accept what the
  system grants.  And it went from 1 GiB to 256 GiB, because compiling 144k
  lines already committed 760.9 MiB -- 74% -- and crossing that limit was not an
  error, it was a silent degradation.  The size comes from a measured table:
  reserving costs about 2.7 MiB and 0.5 ms per TiB, and those milliseconds land
  in startup.
- The phase arena no longer gets memory through the compiler's layer, which
  drags in `windows.h` and C++ streams and, on failure, ALLOCATED memory.
- The size histogram now reaches 16 MiB.  It stopped at `>16K`, where a quarter
  of all large allocations landed in a single bucket -- not enough to decide how
  to serve them.
- Messages printed by the library are in English, unlike its comments: they are
  read by people outside the project.

### Removed

- The dependency on the compiler's environment-flag registry.  It was an unused
  `#include`, and the only thing tying this to the VestaVM tree.

### License

**MIT**, not the GPLv2 the rest of VestaVM uses.  This is still part of the
compiler's family; what changes is that this component is meant to be taken
away, and under copyleft almost nobody can take it -- it would drag every
program that links it under the same terms.  The other direction has no
friction: MIT code goes into a GPLv2 project fine, which is exactly what VestaVM
does with this.

That also drops the two exceptions carried over from the compiler's license that
did not apply here: the compiler-output one (this compiles nothing) and the
OpenSSL one (it links no OpenSSL).  An exception that does not apply is not
harmless -- it invites the belief that it covers you.
