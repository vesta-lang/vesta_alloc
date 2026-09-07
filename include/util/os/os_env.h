/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/os/os_env.h
 * @brief
 * \~english Reading the environment from the SYSTEM, not from the C runtime.
 * \~spanish Leer el entorno del SISTEMA, no del runtime de C.
 * \~
 *
 * \~spanish
 * POR QUE EXISTE ESTO, SI `getenv` ES UNA LINEA.  Porque `getenv` no lee el
 * entorno: lee una COPIA que el runtime de C construye mientras arranca, y esta
 * libreria corre antes de que esa copia exista.
 *
 * El asignador se inicializa en la primera reserva, y la primera reserva puede
 * ocurrir durante la inicializacion estatica -- cualquier global cuyo
 * constructor pida memoria llega ahi antes que `main` --.  Si la tabla de
 * entorno del runtime de C esta construida para entonces no es algo que decida
 * esta libreria: depende del runtime y del orden de enlazado.  Cuando no lo
 * esta, `getenv` devuelve nulo, todos los interruptores se leen como apagados,
 * y no falla nada ni se imprime nada -- sencillamente no se mide.
 *
 * Esa es la peor forma que puede tener un fallo aqui.  Es invisible, depende de
 * que global se construyera primero, e iria y vendria segun se anade codigo que
 * no tiene nada que ver.  Asi que, en vez de establecer si el runtime de hoy y
 * el orden de enlazado de hoy resultan ser seguros, la pregunta se le hace a
 * algo que esta SIEMPRE listo.
 *
 * QUE HACE EN SU LUGAR.  Va a donde vive de verdad el entorno, el bloque que el
 * nucleo le entrego al proceso:
 *
 *   Windows  el PEB, al que se llega con `NtQueryInformationProcess`.  El
 *            nucleo lo rellena antes de la primera instruccion del proceso, asi
 *            que no hay un "demasiado pronto".
 *   POSIX    `environ`, al que el cargador apunta en la pila inicial antes de
 *            que corra ningun constructor.
 *
 * Significa tambien que esta libreria no necesita al runtime de C para
 * contestar una pregunta sobre el proceso -- la misma razon por la que
 * `util/os/os_memory.h` habla con ntdll y no con kernel32.
 *
 * REGLA DE ESTE MODULO, la misma que la de `os_memory`: **aqui no se reserva,
 * no se imprime y no se lanza.**  Esta por debajo del asignador, asi que no hay
 * a que caerse; un fallo es un valor de retorno.
 *
 * \~english
 * WHY THIS EXISTS, WHEN `getenv` IS ONE LINE.  Because `getenv` does not read
 * the environment: it reads a COPY that the C runtime builds while it starts
 * up, and this library runs before that copy exists.
 *
 * The allocator initialises on the first allocation, and the first allocation
 * can happen during static initialisation -- any global whose constructor asks
 * for memory gets there before `main`.  Whether the C runtime's environment
 * table is built by then is not something this library gets to decide: it
 * depends on the runtime, and on link order.  When it is not, `getenv` returns
 * null, every switch reads as off, and nothing fails and nothing is printed --
 * measurement simply does not happen.
 *
 * That is the worst shape a bug can have here.  It is invisible, it depends on
 * which global happened to be constructed first, and it would come and go as
 * unrelated code is added.  So rather than establish whether today's runtime
 * and today's link order happen to be safe, the question is asked of something
 * that is ALWAYS ready.
 *
 * WHAT THIS DOES INSTEAD.  It goes to where the environment actually lives, the
 * block the kernel handed the process:
 *
 *   Windows  the PEB, reached with `NtQueryInformationProcess`.  The kernel
 *            fills it in before the first instruction of the process runs, so
 *            there is no "too early".
 *   POSIX    `environ`, which the loader points at the initial stack before any
 *            constructor runs.
 *
 * It also means this library does not need the C runtime to answer a question
 * about the process -- the same reason `util/os/os_memory.h` talks to ntdll rather
 * than to kernel32.
 *
 * RULE OF THIS MODULE, the same as `os_memory`: **nothing here allocates,
 * prints or throws.**  It sits below the allocator, so there is nothing to fall
 * back to; a failure is a return value.
 *
 * \~
 */
#ifndef VESTA_UTIL_OS_ENV_H
#define VESTA_UTIL_OS_ENV_H

#include <cstddef>

