# Historial de cambios

*[English version below](#changelog-english)*

Formato inspirado en [Keep a Changelog](https://keepachangelog.com/es-ES/).
Las versiones siguen [SemVer](https://semver.org/lang/es/).

Cada entrada dice **por que**, no solo que.  Un historial que solo enumera es
un `git log` peor escrito; lo que hace falta saber es que problema habia.

---

## [Sin publicar]

### Anadido

- **Paginas con PERMISOS y colocadas donde hagan falta: `host_alloc_pages`.**
  Por debajo esto siempre fue un repartidor de arenas; lo unico que estaba fijo
  eran los permisos con que se comprometen sus paginas y que caian donde
  cayeran.  Ahora las dos cosas se piden, y `util::ScratchArena` tambien las
  acepta -- es la misma arena, no una nueva.

  PARA QUE.  El codigo generado alcanza sus datos con desplazamientos de 32
  bits, que cubren +-2 GB; mas alla la referencia no se puede ni emitir.  Asi
  que donde CAE un bloque decide si el codigo que vivira en el funciona.  El JIT
  lo pedia al sistema y acababa **a 16.405 MiB de un dato que estaba a ocho
  bytes de su ancla**, con un "rel32 fuera de rango" que no era un aviso:
  devolvia cero, y ese cero acababa siendo el punto de entrada de un hilo.

  Y PEDIRLO PEGADO NO BASTA, que era lo primero que se probo: con el ancla
  dentro de la region de clases grandes, el recorrido ve 22 regiones y el hueco
  mayor es **CERO** -- la ventana de +-2 GB cabe entera dentro de esos 16 GiB --.
  Es geometria, no mala suerte.  Lo que vale es servir desde DENTRO: los trozos
  salen del mismo cursor que usan las clases grandes y solo cambian los
  permisos, asi que el reparto de datos no se entera -- hay un cliente mas, no
  un mecanismo nuevo -- y la cercania sale sola, porque ese cursor va justo
  detras de todo lo ya entregado.  Medido: de 16.405 MiB a **5 MiB**.

  NO SE FIA DE QUE SALGA BIEN: comprueba la distancia de verdad antes de
  devolver, y si el cursor se alejo mas que la ventana descompromete y cae a
  `os_alloc_near`.  `host_exec_far()` cuenta esas veces, porque fallar aqui es
  mudo por naturaleza -- el bloque vuelve igual y lo que se rompe es un
  desplazamiento, mucho despues y en otro sitio.

- **`os_alloc_near`: reservar CERCA de una direccion que ya existe.**  Recorre
  la ventana region por region y se queda con el hueco MAS CERCANO.  Antes esto
  vivia dentro del generador de codigo, probaba `base +- 2^k` -- treinta puntos
  sueltos, que una reserva grande por en medio deja todos ocupados -- y al
  cambiarlo por un recorrido se quedaba en el primer hueco, o sea en el borde de
  la ventana: 1.920 MiB, sin margen para los datos que no son el ancla exacta.
  Eligiendo el mas cercano: **1,8 MiB**.

  El parametro se llama `anchor` y no `near` porque `windows.h` todavia define
  `near` como una macro vacia, herencia de los punteros de dieciseis bits: un
  parametro con ese nombre desaparece y lo que falla es la linea que lo usa.

- **`malloc` es este asignador tambien DENTRO del runtime de C.**  El
  renombrado al enlazar alcanza toda llamada del enlace y ahi se acaba: lo que
  la libreria de C reserva por dentro y devuelve -- `strdup`, `getline`,
  `_wgetdcwd` -- no es una referencia pendiente, asi que ningun enlazador puede
  tocarla.  Esa memoria salia como cero, y cero se lee como "no hay".

  Se cierra con un mecanismo por sistema, los dos **encendidos por defecto**:
  `VESTA_ALLOC_DEFINE_MALLOC` en ELF (definir el simbolo, como jemalloc: las
  llamadas internas de la libreria salen por la PLT) y `VESTA_ALLOC_HOOK_MSVCRT`
  en Windows (un salto en la entrada de `msvcrt!malloc`, porque una DLL no
  tiene PLT y una llamada interna no sale de ella).  SOLO msvcrt: ni kernel32,
  ni ntdll.

  Tres cosas costaron su vuelta y estan escritas donde se deciden: que las dos
  vias **no conviven** con `--wrap` para el mismo simbolo -- `__real_malloc` se
  resolveria con nuestra propia definicion y la primera reserva se llamaria a
  si misma --; que en Windows los saltos tienen que entrar desde un callback de
  TLS y no desde un constructor, porque los cuatro bloques de diferencia
  incluyen la tabla de `atexit`, que crece con `realloc`; y que en ELF hace
  falta una arena de arranque, porque ser `malloc` significa que te pregunten
  antes de que el asignador haya decidido si esta activo.

- **Las entradas ALINEADAS, en las dos plataformas.**  `posix_memalign`,
  `aligned_alloc`, `memalign` y la familia `_aligned_*` de Windows entera.  En
  POSIX el bloque se suelta con el `free` de siempre, asi que tiene que decir
  lo que es por si mismo: se sirve como un TRAMO, cuya cabecera `free` ya
  encuentra enmascarando -- sin marca, sin tabla lateral y sin una instruccion
  mas en el camino de liberar.

  `_aligned_realloc` y sus parientes se cubren aunque no las llame nadie: son
  las que reciben un puntero YA reservado, y sin cubrirlas una de nuestras
  reservas acababa en el monton del runtime.  Eso no falla en la llamada, falla
  mucho despues y en otro sitio.

- **De QUIEN es una direccion** (`util/symbols/module_symbols.h`).  Todo lo que
  resuelve nombres leia la imagen PROPIA y ninguno comprobaba que la direccion
  lo fuera: una de `libc` no encontraba nada en nuestra tabla y salia con el
  ultimo simbolo que hubiera, `_fini`, con la misma cara que un nombre cierto.
  Un informe se lee como un hecho, y un nombre equivocado es peor que ninguno.

  De un modulo ajeno los simbolos se leen de SU fichero -- `slurp` y
  `build_table` nunca fueron especificos de "uno mismo" --, y lo que da el
  cargador queda de respaldo para un modulo despojado.

- **El volcado de texto tambien resuelve nombres**, y las reservas de otros
  modulos salen en su propia seccion.  Antes el mismo proceso escribia
  `parse_tokens` en el CSV y un desplazamiento crudo en la terminal; y los
  sitios ajenos, con nueve reservas frente a trescientas treinta, no asomaban
  en una lista ordenada por cuenta -- justo lo unico que se acababa de ganar.

- **La pagina del informe filtra por AMBITO y por LENGUAJE**, y los dos ejes se
  combinan: todo / solo mis llamadas plegando hacia mi funcion / solo mis
  llamadas en sentido estricto / solo lo externo, cruzado con C, C++ o sin
  determinar.  "Sin determinar" es una vista propia y no un cajon: sin
  informacion de depuracion no hay fichero, y sin fichero un nombre a secas no
  dice en que lenguaje se escribio -- la pagina lo DICE, y dice como
  arreglarlo.  Lo que un filtro deja fuera se cuenta siempre.

### Cambiado

- **El llamante puede decir CUANTO va a tocar de lo que pide, y con eso se
  cierran las dos filas que se perdian.**  Un `calloc` grande tiene dos
  respuestas correctas y opuestas -- quedarse el bloque y limpiarlo, cuyo coste
  es plano en lo que se lea porque se escribe entero, o pedirle uno fresco al
  sistema, que llega ya a cero y cuesta un fallo de pagina por cada pagina que
  se toque --.  Se cruzan en una FRACCION del bloque, entre 1/16 y 1/4, y NO en
  un tamano: la misma peticion de 8 MiB gana de una forma leida a trozos y de
  la otra leida entera.  Ningun umbral por tamano puede elegir; el unico que lo
  sabe es quien llama.

  `AllocFill` es ese eje y `AllocScope` lo declara.  Medido contra msvcrt sobre
  siete tamanos por tres fracciones, **las 21 filas se ganan**, incluidas las
  dos que estaban abiertas: `calloc` de 1 MiB leido 1/64 pasa a 1,40x a favor y
  el de 8 MiB a 1,36x, donde perdia 3,3x.  Por debajo de `kSparseDirectMin`
  (256 KiB, medido) no se aplica: ahi la llamada al sistema cuesta mas que la
  limpieza que ahorra.

  ES UNA AFIRMACION, no un hecho, igual que la etiqueta de proposito: declarar
  disperso un bloque que luego se lee entero cuesta velocidad y nunca
  correccion, y `host_fill_allocs` dice cuantas fueron por cada rama para poder
  contrastarlo.

  Y VA APARTE DE `AllocTag`, con su cuenta aparte, por una razon que se midio:
  los dos ejes de proposito van empaquetados en cuatro bits para que el byte
  indexe la tabla de contadores, y un tercero ahi la llevaria de 16 ranuras a
  64.  Meter el contador en `HostAllocStats` -- que vive en el cache por hilo
  POR DELANTE del lote caliente -- engordaba la estructura de 1216 a 1280 bytes
  y empujaba el lote 32: solo +0,01 ns, por debajo del suelo del 2% del propio
  banco, pero 64 KiB de estatico y, peor, dejaba el camino caliente atado a los
  contadores.  Con el contador en un global, `host_alloc` y `host_free` salen
  **instruccion por instruccion como estaban**.

  Lo que se temia y NO esta ahi, perfilado con contadores hardware sobre doce
  hilos, tres corridas intercaladas de cada disposicion: `SPLIT_LOADS` y
  `SPLIT_STORES` a cero exacto, todos los `XSNP_*` a cero -- ninguna linea viaja
  entre nucleos, o sea cero comparticion falsa -- y `L3 Bound` al 0,0-0,1%.  Un
  `static_assert` sobre el tamano impide que la estructura vuelva a crecer sin
  que nadie lo mire; se comprobo reproduciendo el fallo.

- **Los tres ejes se pueden NOMBRAR desde C**, que antes no: `vesta_host_push_tag`
  tomaba enteros pelados y el significado vivia en un comentario.  Los valores
  viven ahora en `util/alloc/alloc_tag_c.h` y **solo ahi**; el `enum class` de
  C++ se define en terminos de ellos, asi que los dos lenguajes no pueden
  separarse.  Importa porque los numeros VIAJAN: indexan tablas de contadores,
  cruzan la frontera de C y acaban en el CSV de un informe, y dos listas que hoy
  coinciden y manana no, no fallarian -- contarian en la casilla equivocada.

- **`vesta_memset` deja de pasar por las caches cuando el bloque no cabe en
  ellas.**  Escribir por la cache lee cada linea antes de sobreescribirla --
  para hacer suyo un valor que nadie va a mirar -- y desaloja lo que hubiera
  para hacer sitio a un bloque que no va a caber.  Pasada la cache las dos
  mitades sobran, y un almacen no temporal no hace ninguna.

  Lo que costaba, con el bloque ya residente (sin fallos de pagina en la
  medida): **nosotros caiamos de 47 a 15 GB/s entre 8 y 64 MiB, y glibc se
  quedaba plana en 43-48** porque ya hacia esto.  A 64 MiB eran 2,83x.  En
  Windows no se veia, y por eso no habia salido: msvcrt tampoco los usa, asi
  que ahi empatabamos y nos hundiamos los dos.  El banco de estas primitivas no
  llega a estos tamanos -- por debajo de la ultima cache el coste es del bucle
  y por encima es de como se habla con la memoria, y son dos problemas
  distintos.

  Ahora: **1,00x contra glibc a 32 MiB y 1,03x a 64** (eran 2,89x y 2,83x), y
  **2,88x y 3,14x MEJOR que msvcrt**, que sigue sin hacerlo.

  EL UMBRAL NO ES UNA CONSTANTE: es el ultimo nivel de cache, preguntado con
  `CPUID`.  Sale de medir el caso completo -- rellenar y leer despues una
  fraccion --, no solo el relleno: leyendo poco el no temporal gana desde
  16 MiB, pero leyendo entero pierde hasta 1,49x, y **deja de perder en
  cualquier fraccion entre 24 y 28 MiB, con una cache de 30 MB**.  Una CPU que
  no describa su cache contesta un tamano al que no llega ningun bloque, asi
  que se queda el camino de antes sin ningun caso especial que escribir.

  Dos cosas que estan donde se deciden: la barrera al final del relleno no es
  un adorno -- los almacenes no temporales estan debilmente ordenados y sin ella
  una lectura posterior puede no verlos --, y hace falta **una forma por
  compilador**, porque GCC 10 acepta `__builtin_nontemporal_store`, emite un
  almacen NORMAL y deja el nombre como simbolo SIN RESOLVER (comprobado con
  `nm -u`): habria compilado, escrito por las caches y fallado al enlazar, en
  ese orden.

  PENDIENTE: `vesta_memcpy` tiene el mismo agujero, medido -- 1,06x a 1,58x por
  detras de la libc entre 8 y 64 MiB --, y es el mismo arreglo.

- **Lo que pasa de 16 MiB lo sirve el SISTEMA, en una reserva propia.**  Esa
  cifra no es un gusto: es `kMaxSpanChunks` por `kChunkBytes`, o sea el tramo
  mas grande que la region puede RECICLAR.  Por encima, cada reserva
  comprometia paginas dentro de la reserva grande y cada liberacion las
  descomprometia -- y ese par no se parece en nada al de una reserva propia,
  porque comprometer y descomprometer un SUBRANGO lo recorre y reservar y
  soltar un rango entero no:

  | 16 MiB, sin tocar nada         |  coger | devolver |
  | ------------------------------ | -----: | -------: |
  | commit/decommit en una reserva | 9,4 us |  55,2 us |
  | reserve+commit / release       | 0,7 us |   0,7 us |

  Es plano desde la primera vuelta, asi que no era el descriptor de la reserva
  desgastandose con el uso: era lo que cuestan las dos llamadas.

  Y ARREGLA UN AGOTAMIENTO, que es lo que de verdad estaba mal: un tramo de ese
  tamano no cabia en el banco, asi que sus trozos no se volvian a repartir
  nunca y el cursor de la region solo avanzaba.  Medido, no razonado:
  **16.320 reservas de 16 MiB agotaban una region de 256 GiB con NADA vivo**, y
  a partir de ahi el asignador contestaba nulo -- `operator new` lanzando en un
  programa que no retenia ni un byte --.  Ahora 20.000 pasan sin una sola
  negativa, y el test lo comprueba derivando las vueltas de la region que se
  haya conseguido.

  Lo que se gana, en la fila de 16 MiB (antes -> ahora, por fraccion del bloque
  que el llamante llega a leer): 67,4 -> 2,3 us sin tocar nada; 81,5 -> 22,3
  (1/256); 129,7 -> 45,0 (1/64); 301 -> 160 (1/16); 1,1 ms -> 487 us (1/4);
  3,8 -> 2,5 ms (entero).  **Las seis le ganan ahora al asignador del sistema**,
  de 1,04x a 4,55x; antes se perdian.  Ademas dejan de comerse espacio de
  direcciones de la region que nadie devolvia, y sus paginas llegan YA A CERO,
  que es lo que hace gratis a `host_alloc_zeroed` en estos tamanos.

  POR DEBAJO DE LA RAYA NO SE TOCA NADA, y no por prudencia: nuestro coste es
  plano en la fraccion leida -- se limpia el bloque entero -- y el del sistema
  crece con las paginas tocadas, asi que las dos curvas se cruzan en una
  FRACCION (entre 1/16 y 1/4), no en un tamano.  Ningun umbral por tamano puede
  decidir ahi.

  EN ELF LA GANANCIA ES OTRA, y se dice porque medirlo era el punto: alli
  comprometer dentro de una reserva no cuesta lo que en Windows, asi que la
  fila de 16 MiB sale igual antes y despues (4,3 -> 1,1 us sin tocar nada;
  identica en las otras cinco).  Lo que se gana en ELF es el agotamiento de
  arriba, que era comun a las dos.  **Y queda una perdida abierta ahi**: leido
  1/4 y entero, 16 MiB pierde 0,71x y 0,20x contra glibc, que reutiliza el
  MISMO bloque -- ya residente, cero fallos de pagina -- y lo limpia a ~56
  GB/s.  Ya perdia exactamente igual antes de esto (0,75x y 0,21x): no es una
  regresion, es el mismo problema de fraccion del tramo 2-12 MiB asomando por
  arriba, y se decide igual, sabiendo cuanto va a leer el llamante.

  Reconocerlos al soltarlos es la parte que costo: un bloque asi cae fuera de
  las DOS regiones, y leer su memoria para averiguar si es nuestro es
  justamente lo que no se puede hacer con un puntero que podria ser ajeno.  Se
  contesta con una tabla densa de `kDirectSlots` (512), que se recorre entera
  -- son 17 reservas de 1-16 MiB en una compilacion completa --, y llena no es
  un fallo: la peticion vuelve a la region y `host_direct_refused()` lo cuenta.
  El camino caliente de `host_free` sale **instruccion por instruccion igual**,
  comprobado desensamblando; las tres que se anaden estan todas en la rama que
  antes terminaba el programa.

- **`include/` y `src/` se reparten en seis carpetas** por lo que hace cada
  cosa: `alloc/`, `interpose/`, `report/`, `symbols/` (con `symbols/dwarf/`
  debajo), `os/` y `mem/`.  Estaban las veinticuatro cabeceras y las treinta y
  seis fuentes en un solo sitio, que a partir de una docena deja de ser una
  lista y pasa a ser un monton donde hay que buscar.  Las cabeceras se incluyen
  ahora como `util/<carpeta>/<nombre>.h`.

- **Las cabeceras instalables ya no se enumeran a mano.**  Habia cuatro listas,
  una por subdirectorio, y les faltaban `call_site.h`, `module_symbols.h` y
  `msvcrt_hook.h`.  Su modo de fallar era el caro: no rompia al construir,
  rompia en la maquina de quien INSTALARA la libreria.  Se instala el arbol de
  `include/` entero.

- **`util::PerThreadAllocator`: muchos hilos y NI UN cerrojo hasta 16 KiB.**
  Segunda forma de desbordar, elegida por TIPO al compilar como manda D13.  El
  asignador del proceso acota MEMORIA: puede nombrar `kMaxThreads` duenos y a
  partir de ahi los hilos comparten un cache detras de un cerrojo de giro.
  Este acota LATENCIA: cada hilo tiene el suyo, no hay respaldo compartido al
  que caer, y lo que se paga es memoria -- un cache por hilo vivo, de un fondo
  de `kPerThreadCaches` --.

  Ninguno es mejor: acotan cosas distintas, y por eso existen los dos en vez de
  sustituir uno al otro.

  POR QUE HACIA FALTA.  Porque el cerrojo compartido no se degrada, se
  DERRUMBA, y el codo esta donde nadie lo mira: cuando los hilos que van por el
  camino compartido pasan de los nucleos que hay.  A partir de ahi el que lo
  tiene cogido puede quedarse sin procesador y los demas queman su cuanto
  entero esperando a alguien que no corre.  Medido con 20.000 reservas por hilo
  en una maquina de 24 nucleos, con el MISMO trabajo util en las tres filas:

  | hilos | en compartido | ns de CPU por operacion |
  | ----: | ------------: | ----------------------: |
  |    84 |            21 |                    46,5 |
  |    88 |            25 |                   167,4 |
  |   128 |            65 |                   537,1 |

  Y en `bench_contention` con 128 hilos, lo que se nota no es solo el tiempo
  (101,4 -> 32,4 ms) sino la COLA: el hilo mas lento pasa de ser 42,3 veces mas
  lento que el mas rapido a serlo 3,7.

  Sin competencia no cuesta nada: en `bench_vs_malloc`, que es de un solo hilo,
  la columna nueva va a la par de las otras dos.

- **`util/os_env.h`: el entorno se lee del SISTEMA, no del runtime de C.**
  `util::os_env` y `util::os_env_flag` van al bloque que el nucleo dio al
  proceso -- el PEB en Windows, por `NtQueryInformationProcess`; `environ` en
  POSIX -- en vez de a `getenv`.

  `getenv` no lee el entorno: lee una COPIA que el runtime de C monta mientras
  arranca.  Y este asignador se inicializa en la PRIMERA reserva, que puede
  caer durante la inicializacion de estaticos -- cualquier global cuyo
  constructor pida memoria llega antes que `main` --.  Ahi esa copia puede no
  existir todavia, y entonces `getenv` devuelve nulo y TODOS los mandos salen
  apagados sin fallar y sin decirlo.  Leyendo el bloque del sistema no hay un
  "demasiado pronto", y de paso la libreria deja de necesitar al runtime de C
  para contestar una pregunta sobre el proceso -- la misma razon por la que
  `os_memory.cpp` habla con ntdll y no con kernel32.

  Cada variable se lee **una sola vez**, antes de la primera reserva.
  `..._SITES` decidia dos cosas -- si se apunta y si se parchea `operator new`
  -- y se preguntaba dos veces; ahora se pregunta una y las dos decisiones no
  pueden discrepar.

  Se prueba comparandose con `getenv` sobre el entorno ENTERO
  (`tests/test_os_env.cpp`): llegar antes no vale de nada si la respuesta no es
  la misma.  Ademas de los casos que una muestra se salta -- valor vacio, un
  nombre que es prefijo de otro, truncado visible.

- **Version con TIPO en C++** (`util::vesta_memcopy`, `util::vesta_memfill`) y
  **variantes que si llaman** (`vesta_memcpy_noinline`,
  `vesta_memset_noinline`).

  El tipo trae dos datos que desde C no se pueden saber: `sizeof(T)` hace
  constante el tamano y `alignof(T)` **quita el prologo que alinea el
  destino**.  Ese prologo calcula un desplazamiento en ejecucion, y eso
  convierte un tamano constante en variable, con lo que el bucle deja de
  desenrollarse: desensamblado, un relleno de 256 bytes pasa de 66
  instrucciones con 4 ramas a 19 en linea recta.

  Donde se nota es con tamanos que NO son multiplo del ancho de una escritura,
  ahi el prologo se paga entero.  Con tamano redondo la cascada ya se plegaba
  sola y empatan, que es el resultado honesto.  Y con un tipo que NO declara
  alineacion, la version con tipo no se inventa nada: sale igual que la de C.

  Los tiempos NO se copian aqui: viven en `bench/baseline/`, con la maquina y
  las condiciones de la tanda al lado, y los reproducen `bench_memcpy` y
  `bench_memset`.  Un numero suelto en un registro de cambios no dice en que
  maquina ni con que compilador salio, que es lo que mas lo mueve, y envejece
  sin que nadie se entere.

  Se llaman `memcopy`/`memfill` y no `memcpy`/`memset` a proposito: aquellas
  cuentan BYTES y estas cuentan OBJETOS.  Con el mismo nombre la deduccion de
  plantilla elegiria la version con tipo sin que nadie lo escriba y el tercer
  argumento cambiaria de unidad en silencio -- eso no da un error, da memoria
  pisada.

  Las `noinline` estan para poder ELEGIR, no para sustituir: el camino en linea
  sigue siendo el de siempre, y es el que se quiere en el camino caliente del
  asignador.  Con tamanos grandes, en cambio, la expansion son cientos de
  instrucciones en cada sitio de llamada.

  Los bancos comparan ahora las TRES (C, C++ y libc) y traen tamanos que no son
  multiplo, que es donde estaba la diferencia.

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

### Corregido

- **`bench_operator_new` acusaba a corridas que funcionaban.**  Su bucle de
  calentamiento no pasaba el puntero por un `volatile`, y desde C++14 el
  compilador puede BORRAR un par `new`/`delete` sin usar -- GCC borraba el
  bucle entero a `-O2`.  La comprobacion que venia despues leia entonces el
  estado del asignador sin que se hubiera reservado nada todavia, concluia que
  la medicion estaba apagada y se negaba a correr... en una corrida que a
  continuacion apuntaba diez millones de reservas sin un fallo.  En Debug no
  pasaba, porque ahi el bucle sigue estando.

  Ahora los dos bucles pasan por la misma funcion, que es donde vive el
  `volatile`: una copia sin el es un bucle que el compilador puede borrar, y lo
  borra en silencio.  Una autocomprobacion que acusa a lo que funciona es peor
  que no tenerla -- manda a quien la lea detras de un fallo que no existe.

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

- **Pages with PERMISSIONS, placed where they are needed: `host_alloc_pages`.**
  Underneath, this was always an arena dealer; the only fixed things were the
  permissions its pages are committed with and that they landed wherever.  Both
  are now asked for, and `util::ScratchArena` takes them too -- it is the same
  arena, not a new one.

  WHAT FOR.  Generated code reaches its data with 32-bit displacements, which
  cover +-2 GB; past that the reference cannot even be emitted.  So where a
  block LANDS decides whether the code that will live in it works.  The JIT
  asked the system and ended up **16,405 MiB from a datum eight bytes off its
  anchor**, with a "rel32 out of range" that was not a warning: it returned
  zero, and that zero became a thread's entry point.

  AND ASKING FOR IT NEXT DOOR IS NOT ENOUGH, which was the first thing tried:
  with the anchor inside the big-class region, the walk sees 22 regions and the
  largest free run is **ZERO** -- the +-2 GB window fits entirely inside those
  16 GiB.  That is geometry, not bad luck.  What works is serving from INSIDE:
  the chunks come off the very cursor the big classes use and only the
  permissions differ, so the data path does not notice -- one more client, not a
  new mechanism -- and closeness follows on its own, because that cursor runs
  just behind everything already handed out.  Measured: from 16,405 MiB to
  **5 MiB**.

  IT DOES NOT ASSUME IT WORKED: it checks the real distance before returning,
  and if the cursor has run past the window it decommits and falls back to
  `os_alloc_near`.  `host_exec_far()` counts those, because failing here is
  silent by nature -- the block still comes back and what breaks is a
  displacement, much later and somewhere else.

- **`os_alloc_near`: reserving CLOSE to an address that already exists.**  It
  walks the window region by region and takes the NEAREST free run.  This used
  to live inside the code generator, probing `base +- 2^k` -- thirty scattered
  points, all taken as soon as one large reservation lies across the middle --
  and when that became a walk it stopped at the first hole, which is the EDGE of
  the window: 1,920 MiB, with no margin for data that is not the exact anchor.
  Taking the nearest instead: **1.8 MiB**.

  The parameter is called `anchor` and not `near` because `windows.h` still
  defines `near` as an empty macro, left over from sixteen-bit pointers: a
  parameter with that name vanishes and what fails is the line that uses it.

- **`malloc` is this allocator INSIDE the C runtime too.**  Link-time renaming
  reaches every call in the link and stops there: what the C library allocates
  inside itself and hands back — `strdup`, `getline`, `_wgetdcwd` — is not a
  pending reference, so no linker can touch it.  That memory showed as zero,
  and zero reads like "there is none".

  Closed by one mechanism per platform, **both on by default**:
  `VESTA_ALLOC_DEFINE_MALLOC` on ELF (define the symbol, the way jemalloc does:
  the library's own calls go out through the PLT) and `VESTA_ALLOC_HOOK_MSVCRT`
  on Windows (a jump at the entry of `msvcrt!malloc`, because a DLL has no PLT
  and an internal call never leaves it).  ONLY msvcrt: not kernel32, not ntdll.

  Three things cost a round trip each, and are written where they are decided:
  the two ways **cannot coexist** with `--wrap` for the same symbol —
  `__real_malloc` would resolve against our own definition and the first
  allocation would call itself; on Windows the jumps have to go in from a TLS
  callback rather than a constructor, because the four blocks of difference
  include the `atexit` table, which grows with `realloc`; and on ELF a
  bootstrap arena is needed, because being `malloc` means being asked before
  the allocator has decided whether it is active.

- **The aligned entries, on both platforms.**  `posix_memalign`,
  `aligned_alloc`, `memalign` and the whole Windows `_aligned_*` family.  On
  POSIX such a block is released with plain `free`, so it has to say what it is
  by itself: it is served as a span, whose header `free` already finds by
  masking — no marker, no side table, and not one instruction added to the free
  path.

  `_aligned_realloc` and its relatives are covered even though nobody calls
  them: they are the ones handed an ALREADY allocated pointer, and leaving them
  alone meant one of our blocks reaching the runtime's heap.  That does not
  fail at the call; it fails much later and somewhere else.

- **Whose address is this** (`util/symbols/module_symbols.h`).  Everything that
  resolves names read the program's OWN image and none of them checked that the
  address was the program's: one inside `libc` found nothing in our table and
  came back wearing the last symbol we happened to have, `_fini`, with the same
  confidence as a true name.  A report is read as fact, and a wrong name is
  worse than a missing one.

  For a foreign module the symbols are read from ITS file — `slurp` and
  `build_table` were never specific to "self" — with what the loader offers as
  the fallback for a stripped one.

- **The text dump resolves names too**, and allocations from other modules get
  a section of their own.  The same process used to write `parse_tokens` into
  the CSV and a bare offset to the terminal; and foreign sites, with nine
  allocations against three hundred and thirty, never surfaced in a list sorted
  by count — which was exactly what had just been gained.

- **The report page filters by SCOPE and by LANGUAGE**, and the two axes
  combine: everything / only my calls folded onto my function / only my calls
  strictly / only external, crossed with C, C++ or undetermined.
  "Undetermined" is a view of its own and not a bin: with no debug information
  there is no file, and without a file a bare name does not say which language
  wrote it — the page SAYS so, and says how to fix it.  Whatever a filter
  leaves out is always counted.

### Changed

- **The caller can say HOW MUCH of what it asks for it will touch, and that
  closes the two rows that were losing.**  A big `calloc` has two opposite right
  answers -- keep the block and clear it, whose cost is flat in what gets read
  because the whole thing is written, or ask the system for a fresh one, which
  arrives already zero and costs a page fault per page actually touched.  They
  cross at a FRACTION of the block, between 1/16 and 1/4, and NOT at a size: the
  same 8 MiB request wins one way read sparsely and the other read whole.  No
  threshold on size can choose; the only one who knows is the caller.

  `AllocFill` is that axis and `AllocScope` declares it.  Measured against
  msvcrt across seven sizes by three fractions, **all 21 rows win**, including
  the two that were open: `calloc` of 1 MiB read 1/64 goes to 1.40x in our
  favour and the 8 MiB one to 1.36x, where it was losing 3.3x.  Below
  `kSparseDirectMin` (256 KiB, measured) it does not apply: there the system
  call costs more than the clearing it saves.

  IT IS AN ASSERTION, not a fact, exactly like the purpose tag: declaring sparse
  on a block that is then read whole costs speed and never correctness, and
  `host_fill_allocs` says how many went down each branch so the claim can be
  checked.

  AND IT SITS APART from `AllocTag`, with its own count, for a measured reason:
  the two purpose axes are packed into four bits so the byte can index the
  counter table, and a third one there would take it from 16 slots to 64.
  Putting the counter in `HostAllocStats` -- which lives in the per-thread cache
  AHEAD of the hot batch -- grew the structure from 1216 to 1280 bytes and
  pushed the batch 32 along: only +0.01 ns, under the benchmark's own 2% floor,
  but 64 KiB of static memory and, worse, it tied the hot path to the counters.
  With the counter in a global, `host_alloc` and `host_free` come out
  **instruction for instruction as they were**.

  What was feared and is NOT there, profiled with hardware counters on twelve
  threads, three interleaved runs of each layout: `SPLIT_LOADS` and
  `SPLIT_STORES` at exactly zero, every `XSNP_*` event at zero -- no line
  travels between cores, so no false sharing -- and `L3 Bound` at 0.0-0.1%.  A
  `static_assert` on the size stops the structure growing again without anyone
  looking; it was checked by reproducing the failure.

- **The three axes can be NAMED from C**, which they could not before:
  `vesta_host_push_tag` took bare integers and the meaning lived in a doc
  comment.  The values now live in `util/alloc/alloc_tag_c.h` and **only**
  there; the C++ `enum class` is defined in terms of them, so the two languages
  cannot drift apart.  It matters because the numbers TRAVEL: they index counter
  tables, cross the C boundary and end up in a report's CSV, and two lists that
  agree today and not tomorrow would not fail -- they would count in the wrong
  bucket.

- **`vesta_memset` stops going through the caches when the block does not fit
  in them.**  Writing through the cache reads every line before overwriting it
  -- to take ownership of a value nobody will ever look at -- and evicts
  whatever was there to make room for a block that will not fit anyway.  Past
  the cache both halves are waste, and a non-temporal store does neither.

  What it cost, with the block already resident (no page faults in the
  measurement): **we fell from 47 to 15 GB/s between 8 and 64 MiB, and glibc
  stayed flat at 43-48** because it already did this.  At 64 MiB that was 2.83x.
  It was invisible on Windows, which is why it had not come up: msvcrt does not
  do it either, so there we tied and both sank.  These primitives' own benchmark
  does not reach these sizes -- below the last level of cache the cost is the
  loop and above it is how it talks to memory, and those are two problems.

  Now: **1.00x against glibc at 32 MiB and 1.03x at 64** (from 2.89x and 2.83x),
  and **2.88x and 3.14x BETTER than msvcrt**, which still does not do it.

  THE THRESHOLD IS NOT A CONSTANT: it is the last level of cache, asked for with
  `CPUID`.  It comes from measuring the whole case -- fill and then read a
  fraction back -- and not the fill alone: reading little, streaming wins from
  16 MiB, but reading all of it loses by up to 1.49x, and it **stops losing on
  any fraction between 24 and 28 MiB, with a 30 MB cache**.  A CPU that does not
  describe its cache answers a size no block reaches, so the older path stays
  with no special case to write.

  Two things live where they are decided: the fence at the end of the fill is
  not tidiness -- non-temporal stores are weakly ordered and without it a later
  read may not see them -- and it takes **one form per compiler**, because GCC
  10 accepts `__builtin_nontemporal_store`, emits an ORDINARY store for it and
  leaves the name as an UNDEFINED SYMBOL (checked with `nm -u`): it would have
  compiled, written through the caches, and failed to link, in that order.

  STILL OPEN: `vesta_memcpy` has the same hole, measured -- 1.06x to 1.58x
  behind the C library between 8 and 64 MiB -- and it is the same fix.

- **Anything over 16 MiB is served by the SYSTEM, on a reservation of its own.**
  That figure is not a taste: it is `kMaxSpanChunks` times `kChunkBytes`, which
  is to say the largest span the region can RECYCLE.  Above it, every
  allocation committed pages inside the big reservation and every free
  decommitted them again -- and that pair is nothing like the one a private
  reservation costs, because committing and decommitting a SUB-RANGE walks it
  while reserving and releasing a whole range does not:

  | 16 MiB, nothing touched          |   take | give back |
  | -------------------------------- | -----: | --------: |
  | commit/decommit inside a reserve | 9.4 us |   55.2 us |
  | reserve+commit / release         | 0.7 us |    0.7 us |

  It is flat from the very first round, so it was not the reservation's
  descriptor wearing out with use: it was what the two calls cost.

  AND IT FIXES AN EXHAUSTION, which is what was really wrong: a span that size
  did not fit in the pool, so its chunks were never handed out again and the
  region's cursor only moved forward.  Measured, not reasoned: **16,320
  allocations of 16 MiB exhausted a 256 GiB region with NOTHING live**, and
  from there the allocator answered null -- `operator new` throwing in a
  program that was not holding a single byte.  Now 20,000 pass without one
  refusal, and the test checks it with the number of rounds derived from
  whatever region was obtained.

  What that buys, on the 16 MiB row (before -> now, by the fraction of the
  block the caller actually reads): 67.4 -> 2.3 us touching nothing; 81.5 ->
  22.3 (1/256); 129.7 -> 45.0 (1/64); 301 -> 160 (1/16); 1.1 ms -> 487 us
  (1/4); 3.8 -> 2.5 ms (all of it).  **All six now beat the system allocator**,
  by 1.04x to 4.55x; they used to lose.  They also stop eating region address
  space that nothing ever handed back, and their pages arrive ALREADY ZERO,
  which is what makes `host_alloc_zeroed` free at these sizes.

  NOTHING BELOW THE LINE CHANGES, and not out of caution: our cost is flat in
  the fraction read -- the whole block is cleared -- and the system's grows with
  the pages touched, so the two curves cross at a FRACTION (around 1/16 to 1/4),
  not at a size.  No threshold on size can decide there.

  ON ELF THE GAIN IS A DIFFERENT ONE, and it is said because measuring it was
  the point: there, committing inside a reservation does not cost what it costs
  on Windows, so the 16 MiB row comes out the same before and after (4.3 -> 1.1
  us touching nothing; identical on the other five).  What ELF gains is the
  exhaustion above, which both had.  **And a loss stays open there**: read 1/4
  and read whole, 16 MiB comes out at 0.71x and 0.20x against glibc, which
  reuses the SAME block -- already resident, no page faults -- and clears it at
  ~56 GB/s.  It lost by exactly the same margin before this (0.75x and 0.21x):
  not a regression, but the 2-12 MiB fraction problem showing up from above,
  and it is decided the same way, by knowing how much the caller will read.

  Recognising them on free is the part that took work: such a block falls
  outside BOTH regions, and reading its memory to find out whether it is ours is
  exactly what must not be done with a pointer that might be foreign.  The
  answer comes from a dense table of `kDirectSlots` (512) scanned end to end --
  these are 17 allocations of 1-16 MiB in a whole build -- and full is not a
  failure: the request goes back to the region and `host_direct_refused()`
  counts it.  The hot path of `host_free` comes out **instruction for
  instruction as it was**, verified by disassembly; the three that are added are
  all on the branch that used to end the program.

- **`include/` and `src/` are split into six folders** by what each one does:
  `alloc/`, `interpose/`, `report/`, `symbols/` (with `symbols/dwarf/` under
  it), `os/` and `mem/`.  Twenty-four headers and thirty-six sources sat in one
  place, which past a dozen stops being a list and becomes a pile to search.
  Headers are now included as `util/<folder>/<name>.h`.

- **The installable headers are no longer listed by hand.**  There were four
  lists, one per subdirectory, and they were missing `call_site.h`,
  `module_symbols.h` and `msvcrt_hook.h`.  The expensive failure mode: it did
  not break the build, it broke on the machine of whoever INSTALLED the
  library.  The whole `include/` tree is installed instead.

- **`util::PerThreadAllocator`: many threads and NOT ONE lock up to 16 KiB.**
  A second overflow policy, picked by TYPE at compile time as D13 requires.
  The process-wide allocator bounds MEMORY: it can name `kMaxThreads` owners,
  and past that threads share one cache behind a spin lock.  This one bounds
  LATENCY: every thread gets a cache of its own, there is no shared fallback to
  land in, and what it costs is memory -- one cache per live thread, out of a
  pool of `kPerThreadCaches`.

  Neither is better; they bound different things, which is why both exist
  rather than one replacing the other.

  WHY IT WAS NEEDED.  Because the shared lock does not degrade, it COLLAPSES,
  and the knee is where nobody looks: when the threads on the shared path
  outnumber the cores.  Past that the holder can lose its processor and every
  other spinner burns a whole quantum waiting on a thread that is not running.
  Measured with 20,000 allocations per thread on a 24-core machine, with the
  SAME useful work in all three rows:

  | threads | on shared | CPU ns per operation |
  | ------: | --------: | -------------------: |
  |      84 |        21 |                 46.5 |
  |      88 |        25 |                167.4 |
  |     128 |        65 |                537.1 |

  And in `bench_contention` at 128 threads what shows is not only the time
  (101.4 -> 32.4 ms) but the TAIL: the slowest thread goes from being 42.3x
  slower than the fastest to 3.7x.

  It costs nothing where there is no contention: in `bench_vs_malloc`, which is
  single-threaded, the new column is on par with the other two.

- **`util/os_env.h`: the environment is read from the SYSTEM, not from the C
  runtime.**  `util::os_env` and `util::os_env_flag` go to the block the kernel
  handed the process -- the PEB on Windows, through
  `NtQueryInformationProcess`; `environ` on POSIX -- instead of to `getenv`.

  `getenv` does not read the environment: it reads a COPY the C runtime builds
  while it starts up.  And this allocator initialises on the FIRST allocation,
  which can happen during static initialisation -- any global whose constructor
  asks for memory gets there before `main`.  That copy may not exist yet at
  that point, and then `getenv` returns null and every switch reads as off,
  without failing and without saying so.  Reading the system's block means
  there is no "too early", and it also drops the C runtime as a dependency for
  answering a question about the process -- the same reason `os_memory.cpp`
  talks to ntdll rather than kernel32.

  Each variable is read **once**, before the first allocation.  `..._SITES`
  decided two things -- whether to record and whether to patch `operator new`
  -- and was asked twice; it is now asked once and the two decisions cannot
  disagree.

  Tested by comparing against `getenv` over the WHOLE environment
  (`tests/test_os_env.cpp`): arriving earlier is worth nothing if the answer is
  not the same one.  Plus the cases a sample would skip -- an empty value, a
  name that is a prefix of another, visible truncation.

- **Typed version in C++** (`util::vesta_memcopy`, `util::vesta_memfill`) and
  **variants that do call** (`vesta_memcpy_noinline`, `vesta_memset_noinline`).

  The type carries two facts C cannot know: `sizeof(T)` makes the length
  constant and `alignof(T)` **removes the prologue that aligns the
  destination**.  That prologue computes an offset at run time, which turns a
  constant length into a variable one and stops the loop from being unrolled:
  disassembled, a 256-byte fill goes from 66 instructions with 4 branches to 19,
  straight line.

  It shows up at sizes that are NOT a multiple of the store width, where the
  prologue is paid in full.  At round sizes the cascade already folded on its
  own and they tie, which is the honest result.  And with a type that declares
  no alignment the typed version invents nothing: it comes out level with the C
  one.

  The timings are NOT copied here: they live in `bench/baseline/`, with the
  machine and the run conditions beside them, and `bench_memcpy` /
  `bench_memset` reproduce them.  A loose number in a changelog does not say
  which machine or which compiler produced it -- the two things that move it
  most -- and it goes stale without anybody noticing.

  They are called `memcopy`/`memfill` and not `memcpy`/`memset` on purpose: the
  former count BYTES and these count OBJECTS.  With the same name, template
  deduction would pick the typed one without anybody writing it and the third
  argument would silently change unit -- that does not give an error, it gives
  overwritten memory.

  The `noinline` ones exist so you can CHOOSE, not to replace anything: the
  inline path is still the default and it is what the allocator's hot path
  wants.  At large sizes, though, the expansion is hundreds of instructions at
  every call site.

  The benchmarks now compare all THREE (C, C++ and libc) on every row and over
  the same addresses, and include sizes that are not multiples, which is where
  the difference was.

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

### Fixed

- **`bench_operator_new` accused runs that were working.**  Its warm-up loop did
  not park the pointer in a `volatile`, and since C++14 the compiler may DELETE
  an unused `new`/`delete` pair -- GCC deleted the whole loop at `-O2`.  The
  check that followed then read the allocator's state with nothing allocated
  yet, concluded measurement was off and refused to run... on a run that went on
  to record ten million allocations without a hitch.  Debug never showed it,
  because there the loop is still present.

  Both loops now go through the same function, which is where the `volatile`
  lives: a copy without it is a loop the compiler is free to delete, and it
  deletes it silently.  A self-check that accuses working code is worse than no
  self-check -- it sends whoever reads it after a bug that is not there.

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
