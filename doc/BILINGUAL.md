# La documentacion del codigo es bilingue

*[English below](#bilingual-code-documentation)*

La referencia de esta libreria se genera **dos veces del mismo codigo**, una en
ingles y otra en espanol, con los marcadores `\~` de Doxygen.

```sh
doxygen doc/Doxyfile.en        # -> doc/out/en/html
doxygen doc/Doxyfile.es        # -> doc/out/es/html
python  doc/check_parity.py    # comprueba que las dos estan completas
```

## La convencion, en una linea

**Cada elemento se abre con `\~english`, sigue con `\~spanish` y se CIERRA con
`\~` a secas.**

```cpp
/**
 * @brief
 * \~english Allocates @p n bytes.
 * \~spanish Reserva @p n bytes.
 * \~
 *
 * \~english
 * The block comes from a per-thread free list, so the fast path
 * synchronises nothing at all.
 *
 * \~spanish
 * El bloque sale de una lista de libres por hilo, asi que el camino
 * rapido no sincroniza nada.
 *
 * \~
 * @param n  \~english how many bytes  \~spanish cuantos bytes  \~
 * @return   \~english the block, or null  \~spanish el bloque, o nulo  \~
 */
```

Elemento es cada `@brief`, cada parrafo, cada `@param`, el `@return` y cada
`@par`.  El `@file` tambien.

## Por que el cierre no es opcional

`\~english` no marca *una frase*: **cambia el idioma activo hasta el siguiente
marcador**.  Sin el `\~` de cierre, todo lo que venga detras -- el parrafo
siguiente, los `@param`, el `@return` -- se queda en el ultimo idioma que se
nombro, y desaparece del otro.

Y desaparece **en silencio**.  Doxygen no avisa: se limita a no escribirlo.  Una
referencia a la que le falta la mitad de los parametros se publica igual de bien
que una completa.

Eso es lo que comprueba `doc/check_parity.py`: genera las dos, compara los dos
arboles elemento a elemento y falla diciendo cual se quedo sin traducir.  Sin
esa comprobacion, esta convencion no seria sostenible -- y con ella, olvidarse
es un rojo en vez de un hueco que nadie ve.

## Los ejemplos: NUNCA marcadores dentro de `@code`

Dentro de un bloque `@code ... @endcode` Doxygen no interpreta comandos, asi que
un `\~english` ahi dentro **sale impreso tal cual** en la referencia.  Si el
ejemplo lleva comentarios, van dos bloques enteros, uno por idioma, con los
marcadores **fuera**:

```cpp
/**
 * \~english
 * @code
 * void *p = util::host_alloc(64);
 * if (p == nullptr) return;          // out of memory
 * @endcode
 *
 * \~spanish
 * @code
 * void *p = util::host_alloc(64);
 * if (p == nullptr) return;          // sin memoria
 * @endcode
 *
 * \~
 */
```

Si el ejemplo no lleva comentarios -- que suele ser lo mejor --, con un solo
bloque basta: el codigo se lee igual en los dos idiomas.

## Como se mide lo que falta

`check_parity.py` cuenta ademas los elementos con el **mismo texto en los dos
idiomas**, que casi siempre significa que ese comentario todavia no se ha
tocado.  Ese numero bajando es el progreso; no llegara a cero del todo, porque
alguna cosa se escribe igual en los dos idiomas a proposito.

---

# Bilingual code documentation

The reference for this library is generated **twice from the same code**, once
in English and once in Spanish, using Doxygen's `\~` markers.

```sh
doxygen doc/Doxyfile.en        # -> doc/out/en/html
doxygen doc/Doxyfile.es        # -> doc/out/es/html
python  doc/check_parity.py    # checks that both are complete
```

## The convention, in one line

**Every element opens with `\~english`, continues with `\~spanish`, and CLOSES
with a bare `\~`.**  See the example above.

An element is each `@brief`, each paragraph, each `@param`, the `@return` and
each `@par`.  The `@file` block too.

## Why the closing marker is not optional

`\~english` does not mark *a sentence*: it **changes the active language until
the next marker**.  Without the closing `\~`, everything after it — the next
paragraph, the `@param`s, the `@return` — stays in the last language named, and
vanishes from the other one.

And it vanishes **silently**.  Doxygen does not warn; it simply does not write
it.  A reference missing half its parameters publishes just as well as a
complete one.

That is what `doc/check_parity.py` checks: it generates both, compares the two
trees element by element, and fails naming whichever was left untranslated.
Without that check this convention would not be sustainable — with it,
forgetting is a red test instead of a hole nobody sees.
