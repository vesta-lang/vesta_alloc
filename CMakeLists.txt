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

set(VESTA_ALLOC_SOURCES
        src/host_allocator.cpp
        src/host_allocator_c.cpp
        src/os_memory.cpp
        src/scratch_arena.cpp
        src/thread_slot.cpp
)

set(VESTA_ALLOC_HEADERS
        include/util/alloc_tag.h
        include/util/host_allocator.h
        include/util/host_allocator_c.h
        include/util/host_allocator_layout.h
        include/util/os_memory.h
        include/util/scratch_arena.h
        include/util/thread_slot.h
        include/util/vesta_memcpy.h
        include/util/vesta_memset.h
)

# Las primitivas de memoria van por dentro de `util/mem/`, una carpeta por
# arquitectura.  Se instalan aparte porque conservan su subdirectorio: las dos
# cabeceras de arriba las incluyen por ruta.
set(VESTA_ALLOC_MEM_HEADERS
        include/util/mem/mem_config.h
        include/util/mem/mem_inline.h
)
set(VESTA_ALLOC_MEM_X86_HEADERS
        include/util/mem/x86/avx2_memcpy.h
        include/util/mem/x86/avx2_memset.h
        include/util/mem/x86/erms_memcpy.h
        include/util/mem/x86/erms_memset.h
        include/util/mem/x86/sse2_memcpy.h
        include/util/mem/x86/sse2_memset.h
        include/util/mem/x86/x86_cpu.h
        include/util/mem/x86/x86_cpuid.h
        include/util/mem/x86/x86_vec.h
)
set(VESTA_ALLOC_MEM_GENERIC_HEADERS
        include/util/mem/generic/scalar_memcpy.h
        include/util/mem/generic/scalar_memset.h
)

# -----------------------------------------------------------------------------
#  El objetivo principal: objetos.  Ver la nota de arriba.
# -----------------------------------------------------------------------------
add_library(vesta_alloc OBJECT ${VESTA_ALLOC_SOURCES})

# Las cabeceras se incluyen como `util/host_allocator.h`.  El prefijo se
# conserva porque es tambien el namespace (`util::`), y porque cambiarlo
# obligaria a tocar los cuarenta y dos ficheros que ya las incluyen.
target_include_directories(vesta_alloc PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

target_compile_features(vesta_alloc PUBLIC cxx_std_17)

# Que se pueda meter dentro de una libreria compartida.
set_target_properties(vesta_alloc PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    # `kernel32` trae `VirtualAlloc`, `TlsAlloc` y `K32GetProcessMemoryInfo`.
    # No hace falta `psapi`: la cabecera pide la version 2 de esa API, que vive
    # en kernel32 -- ver `src/os_memory.cpp`.
    target_link_libraries(vesta_alloc PUBLIC kernel32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(vesta_alloc PUBLIC Threads::Threads)
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
if(VESTA_ALLOC_BUILD_TESTS)
    if(VESTA_ALLOC_STANDALONE)
        enable_testing()
    endif()
    foreach(_t IN ITEMS test_host_allocator test_host_spans test_alloc_tag
                        test_single_owner test_thread_slot test_scratch_arena
                        test_mem_ops)
        add_executable(vesta_alloc_${_t} tests/${_t}.cpp
                $<TARGET_OBJECTS:vesta_alloc>)
        target_link_libraries(vesta_alloc_${_t} PRIVATE vesta_alloc)
        set_target_properties(vesta_alloc_${_t} PROPERTIES
                FOLDER "vesta_alloc/tests")
        if(VESTA_ALLOC_STANDALONE)
            add_test(NAME vesta_alloc_${_t} COMMAND vesta_alloc_${_t})
        endif()
    endforeach()
endif()

# -----------------------------------------------------------------------------
#  Ejemplos.  Estan en INGLES a proposito, al reves que el resto del proyecto:
#  son lo primero que lee quien se lleve esta libreria, y ese lector viene de
#  fuera.  Los comentarios de la libreria siguen en espanyol.
# -----------------------------------------------------------------------------
if(VESTA_ALLOC_BUILD_EXAMPLES)
    foreach(_e IN ITEMS basic_usage purpose_tags phase_arena)
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
        foreach(_e IN ITEMS c_basic c_library_hook c_mem_ops)
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
                        bench_memcpy bench_memset)
        add_executable(vesta_alloc_${_b} bench/${_b}.cpp
                $<TARGET_OBJECTS:vesta_alloc>)
        target_link_libraries(vesta_alloc_${_b} PRIVATE vesta_alloc)
        set_target_properties(vesta_alloc_${_b} PROPERTIES
                FOLDER "vesta_alloc/bench")
    endforeach()
endif()

# -----------------------------------------------------------------------------
#  Instalacion, solo cuando se construye suelto.
# -----------------------------------------------------------------------------
if(VESTA_ALLOC_STANDALONE)
    include(GNUInstallDirs)
    install(FILES ${VESTA_ALLOC_HEADERS}
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/util)
    install(FILES ${VESTA_ALLOC_MEM_HEADERS}
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/util/mem)
    install(FILES ${VESTA_ALLOC_MEM_X86_HEADERS}
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/util/mem/x86)
    install(FILES ${VESTA_ALLOC_MEM_GENERIC_HEADERS}
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/util/mem/generic)
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
