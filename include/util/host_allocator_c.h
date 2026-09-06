/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/host_allocator_c.h
 * @brief El asignador del proyecto, con la firma que entiende C.
 *
 * POR QUE EXISTE.  Porque hay memoria que hoy NO VEMOS: las librerias escritas
 * en C que llevamos dentro -- Capstone, SQLite, OpenSSL, miniz -- llaman a
 * `malloc` directamente, asi que no pasan por nuestro `operator new` y no
 * aparecen en ninguna cuenta.  Casi todas ofrecen un gancho para cambiarles el
 * asignador, y ese gancho pide punteros a funcion con la firma de `malloc`:
 * eso es lo que hay aqui.
 *
 * Y de paso sirve a los plugins nativos, que ya hablan C
 * (`include/ffi/vesta_plugin.h`).
 *
 * QUE SE GANA.  Que esa memoria entre en el mismo sitio que la nuestra: los
 * mismos contadores, el mismo reparto por proposito, la misma region.  Con eso
 * se puede contestar por fin cuanto pesa cada libreria -- por ejemplo la
 * compresion de la cache, que ya sabemos que se lleva un 5,4% de las
 * instrucciones de una compilacion en frio, pero de cuya memoria no sabemos
 * nada.
 *
 * QUE CUESTA.  Desde C no se puede meter el camino rapido dentro de quien
 * llama, asi que se paga una llamada por reserva.  Es exactamente lo que ya
 * paga quien llama a `malloc`, asi que no pierde nada: simplemente no gana la
 * parte que si gana el C++.
 *
 * ESTA CABECERA ES C PURO.  Nada de plantillas, referencias ni namespaces, y no
 * incluye ninguna cabecera de C++.  Se puede incluir desde un `.c`.
 */
#ifndef VESTA_UTIL_HOST_ALLOCATOR_C_H
#define VESTA_UTIL_HOST_ALLOCATOR_C_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Sirve @p n bytes.  Equivalente a `malloc`.
 * @return NULL si no se pudo.  Con @p n a cero devuelve un bloque valido de
 *         tamano minimo, como hace `malloc` en las implementaciones normales.
 *
 * Lo devuelto vale para cualquier tipo: sale alineado a 16 bytes.
 *
 * @par Hilos
 * **Segura desde cualquier hilo**, y sin cerrojos en el camino normal: cada
 * hilo tiene sus propias listas.  Ver `util/host_allocator.h` para el detalle.
 *
 * @code
 *   // Enchufar el asignador del proyecto a Capstone.
 *   cs_opt_mem mem;
 *   mem.malloc   = vesta_host_alloc;
 *   mem.calloc   = vesta_host_calloc;
 *   mem.realloc  = vesta_host_realloc;
 *   mem.free     = vesta_host_free;
 *   mem.vsnprintf = vsnprintf;
 *   cs_option(0, CS_OPT_MEM, (size_t)&mem);
 *   // A partir de aqui, lo que reserve Capstone sale en nuestros contadores.
 * @endcode
 */
void *vesta_host_alloc(size_t n);

/**
 * @brief Sirve @p count elementos de @p size bytes, PUESTOS A CERO.
 * @return NULL si no se pudo o si el producto se desbordaria.
 *
 * @par Hilos
 * **Segura desde cualquier hilo**, igual que @c vesta_host_alloc.
 *
 * @code
 *   int *v = (int *)vesta_host_calloc(n, sizeof(int));   // n enteros a cero
 *   if (v == NULL) return -1;
 * @endcode
 */
void *vesta_host_calloc(size_t count, size_t size);

/**
 * @brief Cambia el tamano de @p p a @p n bytes.  Equivalente a `realloc`.
 * @return NULL si no se pudo; en ese caso @p p SIGUE SIENDO VALIDO y hay que
 *         liberarlo, igual que manda `realloc`.
 *
 * Con @p p a NULL equivale a @c vesta_host_alloc.  Con @p n a cero libera y
 * devuelve NULL.
 *
 * El tamano viejo NO hace falta pasarlo: de un puntero se saca su trozo con una
 * mascara, y del trozo su clase.  Por eso se puede copiar lo justo.
 *
 * @par Hilos
 * **Segura desde cualquier hilo.**  Vale aunque @p p lo reservara OTRO hilo.
 *
 * @code
 *   char *buf = (char *)vesta_host_alloc(64);
 *   char *mas = (char *)vesta_host_realloc(buf, 256);
 *   if (mas == NULL) { vesta_host_free(buf); return -1; }  // buf sigue vivo
 *   buf = mas;
 * @endcode
 */
void *vesta_host_realloc(void *p, size_t n);

/**
 * @brief Devuelve un bloque.  Equivalente a `free`.  Con NULL no hace nada.
 *
 * @par Hilos
 * **Segura desde cualquier hilo**, incluso si lo reservo otro: en ese caso el
 * bloque va a una pila atomica de su dueno, sin bloquear a nadie.
 *
 * @code
 *   vesta_host_free(buf);
 * @endcode
 */
void vesta_host_free(void *p);

/**
 * @brief Cuanto mide de verdad el bloque @p p.
 * @return Los bytes UTILIZABLES, que pueden ser mas de los pedidos porque el
 *         tamano se redondea a una clase.  Cero si @p p no es nuestro.
 *
 * Sirve para lo que en otras librerias se llama `malloc_usable_size`: permite
 * aprovechar el redondeo en vez de desperdiciarlo.
 *
 * @par Hilos
 * **Segura desde cualquier hilo.**  Solo lee la cabecera del trozo.
 *
 * @code
 *   // Crecer sin pedir nada si el redondeo ya daba de sobra.
 *   if (vesta_host_usable_size(buf) >= necesito) { ... }
 * @endcode
 */
size_t vesta_host_usable_size(const void *p);

/**
 * @brief Declara para que se reserva en ESTE hilo, hasta que se quite.
 * @param use   0 no se, 1 instantaneo, 2 medio, 3 largo.
 * @param shape 0 no se, 1 fijo, 2 creciente.
 * @return La etiqueta que habia puesta, para devolverla despues.
 *
 * Es el equivalente en C de `util::AllocScope`.  En C no hay destructores, asi
 * que restaurar es cosa del que llama -- y hay que hacerlo, porque una etiqueta
 * que se queda puesta contamina todo lo que ese hilo reserve despues, y eso no
 * se nota mirando: el reparto sale plausible pero equivocado.
 *
 * @par Hilos
 * **Segura desde cualquier hilo**, y afecta SOLO al que llama.  Si el trabajo
 * se reparte a otros hilos, la etiqueta no viaja sola.
 *
 * @code
 *   // Lo que reserve la descompresion se cuenta como instantaneo y fijo.
 *   const unsigned antes = vesta_host_push_tag(1, 1);
 *   descomprimir(...);
 *   vesta_host_pop_tag(antes);
 * @endcode
 */
unsigned vesta_host_push_tag(unsigned use, unsigned shape);

/**
 * @brief Vuelve a poner la etiqueta que devolvio @c vesta_host_push_tag.
 *
 * @par Hilos
 * **Segura desde cualquier hilo**, y afecta solo al que llama.
 */
void vesta_host_pop_tag(unsigned previous);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // VESTA_UTIL_HOST_ALLOCATOR_C_H