namespace util {

/// \~english Returned by @c os_env when the variable is not in the environment
///           at all.  Distinct from a variable that IS set and empty, which
///           returns 0 -- the two mean different things to whoever asked, and
///           merging them hides one of them.
/// \~spanish Lo que devuelve @c os_env cuando la variable no esta en el entorno
///           en absoluto.  Distinto de una variable que SI esta puesta y vacia,
///           que devuelve 0 -- las dos significan cosas distintas para quien
///           pregunta, y juntarlas esconde una de ellas.
/// \~
inline constexpr size_t kOsEnvUnset = static_cast<size_t>(-1);

/**
 * @brief
 * \~english Copies the value of an environment variable into a caller's buffer.
 * \~spanish Copia el valor de una variable de entorno en un bufer de quien
 *          llama.
 * \~
 *
 * \~english
 * The return value is the length the value HAS, not the length that was
 * written: a result >= @p cap means it did not fit and what is in @p buf is
 * truncated.  That is on purpose -- returning the truncated length would hand
 * back a short value that looks complete, which is the kind of quiet wrong
 * answer this project does not accept.
 *
 * The buffer is the caller's because there is nowhere to put one: allocating
 * here would call the allocator, and the allocator is what calls this.
 *
 * @note On Windows the environment is UTF-16.  Values are narrowed byte by
 *       byte and anything outside ASCII becomes `?`.  What this library asks
 *       about are switches, so a value that needs more than ASCII is already
 *       not an answer to the question -- but it is visible rather than silently
 *       mangled into something that could parse.
 *
 * @par Threads
 * Safe.  Reads a block the process does not modify through this path; it does
 * NOT see a later `setenv`/`SetEnvironmentVariable`, which is what makes it
 * usable before the C runtime exists.
 *
 * \~spanish
 * Lo que devuelve es la longitud que TIENE el valor, no la que se escribio: un
 * resultado >= @p cap significa que no cabia y que lo que hay en @p buf esta
 * truncado.  Es a proposito -- devolver la longitud truncada entregaria un
 * valor corto con pinta de completo, que es la clase de respuesta callada y
 * equivocada que este proyecto no acepta.
 *
 * El bufer es de quien llama porque no hay donde poner uno: reservar aqui
 * llamaria al asignador, y el asignador es quien llama a esto.
 *
 * @note En Windows el entorno es UTF-16.  Los valores se estrechan byte a byte
 *       y todo lo que quede fuera de ASCII se convierte en `?`.  Lo que esta
 *       libreria pregunta son interruptores, asi que un valor que necesite mas
 *       que ASCII ya no es una respuesta a la pregunta -- pero se ve, en vez de
 *       quedar destrozado en silencio en algo que si podria interpretarse.
 *
 * @par Hilos
 * Segura.  Lee un bloque que el proceso no modifica por este camino; NO ve un
 * `setenv`/`SetEnvironmentVariable` posterior, que es justo lo que la hace
 * utilizable antes de que exista el runtime de C.
 *
 * \~
 * @param name
 * \~english the variable name, ASCII.  Case-insensitive on Windows, exact on
 *           POSIX -- that is the system's rule, not ours.
 * \~spanish el nombre de la variable, en ASCII.  Sin distinguir mayusculas en
 *           Windows, exacto en POSIX -- esa es la regla del sistema, no
 *           nuestra.
 * \~
 * @param buf
 * \~english where the value goes.  Always nul-terminated when @p cap > 0.
 * \~spanish donde va el valor.  Siempre terminado en nulo cuando @p cap > 0.
 * \~
 * @param cap
 * \~english the size of @p buf in bytes.
 * \~spanish el tamano de @p buf en bytes.
 * \~
 * @return
 * \~english the FULL length of the value, or @c kOsEnvUnset if it is not set.
 * \~spanish la longitud COMPLETA del valor, o @c kOsEnvUnset si no esta puesta.
 * \~
 *
 * \~english
 * @code
 *   char buf[32];
 *   const size_t n = util::os_env("VESTA_HOST_ALLOC_TAG", buf, sizeof(buf));
 *   if (n == util::kOsEnvUnset) return kDefaultTag;   // nobody asked
 *   if (n >= sizeof(buf)) return kDefaultTag;         // asked for nonsense
 * @endcode
 *
 * \~spanish
 * @code
 *   char buf[32];
 *   const size_t n = util::os_env("VESTA_HOST_ALLOC_TAG", buf, sizeof(buf));
 *   if (n == util::kOsEnvUnset) return kDefaultTag;   // nadie lo pidio
 *   if (n >= sizeof(buf)) return kDefaultTag;         // pidieron un disparate
 * @endcode
 *
 * \~
 */
size_t os_env(const char *name, char *buf, size_t cap) noexcept;

/**
 * @brief
 * \~english The question this library actually asks: is this switch on?
 * \~spanish La pregunta que esta libreria hace de verdad: esta encendido este
 *          interruptor?
 * \~
 *
 * \~english
 * One place decides what "on" means, so `VESTA_HOST_ALLOC_STATS=0` cannot mean
 * one thing here and the opposite three files away.
 *
 * @par Threads
 * Safe.  Allocates nothing.
 *
 * \~spanish
 * Un solo sitio decide que significa "encendido", asi que
 * `VESTA_HOST_ALLOC_STATS=0` no puede significar una cosa aqui y la contraria
 * tres ficheros mas alla.
 *
 * @par Hilos
 * Segura.  No reserva nada.
 *
 * \~
 * @param name
 * \~english the variable name, ASCII.
 * \~spanish el nombre de la variable, en ASCII.
 * \~
 * @return
 * \~english true when the variable is set, non-empty, and not exactly `"0"`.
 * \~spanish true cuando la variable esta puesta, no vacia, y no es exactamente
 *           `"0"`.
 * \~
 *
 * \~english
 * @code
 *   // Read ONCE, before the first allocation: switching measurement on later
 *   // would leave everything from start-up out without saying so.
 *   const bool sites = util::os_env_flag("VESTA_HOST_ALLOC_SITES");
 * @endcode
 *
 * \~spanish
 * @code
 *   // Se lee UNA vez, antes de la primera reserva: encender la medicion mas
 *   // tarde dejaria fuera todo el arranque sin decirlo.
 *   const bool sites = util::os_env_flag("VESTA_HOST_ALLOC_SITES");
 * @endcode
 *
 * \~
 */
bool os_env_flag(const char *name) noexcept;

} // namespace util

#endif // VESTA_UTIL_OS_ENV_H
