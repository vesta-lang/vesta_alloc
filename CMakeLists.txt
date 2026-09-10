# =============================================================================
#  vesta_alloc -- asignador de memoria del anfitrion, reutilizable
# =============================================================================
#
# Se puede sacar de aqui y llevarselo: no depende de nada del compilador ni de
# ninguna libreria de terceros.  Solo del sistema operativo.
#
# Se construye de dos maneras:
#
#   - COMO PARTE de otro proyecto (`add_subdirectory`): se usa el objetivo de
#     OBJETOS `vesta_alloc`, y sus objetos entran dentro de quien lo consuma.
#   - SOLO, como cualquier proyecto: genera `libvesta_alloc.a` (y opcionalmente
#     la dinamica), sus cabeceras instalables y sus pruebas.
#
#       cmake -S . -B build && cmake --build build && ctest --test-dir build
#
# POR QUE EL OBJETIVO PRINCIPAL ES DE OBJETOS Y NO UN ARCHIVO.  Porque esta
# libreria reemplaza `operator new` y `operator delete` globales, y de un
# ARCHIVO (`.a`, `.lib`) el enlazador solo saca los objetos que alguien
# referencia POR SU NOMBRE.  Nadie llama a `operator new` por su nombre -- lo
# genera el compilador --, asi que el objeto que lo define no se extrae y el
# programa se queda con el asignador del sistema.  Y eso NO da ningun error: da
# un binario que funciona y va mas lento, que es el peor modo de fallo posible.
#
# Quien enlace la version ESTATICA tiene ese problema y tiene que resolverlo:
# ver el README (`--whole-archive`, `/WHOLEARCHIVE:` o `-force_load`).
cmake_minimum_required(VERSION 3.12)

# Detectar si se esta construyendo suelto o dentro de otro proyecto.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # C tambien: los ejemplos en C se compilan COMO C, que es lo que comprueba
    # que la cabecera de la capa en C lo sea de verdad.
    project(vesta_alloc VERSION 1.0.0 LANGUAGES CXX C)
    set(VESTA_ALLOC_STANDALONE ON)

    # -------------------------------------------------------------------------
    #  Tres modos, los mismos que el proyecto de donde sale esto.
    # -------------------------------------------------------------------------
    #
    #  Release  lo que se envia.
    #  Profile  Release CON simbolos.  No vale perfilar sobre Debug: sin meter
    #           en linea, cualquier funcion pequena llamada a menudo copa la
    #           lista, y aqui casi todo son funciones pequenas llamadas a
    #           menudo.  Y `RelWithDebInfo` es -O1, que es OTRO programa.
    #  Debug    para depurar el asignador, no para medirlo.
    #
    #  Solo se definen cuando esto se construye SUELTO.  Metido en otro
    #  proyecto se heredan sus opciones a proposito: compilar una parte del
    #  binario con banderas distintas es como se llega a fallos que solo
    #  aparecen en una configuracion.
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        # Sin tipo de construccion, CMake no pasa NINGUNA opcion de
        # optimizacion: saldria un asignador a `-O0`, varias veces mas lento
        # que el del sistema al que pretende sustituir.  Y no fallaria nada,
        # solo iria peor -- que es justo el modo de fallo que este componente
        # esta escrito para evitar en otros sitios.
        set(CMAKE_BUILD_TYPE Release CACHE STRING
                "Tipo de construccion: Release, Profile o Debug" FORCE)
        message(STATUS "[vesta_alloc] sin CMAKE_BUILD_TYPE: se usa Release")
    endif()
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
            Debug Release Profile)

    # `generic` POR DEFECTO, y a proposito: esto es una biblioteca que se
    # distribuye, y con `native` el `.a` saldria ordenado para la maquina que lo
    # construyo.  No dejaria de arrancar en otra -- eso lo fija `-march`, y
    # `-mtune` solo cambia el ORDEN de las instrucciones --, pero iria peor sin
    # que nadie supiera por que, y encima el resultado dependeria de en que
    # ordenador se construyo.
    #
    # Quien lo construya para si mismo puede pedir `native`:
    #     cmake -S . -B build -DVESTA_ALLOC_TUNE=native
    # OJO al escribirlo: el texto de ayuda tiene que ser UNA sola cadena.  En
    # dos, CMake las toma como valores mas y la variable acaba conteniendo el
    # texto entero -- que se cuela tal cual en la linea de compilacion.
    set(VESTA_ALLOC_TUNE "generic" CACHE STRING
            "Microarquitectura para -mtune: generic para distribuir, native para la maquina de uno")

    # `-march=x86-64` es la LINEA BASE: no se emite ni una instruccion fuera
    # del x86-64 original.  Y `-ffast-math` NO esta, igual que en el proyecto
    # padre: aqui no hay aritmetica de coma flotante que ganar, y relajar la
    # semantica de IEEE a cambio de nada solo compra formas raras de fallar.
    set(_va_opt
            -O3
            -march=x86-64
            -mtune=${VESTA_ALLOC_TUNE}
            -fstrict-aliasing
            -fno-plt
            -ffunction-sections
            -fdata-sections)
    string(REPLACE ";" " " _va_opt_str "${_va_opt}")

    set(CMAKE_CXX_FLAGS_RELEASE "${_va_opt_str} -DNDEBUG"
            CACHE STRING "Flags C++ de Release" FORCE)
    set(CMAKE_CXX_FLAGS_PROFILE "${_va_opt_str} -DNDEBUG -g1 -fno-omit-frame-pointer"
            CACHE STRING "Flags C++ para perfilar (Release + simbolos)" FORCE)
    # Debug tambien lleva `-march=x86-64` explicito.  Sin el se usa lo que
    # llevara compilado el propio GCC, que en algunas distribuciones NO es la
    # linea base: un binario de depuracion que no arranca en otra maquina es
    # una forma tonta de perder una tarde.
    set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -march=x86-64"
            CACHE STRING "Flags C++ de Debug" FORCE)
    # LAS MISMAS PARA C, y no es cosmetica.  Estaban solo las de C++, asi que
    # los ejemplos en C se compilaban con lo que CMake pone por defecto: sin
    # `-O3` y, en Profile, SIN `-g`.  Se noto porque el ejemplo del informe en
    # C sacaba los nombres de sus funciones pero sin fichero ni linea -- caia
    # al segundo intento del resolutor, la tabla de simbolos, porque su unidad
    # no traia informacion de depuracion mientras el resto del binario si.
    # Media libreria compilada con otras banderas es ademas como se llega a
    # fallos que solo aparecen en una configuracion.
    set(CMAKE_C_FLAGS_RELEASE "${_va_opt_str} -DNDEBUG"
            CACHE STRING "Flags C de Release" FORCE)
    set(CMAKE_C_FLAGS_PROFILE "${_va_opt_str} -DNDEBUG -g1 -fno-omit-frame-pointer"
            CACHE STRING "Flags C para perfilar (Release + simbolos)" FORCE)
    set(CMAKE_C_FLAGS_DEBUG "-O0 -g -march=x86-64"
            CACHE STRING "Flags C de Debug" FORCE)

    set(CMAKE_EXE_LINKER_FLAGS_PROFILE "${CMAKE_EXE_LINKER_FLAGS_RELEASE}"
            CACHE STRING "Flags de enlazado para perfilar" FORCE)
    set(CMAKE_SHARED_LINKER_FLAGS_PROFILE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE}"
            CACHE STRING "Flags de enlazado dinamico para perfilar" FORCE)
    if(CMAKE_CONFIGURATION_TYPES)
        list(APPEND CMAKE_CONFIGURATION_TYPES Profile)
        list(REMOVE_DUPLICATES CMAKE_CONFIGURATION_TYPES)
    endif()

    message(STATUS "[vesta_alloc] modo ${CMAKE_BUILD_TYPE}, -mtune=${VESTA_ALLOC_TUNE}")
