/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: MIT (ver LICENSE).  Parte de la familia de VestaVM.
 */

/**
 * @file util/scratch_arena.h
 * @brief Memoria de USAR Y TIRAR para una fase, con reserva a puntero.
 *
 * PARA QUE SIRVE.  Cuando toda la memoria de una fase muere junta, un asignador
 * de proposito general esta haciendo un trabajo que nadie le ha pedido: llevar
 * la cuenta de cada bloque para poder devolverlo por separado.  Aqui reservar
 * es avanzar un puntero y devolver no es nada; la fase entera se suelta con una
 * escritura.  Medido contra el asignador general del proyecto: 1,95 ns por
 * reserva frente a 10,03, o sea 5,2x (y ~22x frente al `malloc` del sistema).
 *
 * CUANDO **NO** SIRVE -- leer esto antes de usarla.  Que la vida util encaje no
 * basta: tambien tiene que encajar el PATRON de reserva.  Una arena no puede
 * reclamar nada hasta el final, y un contenedor que CRECE (`std::vector` y
 * companyia) reserva un bufer nuevo y abandona el viejo en cada crecimiento.
 * Metiendo el analisis de rangos aqui -- cuya vida util encajaba
 * perfectamente -- el pico de memoria paso de 2.414 MB a 5.455 MB (+126%) a
 * cambio de un 4% de velocidad, porque son mas de un millon de inserciones que
 * van dejando restos.  Para reservas pequenas y repetidas de contenedores que
 * crecen, lo correcto es el asignador por clases de `util/host_allocator.h`,
 * que al crecer SI devuelve el bufer viejo.
 *
 * Esta arena encaja con: muchos objetos pequenos de tamano CONOCIDO que no se
 * redimensionan, nodos que se enlazan, buferes de trabajo de una pasada.
 *
 * Y nada de lo que salga de aqui puede sobrevivir a su fase: si escapa, al
 * reciclar queda apuntando a memoria reutilizada, y ese fallo aparece lejisimos
 * de su causa.  Comprobarlo ANTES, no despues.
 *
 * MARCA Y VUELTA, no un reinicio a secas: se apunta donde estaba y se vuelve
 * ahi.  Asi dos fases anidadas no se pisan, que un reinicio global si haria.
 *
 * REUSA lo que ya hay: los bloques salen de `util/os_memory.h` -- la unica capa
 * que habla de memoria con el sistema por debajo de las reservas -- y la arena
 * de cada hilo se localiza con `ThreadSlot`, que evita la TLS emulada de MinGW
 * (10,83 ns por acceso frente a 0,65).
 */
#ifndef VESTA_UTIL_SCRATCH_ARENA_H
#define VESTA_UTIL_SCRATCH_ARENA_H

#include <cstddef>
#include <cstdint>
#include <new>

namespace util {

/**
 * @brief Arena de fase: se reserva avanzando y se suelta toda de golpe.
 *
 * @par Hilos
 * **NINGUN metodo de esta clase es seguro entre hilos, y es EL DISEÑO.**  Una
 * arena vale 1,95 ns por reserva porque reservar es sumar a un puntero; meter
 * ahi un atomico o un cerrojo costaria mas que todo el trabajo que hace y la
 * dejaria sin motivo para existir.
 *
 * La seguridad sale de otro sitio: **cada hilo tiene la suya** y la encuentra
 * con @c scratch_arena().  Mientras nadie pase punteros a una arena entre
 * hilos, no hay nada que sincronizar.
 *
 * Lo que eso PROHIBE, y hay que tenerlo presente porque no da error:
 *   - guardar una `ScratchArena&` y usarla desde otro hilo;
 *   - repartir por el pool trabajo que reserve de una arena tomada fuera;
 *   - que un objeto reservado aqui lo LIBERE otro hilo (no hay liberar, pero
 *     tampoco puede seguir vivo cuando la fase se recicle).
 */
class ScratchArena {
  public:
    /// Un bloque grande de donde se van repartiendo trozos.
    struct Block {
        Block *next;
        size_t size; ///< bytes utiles detras de esta cabecera
        size_t used;
    };

    /// Donde estaba la arena, para poder volver.
    struct Mark {
        Block *block;
        size_t used;
    };

    /**
     * @brief Reserva @p n bytes alineados a @p align.  nullptr si no hay.
     *
     * @par Hilos
     * **NO segura**, a proposito: solo el hilo dueño de esta arena.
     *
     * @code
     *   // Un bufer de trabajo de una pasada, sin pasar por el asignador.
     *   util::ScratchScope fase;                       // marca al entrar
     *   auto *tabla = static_cast<Nodo *>(
     *       util::scratch_arena().allocate(n * sizeof(Nodo), alignof(Nodo)));
     *   if (tabla == nullptr) return false;            // sin memoria
     *   // ...usar `tabla`; al salir del ambito se recicla todo de golpe.
     * @endcode
     */
    void *allocate(size_t n, size_t align) noexcept;

    /**
     * @brief Apunta el estado actual, para poder volver aqui.
     *
     * @par Hilos
     * **NO segura**: solo el hilo dueño.
     */
    Mark mark() const noexcept {
        return Mark{current_, current_ ? current_->used : 0};
    }