else()
    set(VESTA_ALLOC_STANDALONE OFF)
endif()

# Avisar si se construye sin optimizar, pero SOLO cuando esto se construye
# suelto.  Ahi el tipo de construccion es una eleccion sobre ESTA biblioteca y
# conviene decir que las cifras del README no se parecen en nada a las de una
# construccion sin optimizar.
#
# Metido en otro proyecto NO se avisa, y no por cortesia: compilar en Debug es
# el flujo normal de desarrollo de quien nos incluye, asi que el aviso saldria
# en cada configuracion sin que haya nada que decidir.  Un aviso que sale
# siempre deja de leerse, y arrastra con el a los que si importan.
if(VESTA_ALLOC_STANDALONE AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(WARNING "[vesta_alloc] construccion Debug: el asignador ira MUCHO "
            "mas lento que el del sistema.  Correcto para depurarlo; no para "
            "medirlo ni para enviarlo.")
endif()

option(VESTA_ALLOC_BUILD_STATIC "Generar libvesta_alloc.a"
        ${VESTA_ALLOC_STANDALONE})
option(VESTA_ALLOC_BUILD_SHARED "Generar la version dinamica (leer el README)"
        OFF)
option(VESTA_ALLOC_BUILD_TESTS "Compilar las pruebas"
        ${VESTA_ALLOC_STANDALONE})
option(VESTA_ALLOC_BUILD_EXAMPLES "Compilar los ejemplos"
        ${VESTA_ALLOC_STANDALONE})
option(VESTA_ALLOC_BUILD_BENCH "Compilar los bancos de medida"
        ${VESTA_ALLOC_STANDALONE})

# -----------------------------------------------------------------------------
#  `malloc` TAMBIEN es este asignador
# -----------------------------------------------------------------------------
#
# `operator new` esta reemplazado globalmente, asi que todo `new` del programa
# -- el nuestro y el de las librerias en C++ que van dentro -- ya pasa por
# aqui.  Las entradas en C no tenian nada de eso, y el hueco no era solo de C:
# un `.cpp` que llama a `malloc` directamente se iba al sistema igual.
#
# Se cierra renombrando las llamadas AL ENLAZAR (`-Wl,--wrap=`), que es la via
# que fija el plan (D12): no se toca, ni se recompila, ni se entera el codigo
# que llama -- que es lo unico que sirve para las librerias vendorizadas, donde
# editar el sitio de llamada no es una opcion.
#
# Se puede apagar, y no por cortesia: sin poder apagarlo no hay con que
# comparar, que es la unica forma de saber si esto mejora algo.  Apagarlo aqui,
# al CONFIGURAR, y no con una variable de entorno en ejecucion: el renombrado es
# de tiempo de enlace y no se deshace despues -- eso es lo que hundio al
# `VESTA_NO_HOST_SLAB` que existia antes, que dejaba las llamadas renombradas y
# sin nadie que las sirviera.  Para comparar en ejecucion esta
# `support/system_alloc.h`, que alcanza el del sistema dentro del mismo proceso.
option(VESTA_ALLOC_INTERPOSE_MALLOC
        "Que malloc/calloc/realloc/free sean este asignador (via -Wl,--wrap)"
        ON)

# -----------------------------------------------------------------------------
#  LA OTRA VIA: SER el simbolo, en vez de que lo renombren
# -----------------------------------------------------------------------------
#
# Renombrar al enlazar llega a todas las llamadas del enlace y ahi se acaba.  Lo
# que la libreria de C reserva DENTRO de si misma nunca es una referencia
# pendiente, asi que ningun enlazador puede renombrarla -- y eso es `strdup`,
# `getline`, `asprintf` y compania, que devuelven un bloque para que lo suelte
# quien llama.  Hoy esa memoria no sale en el informe, que es lo mismo que decir
# que no existe.
#
# Definiendo el simbolo si se llega: en ELF la definicion del ejecutable gana a
# la de `libc.so` y las llamadas internas de la libreria pasan por la PLT.  Es
# lo que hacen jemalloc, tcmalloc y mimalloc.
#
# NO CONVIVE con el renombrado, y no es una cuestion de gusto: con `--wrap` y
# una definicion propia a la vez, el enlazador resuelve `__real_malloc` con la
# UNICA definicion que hay -- la nuestra --, y la primera reserva se llama a si
# misma hasta agotar la pila.  Comprobado con una sonda de tres lineas antes de
# escribir nada.  Por eso, mas abajo, esos simbolos salen de la lista de
# `--wrap` cuando esto esta encendido.
#
# Y son los CUATRO -- `malloc`, `free`, `realloc` y `calloc` --, no solo el par.
# En cuanto `malloc` es nuestro, la libc tiene bloques NUESTROS, y si `realloc`
# sigue siendo suyo los hace crecer leyendo una cabecera que no escribio nadie.
# No es un supuesto: `getcwd(NULL,0)` reserva con `malloc` y ENCOGE con
# `realloc`, y con eso murio el compilador en Linux la primera vez que se
# valido ahi.  La traza esta en la cabecera de `malloc_define.cpp`.
#
# ENCENDIDA EN ELF, y la razon de fondo es la PARIDAD.  En Windows el mismo
# agujero lo cierra el gancho del runtime (`VESTA_ALLOC_HOOK_MSVCRT`, encendido
# ahi), asi que dejar esta apagada significaba que el MISMO programa daba un
# informe con una cobertura en un sistema y otra en el otro -- y dos medidas
# que no cubren lo mismo no se pueden comparar, que es para lo que sirve medir.
#
# Se enciende porque esta comprobada, no porque toque: 15 de 15 pruebas con GCC
# 15.2 y con Clang 19.1 con ella puesta, y con un test que demuestra lo unico
# que compra -- que lo que `strdup` reserva DENTRO de la libc sale como bloque
# nuestro.  Se puede apagar para comparar, que es la unica forma de saber si
# suma; y ahi el renombrado sigue entero.
#
# El valor por defecto es POR PLATAFORMA: fuera de ELF no hay simbolo al que
# ganarle -- un DLL se alcanza por la tabla de importacion --, y pedirlo alli
# es un error que se DICE mas abajo en vez de construir algo que no haria nada.
if(WIN32 OR APPLE OR MSVC)
    set(_va_define_malloc_default OFF)
else()
    set(_va_define_malloc_default ON)
endif()
option(VESTA_ALLOC_DEFINE_MALLOC
        "Que malloc/free/realloc/calloc sean simbolos NUESTROS (solo ELF; \
sustituye a --wrap para esos cuatro y alcanza tambien lo que la libc reserva \
por dentro)"
        ${_va_define_malloc_default})

# -----------------------------------------------------------------------------
#  Y LO MISMO EN WINDOWS, que ahi no hay simbolo al que ganarle
# -----------------------------------------------------------------------------
#
# El agujero es el mismo -- lo que el runtime de C reserva DENTRO de si mismo y
# devuelve: `_strdup`, `_wcsdup`, `_fullpath`, `_getcwd` --, pero la solucion de
# ELF no vale: una llamada de una funcion de la DLL a otra funcion de la DLL no
# sale por ninguna tabla, asi que no hay definicion que gane ni importacion que
# redirigir.
#
# Lo unico que queda es el codigo, y lo que se pone ahi es UNA instruccion: un
# salto en la entrada de `malloc` que apunta a la nuestra.  Es exactamente lo
# que esta libreria ya escribe sobre `operator new` para apuntar los sitios de
# reserva, con la misma maquinaria (`src/code_patch.h`).
#
# SOLO msvcrt.  Ni kernel32, ni ntdll, ni ningun otro componente: el runtime de
# C es quien reserva por cuenta del programa, y es el unico que se toca.
#
# ENCENDIDA.  Reescribir codigo en ejecucion es lo mas invasivo que hace esta
# libreria, asi que no se enciende por gusto -- se enciende porque el informe
# sin esto MIENTE POR OMISION: la memoria que el runtime reserva por dentro no
# sale, y lo que no sale se lee como que no existe.  Que es lo mismo que decia
# la cabecera de `malloc_interpose.cpp` de este agujero cuando no habia forma
# de cerrarlo.
#
# Y se enciende porque se ha comprobado, no porque parezca que va: con el
# compilador entero, el `.velb` que sale es IDENTICO byte a byte al de sin
# gancho, el programa da el mismo resultado, y aparecen doce reservas que antes
# no se veian.  Se puede apagar para comparar, que es la unica forma de saber
# si esto suma.
#
# El valor por defecto es POR PLATAFORMA y no un `ON` a secas: fuera de Windows
# esto no tiene nada que parchear, y pedirlo alli es un error que se DICE (ver
# mas abajo).  Con un `ON` fijo, configurar en Linux fallaria sin que nadie
# hubiera pedido nada.
option(VESTA_ALLOC_HOOK_MSVCRT
        "Que lo que el runtime de C reserva por dentro tambien sea este \
asignador (solo Windows; escribe un salto en la entrada de msvcrt!malloc)"
        ${WIN32})

# -----------------------------------------------------------------------------
#  LTO BREAKS THIS, AND WE STOP HERE
#
#  `call_site.cpp` records where each allocation comes from by writing a jump
#  over the ENTRY of `operator new`.  That assumes every allocation goes through
#  that entry -- always true, because `operator new` is replaceable and lives in
#  another translation unit -- and link-time optimisation breaks it: it may
#  inline `operator new` into its callers, and those allocations no longer pass
#  through the patched entry.
#
#  The expensive part is HOW it breaks: it does not fail, it does not warn, those
#  allocations simply do not appear in the report.  A list of who allocates that
#  is missing allocations is not incomplete, it is wrong -- and there is no way
#  to tell by looking at it.
#
#  Checked HERE and not with an `#error` because the preprocessor cannot: neither
#  GCC nor Clang define any macro for `-flto`.  CMake does know, so CMake is the
#  one that has to say it.
# -----------------------------------------------------------------------------
if(CMAKE_INTERPROCEDURAL_OPTIMIZATION
        OR CMAKE_CXX_FLAGS MATCHES "-flto"
        OR CMAKE_CXX_FLAGS_RELEASE MATCHES "-flto"
        OR CMAKE_EXE_LINKER_FLAGS MATCHES "-flto")
    message(FATAL_ERROR
            "vesta_alloc: this project does not use LTO, and "
            "`src/interpose/call_site.cpp` counts on that.\n"
            "The who-allocates report patches the entry of `operator new`; with "
            "LTO the linker may inline it into its callers and those "
            "allocations stop being recorded -- without failing and without "
            "warning, which is the worst thing that can happen to a list.\n"
            "If LTO is genuinely needed, first work out how an inlined "
            "allocation gets attributed; until then this stops the build on "
            "purpose.")
endif()

set(VESTA_ALLOC_SOURCES
        src/report/alloc_sites.cpp
        # The same report as DATA, for a tool to walk instead of a printout to
        # read.  The naming comes in through a hook while symbol resolution
        # still lives in the host; see the header.
        src/report/alloc_csv.cpp
        # The same export with the signatures C understands.  A library carried
        # inside a project and written in C already allocates through us; being
        # counted and being able to ASK for the report are different things.
        src/report/alloc_csv_c.cpp
        # The `operator new` family, plus the patch that makes them say where
        # they came from.  The return address is taken by a `mov` from `[rsp]`,
        # not by a compiler builtin.  See the file header; their bodies stay in
        # host_allocator.cpp.
        src/interpose/call_site.cpp
        src/alloc/host_allocator.cpp
        src/alloc/host_allocator_c.cpp
        # The environment, read from the block the SYSTEM gave the process and
        # not from the C runtime's copy of it -- which does not exist yet when
        # the allocator initialises during static initialisation.
        src/os/os_env.cpp
        src/os/os_memory.cpp
        # Mirarse a UNO MISMO: encontrar las secciones del propio binario, leer
        # su tabla de simbolos y su informacion de depuracion.  Vive aqui y no
        # en quien la usa porque no tiene nada de un compilador -- cualquier
        # programa que quiera poner nombres a sus propias direcciones necesita
        # exactamente esto --, y porque es lo que le falta a esta libreria para
        # contestar sola de donde viene una reserva.
        #
        # Partido por sus costuras: lo que depende del FORMATO (PE frente a
        # ELF) en su propia unidad, y la API sin un solo `#if` de plataforma.
        src/symbols/self_image.cpp
        src/symbols/self_image_format.cpp
        src/symbols/self_symbols.cpp
        # Un fichero por formato, y el `#if` envuelve la unidad ENTERA: fuera
        # de su sistema no compilan nada.  Anadir un formato es anadir un
        # fichero, no otra rama dentro de los que ya estan.
        src/symbols/self_symbols_pe.cpp
        src/symbols/self_symbols_elf.cpp
        # El lector de DWARF, partido por sus costuras.  Estaba en un solo
        # fichero de mil setecientas lineas con todo dentro de un namespace
        # anonimo -- comodo de escribir, opaco de leer: nada obligaba a decir
        # que necesitaba cada parte de las demas.  Ahora esta escrito, en
        # `src/dwarf_internal.h`.
        src/symbols/dwarf/dwarf_cursor.cpp
        src/symbols/dwarf/dwarf_forms.cpp
        src/symbols/dwarf/dwarf_ranges.cpp
        src/symbols/dwarf/dwarf_die.cpp
        src/symbols/dwarf/dwarf_lines.cpp
        src/symbols/dwarf/dwarf_index.cpp
        src/symbols/self_dwarf.cpp
        # De quien es una direccion, preguntandoselo al CARGADOR.  Todo lo de
        # arriba lee la imagen PROPIA, y ninguno comprobaba que la direccion lo
        # fuera: una de otro modulo salia con el ultimo simbolo de nuestra
        # tabla, `_fini`, con la misma cara que un nombre cierto.
        src/symbols/module_symbols.cpp
        # El cable entre leerse a uno mismo y el gancho de la exportacion: con
        # una linea, el informe sale con nombres en vez de desplazamientos.
        src/symbols/self_resolver.cpp
        # La paridad para C: lo mismo que ve un programa en C++ lo ve uno en C.
        # El C++ de este proyecto LLAMA a codigo C y varias dependencias son C
        # puro, asi que una capacidad solo-C++ es una capacidad que la mitad
        # del programa no tiene.
        src/symbols/self_inspect_c.cpp
        src/report/alloc_sites_c.cpp
        src/alloc/scratch_arena.cpp
        src/os/thread_slot.cpp
)

# -----------------------------------------------------------------------------
#  Las dos fuentes que OTROS compilan por su cuenta, dichas desde aqui
# -----------------------------------------------------------------------------
#
# Hay quien necesita lo que esta libreria le pide al sistema -- la memoria y la
# ranura por hilo -- SIN el resto: en el proyecto de donde sale esto, el
# recolector de basura se compila freestanding (`-fno-exceptions`, `-fno-rtti`)
# para que su archivo enlace sin `g++`, asi que no puede usar los objetos de
# aqui aunque quiera.  Compilar el mismo fuente dos veces con banderas
# distintas es intencionado y no se arregla.
#
# Lo que si se arregla es que la RUTA estuviera escrita a mano en cada uno de
# esos sitios.  Eran cinco, y al reorganizar las carpetas se rompieron los
# cinco a la vez.  Diciendolas desde aqui, mudarlas se arregla en un sitio.
# Por SEPARADO y no en una lista: quien las usa no quiere las dos.  El
# recolector se lleva la memoria; los objetivos que necesitan la ranura por
# hilo se llevan la ranura.  Una lista conjunta obligaria a arrastrar la que no
# se usa, que en un objetivo freestanding no es un detalle.
if(NOT VESTA_ALLOC_STANDALONE)
    set(VESTA_ALLOC_OS_MEMORY_SRC
            ${CMAKE_CURRENT_SOURCE_DIR}/src/os/os_memory.cpp PARENT_SCOPE)
    set(VESTA_ALLOC_THREAD_SLOT_SRC
            ${CMAKE_CURRENT_SOURCE_DIR}/src/os/thread_slot.cpp PARENT_SCOPE)
endif()

# -----------------------------------------------------------------------------
#  LAS CABECERAS NO SE ENUMERAN.  Ver `install(DIRECTORY ...)`, al final.
# -----------------------------------------------------------------------------
#
# Habia aqui cuatro listas escritas a mano, una por subdirectorio, y su modo de
# fallar era el peor de todos: olvidarse de anadir una cabecera nueva no rompia
# nada al construir -- rompia en la maquina de quien INSTALARA la libreria, al
# incluir algo que en el arbol de fuentes si estaba.  Y ya habia pasado:
# faltaban `call_site.h`, `module_symbols.h` y `msvcrt_hook.h`.
#
# Se instala el arbol de `include/` entero.  La estructura se conserva sola y no
# queda nada que mantener al dia.

# -----------------------------------------------------------------------------
#  El objetivo principal: objetos.  Ver la nota de arriba.
# -----------------------------------------------------------------------------
add_library(vesta_alloc OBJECT ${VESTA_ALLOC_SOURCES})

# Las cabeceras se incluyen como `util/alloc/host_allocator.h`.  El prefijo
# `util/` se conserva porque es tambien el namespace (`util::`); lo que hay
# debajo son SEIS carpetas por lo que hace cada cosa, no por como se llama:
#
#   util/alloc/      el asignador y su geometria
#   util/interpose/  ser `malloc`: el parche, el gancho del runtime
#   util/report/     los sitios de reserva, las etiquetas, el volcado a CSV
#   util/symbols/    leerse a uno mismo: imagen, simbolos, DWARF, modulos
#   util/os/         lo que se le pide al sistema
#   util/mem/        las primitivas de copiar y rellenar
#
# Estaban las veinticuatro en el mismo sitio, que a partir de una docena deja
# de ser una lista y pasa a ser un monton donde hay que buscar.
target_include_directories(vesta_alloc PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(vesta_alloc PUBLIC cxx_std_17)

# -----------------------------------------------------------------------------
#  Las entradas en C, redirigidas al enlazar.  Ver la opcion, arriba.
# -----------------------------------------------------------------------------
#
# Solo con enlazadores que entienden `--wrap` (los de GNU y LLVM).  El de
# Apple y el de Microsoft no lo tienen, y ahi se DICE en vez de generar una
# linea de enlazado que falla con un mensaje sobre una opcion desconocida.
#
# Las banderas van por `INTERFACE`, asi que las hereda quien nos enlace sin
# tener que saber que existen.  Y tienen que ir por ahi a la fuerza: `--wrap`
# renombra las referencias PENDIENTES de un enlace, de modo que ponerlas solo
# al compilar esta libreria no redirigiria ni una llamada de las de fuera, que
# son justamente las que se quieren ver.
if(VESTA_ALLOC_INTERPOSE_MALLOC)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE AND NOT MSVC)
        target_sources(vesta_alloc PRIVATE src/interpose/malloc_interpose.cpp)
        # Las de alineacion van en la MISMA lista y no aparte: una reserva
        # alineada es una reserva, y dejarla fuera daria un informe al que le
        # falta justamente la memoria de los tipos que piden linea de cache o
        # pagina -- la que mas suele importar cuando se mira un informe.
        #
        # Pedir el renombrado de un simbolo que esta plataforma no tiene no
        # cuesta nada: `--wrap` solo actua sobre referencias PENDIENTES, y si
        # nadie llama a `posix_memalign` no hay ninguna que renombrar.  Por eso
        # la lista no se parte por sistema, salvo el par de Windows, que ademas
        # necesita su `__real_`.
        set(_wrapped malloc calloc realloc free
                memalign aligned_alloc posix_memalign)
        if(WIN32)
            # La familia ENTERA, no solo el par.  Las de mas abajo reciben un
            # puntero YA reservado, asi que dejarlas sin renombrar es dejar que
            # una de las nuestras acabe en el monton del runtime -- y eso no
            # falla en la llamada, falla mucho despues y en otro sitio.
            list(APPEND _wrapped _aligned_malloc _aligned_free
                    _aligned_realloc _aligned_recalloc _aligned_msize
                    _aligned_offset_malloc _aligned_offset_realloc)
            # Y LAS DOS QUE NO RESERVAN NADA, que es justo por lo que faltaban:
            # `_msize` y `_expand` cogen un puntero y se lo plantean al monton
            # NT, que comprueba y PARA EL PROCESO al ver uno nuestro.  Asi murio
            # una ventana dentro de `CreateWindowExW`, por la tabla de salida
            # que uxtheme reserva al cargarse.  No parecen de la lista de un
            # asignador, y esa es toda la razon de que se pasaran por alto.
            #
            # Cuales de las entradas de msvcrt hacian falta salio de BARRER su
            # codigo buscando referencias a las ranuras de importacion del
            # monton -- catorce sitios en seis funciones --, no de leer nombres.
            # Con estas dos la familia queda cerrada: lo alineado y las
            # variantes `_dbg` desembocan en ellas.
            list(APPEND _wrapped _msize _expand)
        endif()
        # Y los que la OTRA via define salen de la lista, porque las dos a la
        # vez no se pueden: `__real_malloc` se resolveria con nuestra propia
        # definicion y la primera reserva se llamaria a si misma.  Se quitan
        # aqui, y no en el otro bloque, para que la lista siga siendo un sitio
        # unico -- lo contrario es la forma de que un dia sobre uno.
        #
        # SON LOS CUATRO, y salen JUNTOS.  Con `malloc` nuestro y `realloc` de
        # la libc, la libreria reserva por nuestra puerta y redimensiona por la
        # suya, sobre un bloque cuya cabecera no escribio ella: `getcwd(NULL,0)`
        # pide con `malloc` y ENCOGE con `realloc`, y ahi murio el compilador en
        # Linux.  Partir la familia no es una version mas pequena del
        # mecanismo, es una que corrompe.
        if(VESTA_ALLOC_DEFINE_MALLOC)
            list(REMOVE_ITEM _wrapped malloc free realloc calloc)
        endif()
        foreach(_fn IN ITEMS ${_wrapped})
            # Como "libreria" y no con `target_link_options` a proposito: eso
            # es de CMake 3.13 y aqui el minimo es 3.12.  Una entrada que
            # empieza por `-` se pasa tal cual al enlazador, que es lo que hace
            # falta.
            target_link_libraries(vesta_alloc INTERFACE "-Wl,--wrap=${_fn}")
        endforeach()
    else()
        message(STATUS "[vesta_alloc] este enlazador no tiene --wrap: "
                "malloc/free se quedan en los del sistema y su memoria NO "
                "saldra en el informe")
    endif()
endif()

# -----------------------------------------------------------------------------
#  EL MODO COMPROBACION: un sanitizador propio del asignador
# -----------------------------------------------------------------------------
#
# Caza fugas, dobles liberaciones, punteros ajenos, desbordamientos y escrituras
# despues de liberar, y los dice con pila, `fichero:linea` y codigo de salida.
#
# POR QUE NUESTRO Y NO UNA HERRAMIENTA DE FUERA: valgrind es de Linux y en
# Windows con este juego de herramientas no hay equivalente usable, asi que
# remitir a la herramienta externa dejaria la MITAD de las plataformas sin poder
# mirar su propia memoria.  Y sale barato porque lo caro ya esta hecho: las
# puertas de reserva estan interpuestas en los dos sistemas, los simbolos se
# resuelven con nuestro lector DWARF y la tabla de exportacion de PE -- sin
# simbolizador externo -- y los permisos de pagina ya son nuestros.
#
# ES UNA MACRO, NO UNA VARIABLE DE ENTORNO, y por lo mismo que
# `VESTA_ALLOC_SIZE_HISTOGRAM`: un portillo en ejecucion costaria una carga y un
# salto POR RESERVA para no hacer nada, en todos los builds y para siempre.
# Apagada, cada entrada del comprobador es una funcion en linea vacia y
# `host_alloc`/`host_free` generan exactamente lo mismo que antes de que esto
# existiera.
#
# Lo que SI se elige en ejecucion, dentro de un build que ya lo lleva dentro, es
# cuanto comprueba: `VESTA_ALLOC_SAN` (nivel), `VESTA_ALLOC_SAN_POISON` (cuanto
# se envenena) y `VESTA_ALLOC_SAN_GUARD` (que borde vigila la pagina de guarda).
# Ahi un portillo no cuesta nada que merezca contarse -- ese build no existe
# para ir rapido -- y poder bajar el coste sin recompilar es lo que hace que el
# comprobador se llegue a usar.
option(VESTA_ALLOC_SANITIZER
        "Modo COMPROBACION: fugas, dobles liberaciones, desbordamientos y uso \
despues de liberar, con pila y fichero:linea.  Apagado no cuesta nada" OFF)

if(VESTA_ALLOC_SANITIZER)
    target_sources(vesta_alloc PRIVATE src/alloc/sanitizer.cpp)
    # PUBLIC porque cambia lo que hacen funciones EN LINEA de la cabecera: quien
    # nos incluya tiene que compilarse con la misma idea, o su `host_alloc` y el
    # nuestro dejarian de ser la misma funcion.
    target_compile_definitions(vesta_alloc PUBLIC VESTA_ALLOC_SANITIZER=1)
    message(STATUS "[vesta_alloc] modo COMPROBACION encendido: este build NO es "
            "para medir tiempos")
endif()

# -----------------------------------------------------------------------------
#  Y el fichero que DEFINE los simbolos, cuando se pide.  Ver la opcion arriba.
# -----------------------------------------------------------------------------
#
# Solo ELF.  En Windows no hay simbolo al que ganarle -- el llamante llega al
# CRT por la tabla de importacion --, asi que pedirlo ahi se DICE en vez de
# construir algo que no haria nada.
if(VESTA_ALLOC_DEFINE_MALLOC)
    if(WIN32 OR APPLE OR MSVC)
        message(FATAL_ERROR "[vesta_alloc] VESTA_ALLOC_DEFINE_MALLOC es de ELF: "
                "aqui el runtime de C se alcanza por la tabla de importacion y "
                "no hay definicion que gane. En Windows lo cubre "
                "malloc_interpose.cpp con sus punteros __imp__.")
    endif()
    target_sources(vesta_alloc PRIVATE src/interpose/malloc_define.cpp)
    # La marca la ven los DOS ficheros: uno para definir los simbolos, y el
    # otro para NO definir sus gemelos `__wrap_` -- que sin su `--wrap` no
    # tendrian `__real_` al que llamar -- y para conocer la arena de arranque,
    # cuyos bloques no son suyos ni de la libreria de C.
    #
    # Y va por `PUBLIC` porque no es un detalle interno: cambia que simbolos
    # EXISTEN en el enlace.  Quien enlace con nosotros y quiera el `malloc` de
    # la libreria de C ya no lo tiene en `__real_malloc` y tiene que pedirlo con
    # `dlsym`, asi que tiene derecho a saberlo al compilar.
    target_compile_definitions(vesta_alloc PUBLIC VESTA_ALLOC_DEFINE_MALLOC=1)
    # `dlsym` para encontrar el `free` y el `realloc` de la libreria de C, que
    # es lo que recibe los bloques del cargador dinamico.  En glibc 2.34 y
    # posteriores ya vive en `libc`, pero pedirlo no molesta y en las anteriores
    # hace falta.
    target_link_libraries(vesta_alloc PUBLIC ${CMAKE_DL_LIBS})
    message(STATUS "[vesta_alloc] malloc/free/realloc/calloc son simbolos "
            "NUESTROS (--wrap retirado para esos cuatro)")
endif()

# -----------------------------------------------------------------------------
#  El gancho del runtime de C en Windows.  Ver la opcion, arriba.
# -----------------------------------------------------------------------------
if(VESTA_ALLOC_HOOK_MSVCRT)
    if(NOT WIN32)
        message(FATAL_ERROR "[vesta_alloc] VESTA_ALLOC_HOOK_MSVCRT es de "
                "Windows: en ELF el mismo agujero lo cierra "
                "VESTA_ALLOC_DEFINE_MALLOC, definiendo el simbolo, que no "
                "necesita escribir en el codigo de nadie.")
    endif()
    target_sources(vesta_alloc PRIVATE src/interpose/msvcrt_hook.cpp)
    target_compile_definitions(vesta_alloc PUBLIC VESTA_ALLOC_HOOK_MSVCRT=1)
    # Y hay que ARRASTRAR el objeto al enlace.  De un archivo estatico solo
    # sale el objeto que resuelva un simbolo pendiente, y a este no lo llama
    # nadie -- el gancho se pone solo, por su callback de TLS --, asi que sin
    # esto no entraria y no pasaria nada, en silencio.  Por `INTERFACE`, como
    # las banderas de `--wrap`: quien nos enlaza lo hereda sin saber que existe.
    target_link_libraries(vesta_alloc INTERFACE "-Wl,-u,vesta_msvcrt_hook_anchor")
    message(STATUS "[vesta_alloc] el runtime de C reserva de este asignador "
            "(salto en msvcrt!malloc y companyia)")
endif()

# Que se pueda meter dentro de una libreria compartida.
set_target_properties(vesta_alloc PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    # `kernel32` trae `TlsAlloc` y `K32GetProcessMemoryInfo`.  No hace falta
    # `psapi`: la cabecera pide la version 2 de esa API, que vive en kernel32
    # -- ver `src/os/os_memory.cpp`.
    #
    # `ntdll` trae `NtAllocateVirtualMemory` y companyia, que es a quien se le
    # pide la memoria: `VirtualAlloc` solo valida argumentos y llama a esa.  Se
    # ENLAZAN en vez de resolverlas al arrancar, de forma que el sitio de
    # llamada quede en un salto por la tabla de importacion y no en una lectura
    # mas una comprobacion de nulo.  El motivo largo esta en `os_memory.cpp`.
    target_link_libraries(vesta_alloc PUBLIC kernel32 ntdll)
else()
    find_package(Threads REQUIRED)
    # `dl` trae `dladdr`, que es quien dice de que modulo es una direccion.  Va
    # SIEMPRE y no solo cuando se pide algo: el informe lo necesita para no
    # ponerle a una direccion ajena un nombre de nuestra tabla de simbolos.  En
    # glibc 2.34 y posteriores ya vive dentro de `libc` y `CMAKE_DL_LIBS` queda
    # vacio, asi que esto no anade nada donde no hace falta.
    target_link_libraries(vesta_alloc PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

# -----------------------------------------------------------------------------
#  Las bibliotecas de verdad, hechas de esos MISMOS objetos (sin recompilar).
# -----------------------------------------------------------------------------
if(VESTA_ALLOC_BUILD_STATIC)
    add_library(vesta_alloc_static STATIC $<TARGET_OBJECTS:vesta_alloc>)
    target_link_libraries(vesta_alloc_static PUBLIC vesta_alloc)
    set_target_properties(vesta_alloc_static PROPERTIES
            OUTPUT_NAME vesta_alloc)
    add_library(vesta_alloc::static ALIAS vesta_alloc_static)
endif()

if(VESTA_ALLOC_BUILD_SHARED)
    # AVISO, y esta en el README con mas detalle: en una biblioteca DINAMICA el
    # reemplazo de `operator new` no es fiable -- el ejecutable ya trae el suyo
    # y no lo cede --.  La interfaz en C (`vesta_host_alloc` y companyia) si
    # funciona igual.  Por eso esto esta APAGADO por defecto.
    add_library(vesta_alloc_shared SHARED $<TARGET_OBJECTS:vesta_alloc>)
    target_link_libraries(vesta_alloc_shared PUBLIC vesta_alloc)
    set_target_properties(vesta_alloc_shared PROPERTIES
            OUTPUT_NAME vesta_alloc
            VERSION ${PROJECT_VERSION}
            SOVERSION 1)
    add_library(vesta_alloc::shared ALIAS vesta_alloc_shared)
endif()

# -----------------------------------------------------------------------------
#  Pruebas.  Cada `.cpp` es un binario suelto que sale con 0 o con 1, que es la
#  convencion del proyecto de donde sale esto.
# -----------------------------------------------------------------------------
#
# Los ejecutables se agrupan en una carpeta del IDE (`FOLDER`) para que, metidos
# en un proyecto grande, no se pierdan entre los suyos.
#
# `enable_testing()` y `add_test` SOLO en modo suelto: el proyecto que nos
# incluye puede no usar ctest -- VestaVM no lo usa, cada prueba es un binario
# que sale con 0 o con 1 --, y registrar ahi pruebas ajenas ensuciaria su
# tanda.  Los binarios existen igual y se pueden lanzar desde el IDE.
# Lo que necesita el andamiaje de `support/`, que es lo unico del arbol que lee
# variables de entorno y escribe ficheros.
#
# El CRT de Microsoft marca `getenv` y `fopen` como obsoletas y propone sus
# propias `_s`.  No lo son: son ISO C, y las `_s` no existen fuera de Windows,
# asi que aceptar la sugerencia partiria en dos un codigo que hoy compila igual
# en los dos sistemas.  Lo que se apaga es el aviso de un proveedor sobre el
# estandar, no un aviso sobre este codigo -- y por eso se apaga SOLO aqui, en
# los tests y los bancos, y nunca en la libreria.
if(WIN32)
    set(VESTA_ALLOC_SUPPORT_DEFS _CRT_SECURE_NO_WARNINGS)
else()
    set(VESTA_ALLOC_SUPPORT_DEFS)
endif()

if(VESTA_ALLOC_BUILD_TESTS)
    if(VESTA_ALLOC_STANDALONE)
        enable_testing()
    endif()
    # La de la interposicion solo existe si la interposicion esta puesta: se
    # apoya en `__real_malloc`, que es un simbolo que crea `--wrap` y que sin
    # el no esta.  Sin esta condicion, apagar la opcion no daria una prueba de
    # menos sino un error de ENLACE, que es un modo de fallo bastante peor.
    set(_va_tests test_host_allocator test_host_spans test_alloc_tag
                  test_alloc_sites test_span_race test_counters_agree
                  test_variants test_thread_slot test_scratch_arena
                  test_mem_ops test_host_layout test_call_site
                  test_os_env test_alloc_csv test_module_symbols)
    if(VESTA_ALLOC_INTERPOSE_MALLOC AND
       CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE AND NOT MSVC)
        list(APPEND _va_tests test_malloc_interpose)
    endif()
    # La del modo COMPROBACION solo cuando el modo esta puesto.  Sin el, el
    # test no probaria nada: sus entradas son funciones vacias y sus cuatro
    # fallos a proposito no los caza nadie -- o sea que pasaria en verde
    # diciendo que todo esta bien, que es exactamente lo que un comprobador no
    # puede hacer.
    if(VESTA_ALLOC_SANITIZER)
        list(APPEND _va_tests test_sanitizer)
    endif()
    foreach(_t IN ITEMS ${_va_tests})
        add_executable(vesta_alloc_${_t} tests/${_t}.cpp
                $<TARGET_OBJECTS:vesta_alloc>)
        target_link_libraries(vesta_alloc_${_t} PRIVATE vesta_alloc)
        target_include_directories(vesta_alloc_${_t} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/support)
        target_compile_definitions(vesta_alloc_${_t} PRIVATE
                ${VESTA_ALLOC_SUPPORT_DEFS})
        set_target_properties(vesta_alloc_${_t} PROPERTIES
                FOLDER "vesta_alloc/tests")
        if(VESTA_ALLOC_STANDALONE)
            add_test(NAME vesta_alloc_${_t} COMMAND vesta_alloc_${_t})
        endif()
    endforeach()

    # El del comprobador necesita su entorno, y las dos variables por una razon
    # distinta cada una: el nivel, porque comprueba cosas que solo existen a
    # partir del 3; y el codigo de salida, porque ese programa DEBE estar lleno
    # de fallos y sin apagarlo el comprobador mataria al test que lo comprueba.
    if(VESTA_ALLOC_SANITIZER AND VESTA_ALLOC_STANDALONE)
        set_tests_properties(vesta_alloc_test_sanitizer PROPERTIES
                ENVIRONMENT "VESTA_ALLOC_SAN=3;VESTA_ALLOC_SAN_EXITCODE=0")
    endif()

    # -------------------------------------------------------------------------
    #  Que la documentacion BILINGUE lo sea de verdad
    # -------------------------------------------------------------------------
    #
    # La referencia se genera dos veces del mismo codigo, una por idioma, y
    # Doxygen se limita a NO ESCRIBIR lo que no tiene version en el idioma que
    # esta generando: no avisa, no falla, y la referencia coja se publica igual
    # de bien que la completa.  Esto genera las dos y las compara.
    #
    # Solo si estan las dos herramientas.  Que falte Doxygen no puede impedir
    # construir una libreria de memoria -- pero se DICE, porque una prueba que
    # desaparece en silencio es una prueba que nadie echa de menos.
    if(VESTA_ALLOC_STANDALONE)
        find_program(VESTA_ALLOC_DOXYGEN doxygen)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(VESTA_ALLOC_DOXYGEN AND Python3_Interpreter_FOUND)
            add_test(NAME vesta_alloc_doc_parity
                    COMMAND ${Python3_EXECUTABLE}
                            ${CMAKE_CURRENT_SOURCE_DIR}/doc/check_parity.py
                    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
        else()
            message(STATUS "[vesta_alloc] sin doxygen o sin python: la "
                    "comprobacion de la documentacion bilingue NO se ejecuta")
        endif()

    endif()
endif()

# -----------------------------------------------------------------------------
#  Ejemplos.  Estan en INGLES: son lo primero que lee quien se lleve esta
#  libreria, y ese lector viene de fuera.  La documentacion de las CABECERAS es
#  bilingue -- se genera en los dos idiomas del mismo codigo; ver
#  `doc/BILINGUAL.md` --, y los comentarios de dentro de un `.cpp` se quedan en
#  uno solo: son para quien mantiene esto, no salen en la referencia.
# -----------------------------------------------------------------------------
if(VESTA_ALLOC_BUILD_EXAMPLES)
    foreach(_e IN ITEMS basic_usage purpose_tags phase_arena symbol_report)
        add_executable(vesta_alloc_example_${_e} examples/${_e}.cpp
                $<TARGET_OBJECTS:vesta_alloc>)
        target_link_libraries(vesta_alloc_example_${_e} PRIVATE vesta_alloc)
        set_target_properties(vesta_alloc_example_${_e} PROPERTIES
                FOLDER "vesta_alloc/examples")
    endforeach()

    # Los de C se compilan COMO C, y eso no es un detalle de presentacion: es
    # lo unico que comprueba que `host_allocator_c.h` sea C de verdad.  Sin
    # esto, la cabecera podria dejar de serlo -- un `bool`, un argumento por
    # defecto, un namespace -- y nadie se enteraria hasta que un usuario en C
    # intentara incluirla.
    #
    # Hace falta que el proyecto tenga el lenguaje C habilitado.  Si no lo
    # tiene, se SALTAN y se DICE, en vez de fallar con un error del generador
    # que no explica nada.
    get_property(_va_langs GLOBAL PROPERTY ENABLED_LANGUAGES)
    if("C" IN_LIST _va_langs)
        # Y hay que forzar el enlazador de C++: el ejecutable solo tiene
        # fuentes en C, asi que CMake elegiria el de C y faltaria la biblioteca
        # estandar de C++.  Es la misma trampa que se encuentra cualquiera que
        # use esto desde un proyecto en C; esta escrito en el README.
        foreach(_e IN ITEMS c_basic c_library_hook c_mem_ops c_report c_symbol_report)
            add_executable(vesta_alloc_example_${_e} examples/${_e}.c
                    $<TARGET_OBJECTS:vesta_alloc>)
            target_link_libraries(vesta_alloc_example_${_e} PRIVATE vesta_alloc)
            set_target_properties(vesta_alloc_example_${_e} PROPERTIES
                    LINKER_LANGUAGE CXX
                    C_STANDARD 99
                    C_STANDARD_REQUIRED ON
                    FOLDER "vesta_alloc/examples")
        endforeach()
    else()
        message(STATUS "[vesta_alloc] sin lenguaje C habilitado: los ejemplos "
                "en C no se compilan (anadir C a project(... LANGUAGES ...))")
    endif()
endif()

# -----------------------------------------------------------------------------
#  Bancos de medida.  No entran en las pruebas: miden, no comprueban, y un
#  numero que fluctua no puede decidir si un cambio pasa o no.
# -----------------------------------------------------------------------------
if(VESTA_ALLOC_BUILD_BENCH)
    foreach(_b IN ITEMS bench_allocator bench_vs_malloc bench_reserve_cost
                        bench_memcpy bench_memset bench_contention
                        bench_operator_new)
        add_executable(vesta_alloc_${_b} bench/${_b}.cpp
                $<TARGET_OBJECTS:vesta_alloc>)
        target_link_libraries(vesta_alloc_${_b} PRIVATE vesta_alloc)
        # `support/` es andamiaje de lo que prueba y mide la libreria -- color y
        # veredictos --, no parte de su interfaz, por eso no esta en `include/`.
        target_include_directories(vesta_alloc_${_b} PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/support)
        set_target_properties(vesta_alloc_${_b} PROPERTIES
                FOLDER "vesta_alloc/bench")
    endforeach()
endif()

# -----------------------------------------------------------------------------
#  Instalacion, solo cuando se construye suelto.
# -----------------------------------------------------------------------------
if(VESTA_ALLOC_STANDALONE)
    include(GNUInstallDirs)
    # EL ARBOL ENTERO, y no una lista de ficheros por carpeta.  Habia cuatro
    # listas escritas a mano -- una por subdirectorio -- y su modo de fallar es
    # el peor: una cabecera nueva que se olvide de anadir no rompe nada aqui,
    # rompe en la maquina de quien INSTALE la libreria, al incluir algo que en
    # el arbol de fuentes si estaba.  Ya habia pasado: faltaban `call_site.h`,
    # `module_symbols.h` y `msvcrt_hook.h`.
    #
    # Copiando el arbol, la estructura de carpetas se conserva sola y no hay
    # nada que mantener al dia.
    install(DIRECTORY include/
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            FILES_MATCHING PATTERN "*.h")
    if(VESTA_ALLOC_BUILD_STATIC)
        install(TARGETS vesta_alloc_static
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    endif()
    if(VESTA_ALLOC_BUILD_SHARED)
        install(TARGETS vesta_alloc_shared
                LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
endif()