    /**
     * @brief Vuelve a @p m.  Lo reservado despues queda listo para reusarse.
     *
     * Marca y vuelta, NO un reinicio: asi dos fases anidadas no se pisan.
     *
     * @par Hilos
     * **NO segura**: solo el hilo dueño.  Y volver con una marca tomada en OTRO
     * hilo no es que sea inseguro, es que no significa nada.
     *
     * @code
     *   // A mano, cuando el ambito RAII no encaja (un bucle, por ejemplo).
     *   util::ScratchArena &a = util::scratch_arena();
     *   for (const auto &item : cosas) {
     *       const auto m = a.mark();
     *       procesar(item);          // reserva lo que le haga falta
     *       a.release(m);            // y se recicla en cada vuelta
     *   }
     * @endcode
     */
    void release(Mark m) noexcept;

    /**
     * @brief Bytes pedidos al sistema, no los que estan en uso.
     *
     * @par Hilos
     * **NO segura**: solo el hilo dueño.
     */
    size_t reserved_bytes() const noexcept { return reserved_; }

  private:
    Block *add_block(size_t least) noexcept;

    Block *head_ = nullptr;
    Block *current_ = nullptr;
    size_t reserved_ = 0;
};

/**
 * @brief La arena de ESTE hilo.  Se crea la primera vez que se pide.
 *
 * @par Hilos
 * **La funcion SI es segura** -- cada hilo recibe la suya --, pero lo que
 * devuelve NO lo es.  Es justo esa division la que permite que la arena no
 * sincronice nada: la seguridad esta en el reparto, no en la estructura.
 *
 * @code
 *   // Correcto: cada hilo pide la suya DENTRO de su tarea.
 *   pool.enqueue([&] {
 *       util::ScratchScope fase;
 *       trabajar();               // reserva de la arena de ESTE trabajador
 *   });
 *
 *   // MAL: tomarla fuera y usarla dentro son dos hilos sobre una arena.
 *   util::ScratchArena &a = util::scratch_arena();
 *   pool.enqueue([&a] { a.allocate(64, 8); });      // NO
 * @endcode
 */
ScratchArena &scratch_arena() noexcept;

/**
 * @brief Marca al entrar y vuelve al salir, pase lo que pase.
 *
 * Es la forma correcta de acotar una fase: si el trabajo sale por una
 * excepcion, la arena se recicla igual.
 *
 * @par Hilos
 * Cada objeto vale para SU hilo.  Se ata a la arena del hilo que lo construye,
 * asi que crearlo en un hilo y destruirlo en otro devolveria una marca ajena.
 * Por eso no es copiable.
 *
 * @code
 *   void optimizar(Funcion &f) {
 *       util::ScratchScope fase;      // marca aqui
 *       ...                           // reserva lo que quiera
 *   }                                 // y se recicla al salir, incluso si lanza
 * @endcode
 */
class ScratchScope {
  public:
    ScratchScope() noexcept : arena_(scratch_arena()), mark_(arena_.mark()) {}
    ~ScratchScope() { arena_.release(mark_); }
    ScratchScope(const ScratchScope &) = delete;
    ScratchScope &operator=(const ScratchScope &) = delete;

  private:
    ScratchArena &arena_;
    ScratchArena::Mark mark_;
};

/**
 * @brief Asignador estandar respaldado por la arena del hilo.
 *
 * Sirve para poner un contenedor dentro de la arena sin tocar el codigo que lo
 * usa.  `deallocate` NO hace nada a proposito: lo que se suelta es la fase
 * entera, no los objetos uno a uno.
 *
 * OJO con la consecuencia, que es la que desaconseja usarlo con contenedores
 * que crecen: sus buferes viejos se quedan hasta que acabe la fase.  Ver la
 * cabecera del fichero.
 *
 * @par Hilos
 * **No es seguro**, por lo mismo que la arena: reserva de la del hilo que
 * llame.  Y hay una trampa propia -- un contenedor construido con esto en un
 * hilo y usado en OTRO reservaria de dos arenas distintas, con lo que sus
 * bufers viejo y nuevo vivirian en fases que se reciclan por separado.  No da
 * error; da memoria pisada mucho despues.
 *
 * @code
 *   // Un vector de trabajo que muere con la fase, de tamano CONOCIDO.
 *   util::ScratchScope fase;
 *   std::vector<int, util::ScratchAlloc<int>> v;
 *   v.reserve(n);           // reservar de una vez: crecer aqui deja restos
 *   for (...) v.push_back(x);
 * @endcode
 */
template <typename T> class ScratchAlloc {
  public:
    using value_type = T;

    ScratchAlloc() noexcept = default;
    template <typename U> ScratchAlloc(const ScratchAlloc<U> &) noexcept {}

    T *allocate(size_t n) {
        void *p = scratch_arena().allocate(n * sizeof(T), alignof(T));
        if (p == nullptr) throw std::bad_alloc();
        return static_cast<T *>(p);
    }
    void deallocate(T *, size_t) noexcept {}

    template <typename U>
    bool operator==(const ScratchAlloc<U> &) const noexcept {
        return true; // todas comparten la arena del hilo
    }
    template <typename U>
    bool operator!=(const ScratchAlloc<U> &) const noexcept {
        return false;
    }
};

} // namespace util

#endif // VESTA_UTIL_SCRATCH_ARENA_H
