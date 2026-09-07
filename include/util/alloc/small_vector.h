/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 */

/**
 * @file util/alloc/small_vector.h
 * @brief
 * \~english A vector with its first N slots INSIDE the object itself.
 * \~spanish Vector con las primeras N posiciones DENTRO del propio objeto.
 * \~
 *
 * \~english
 * For what is almost always tiny but sometimes is not.  A `std::vector` asks
 * for memory the first time something is put into it, and when the container's
 * average life is four elements that allocation costs more than everything
 * done with them.
 *
 * Measured with VTune over a cold build of 15,000 lines: the range analysis
 * state -- one `std::vector` per block and per pass -- took 3.6% of the retired
 * instructions JUST growing and copying, with `util::host_alloc` next to it at
 * 3.5%.  And the average size of those states was 0.7 entries, with 25 as the
 * largest seen.
 *
 * It does not aim to be a complete `std::vector`: it has what its consumers
 * use.  Adding an operation is trivial; having one nobody uses is code that has
 * to be maintained without anybody testing it.
 *
 * \~spanish
 * Para lo que casi siempre es diminuto pero a veces no.  Un `std::vector` pide
 * memoria la primera vez que se le mete algo, y cuando la vida media del
 * contenedor son cuatro elementos esa reserva cuesta mas que todo lo que se
 * hace con ellos.
 *
 * Medido con VTune sobre una compilacion en frio de 15.000 lineas: el estado
 * del analisis de rangos -- un `std::vector` por bloque y por vuelta -- se
 * llevaba el 3,6 % de las instrucciones retiradas SOLO en crecer y copiar, con
 * `util::host_alloc` al lado en el 3,5 %.  Y el tamano medio de esos estados
 * era de 0,7 entradas, con 25 como maximo visto.
 *
 * No pretende ser un `std::vector` completo: tiene lo que usan sus
 * consumidores.  Anadir una operacion es trivial; tener una que nadie usa es
 * codigo que hay que mantener sin que nadie lo pruebe.
 *
 * \~
 */
#ifndef UTIL_SMALL_VECTOR_H
#define UTIL_SMALL_VECTOR_H

#include "util/alloc/host_allocator.h"

#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <type_traits>
#include <utility>
#include <vector> // para volcar un `std::vector` aqui donde ya se construia uno

namespace util {

/**
 * @brief
 * \~english A vector that only asks for memory when it outgrows its N inline
 *          slots.
 * \~spanish Vector que solo pide memoria cuando se le quedan cortas sus N
 *          posiciones de dentro.
 * \~
 *
 * \~english
 * Below N elements there is no allocation at all and the data travels with the
 * object, which is what makes it cheap; above N it behaves like any other
 * vector and grows on the heap, through `util::host_alloc`.
 *
 * @par Threads
 * **Not safe**, like any container: one instance, one thread at a time.  There
 * is nothing shared behind it, so two separate instances never interfere.
 *
 * \~spanish
 * Por debajo de N elementos no hay ninguna reserva y los datos viajan con el
 * objeto, que es lo que lo hace barato; por encima de N se comporta como
 * cualquier otro vector y crece en el monton, por `util::host_alloc`.
 *
 * @par Hilos
 * **No es seguro**, como cualquier contenedor: una instancia, un hilo cada vez.
 * Detras no hay nada compartido, asi que dos instancias distintas no se
 * estorban nunca.
 *
 * \~
 * @tparam T
 * \~english the element type.  It is assumed trivially copyable: the consumers
 *          here are, and assuming it allows moving with @c memcpy instead of
 *          element by element.
 * \~spanish el tipo de elemento.  Se asume trivialmente copiable: los
 *          consumidores de aqui lo son, y suponerlo permite mover con
 *          @c memcpy en vez de elemento a elemento.
 * \~
 * @tparam N
 * \~english how many fit without asking for memory.
 * \~spanish cuantos caben sin pedir memoria.
 * \~
 *
 * \~english
 * @code
 *   util::SmallVector<uint32_t, 4> operands;   // no allocation yet
 *   operands = {a, b};                         // still none
 *   for (uint32_t v : extra) operands.push_back(v);   // heap only past 4
 * @endcode
 *
 * \~spanish
 * @code
 *   util::SmallVector<uint32_t, 4> operandos;  // aun sin reservar nada
 *   operandos = {a, b};                        // sigue sin reservar
 *   for (uint32_t v : extra) operandos.push_back(v);  // monton solo pasado 4
 * @endcode
 *
 * \~
 */
template <typename T, size_t N> class SmallVector {
    static_assert(std::is_trivially_copyable<T>::value,
                  "SmallVector mueve con memcpy: el tipo debe ser trivialmente "
                  "copiable");

  public:
    /* Constructor ESCRITO, no `= default`.  El hueco de dentro no se
     * inicializa a proposito (ver abajo), y con un constructor implicito eso
     * convierte al tipo en no-inicializable como `const` sin inicializador --
     * que es justo como se declara un centinela vacio compartido. */
    /**
     * @brief
     * \~english An empty vector that has asked for nothing.
     * \~spanish Un vector vacio que no ha pedido nada.
     * \~
     */
    SmallVector() noexcept {}

    /**
     * @brief
     * \~english Builds it from a list, which is how operands get written.
     * \~spanish Lo construye desde una lista, que es como se escriben los
     *          operandos.
     * \~
     * @param il
     * \~english the elements, in order.
     * \~spanish los elementos, en orden.
     * \~
     */
    SmallVector(std::initializer_list<T> il) { assign(il.begin(), il.end()); }

    /**
     * @brief
     * \~english Gives back the heap buffer if there was one.
     * \~spanish Devuelve el bufer de fuera si lo habia.
     * \~
     */
    ~SmallVector() { release(); }

    /**
     * @brief
     * \~english Copies the contents of @p o.
     * \~spanish Copia el contenido de @p o.
     * \~
     * @param o
     * \~english the vector to copy.
     * \~spanish el vector a copiar.
     * \~
     */
    SmallVector(const SmallVector &o) { copy_from(o); }

    /**
     * @brief
     * \~english Assigns the contents of @p o, KEEPING the buffer if it already
     *          fits.
     * \~spanish Asigna el contenido de @p o, CONSERVANDO el bufer si ya cabe.
     * \~
     *
     * \~english
     * That reuse is the reason this exists: the consumer assigns one state over
     * another thousands of times, and releasing and asking again on each one is
     * exactly the cost this came to remove.
     *
     * \~spanish
     * Esa reutilizacion es la razon de ser de esto: el consumidor asigna un
     * estado sobre otro miles de veces, y soltar y volver a pedir en cada una
     * es justo el coste que se venia a quitar.
     *
     * \~
     * @param o
     * \~english the vector to copy.
     * \~spanish el vector a copiar.
     * \~
     * @return
     * \~english this vector, already holding what @p o held.
     * \~spanish este vector, ya con lo que tenia @p o.
     * \~
     */
    SmallVector &operator=(const SmallVector &o) {
        if (this != &o) {
            /* Se conserva el bufer si ya cabe.  Es la razon de ser de esto: el
             * consumidor asigna un estado sobre otro miles de veces, y soltar
             * y volver a pedir en cada una es justo el coste que se venia a
             * quitar. */
            if (o.size_ > cap_) {
                release();
                copy_from(o);
            } else {
                std::memcpy(data(), o.data(), o.size_ * sizeof(T));
                size_ = o.size_;
            }
        }
        return *this;
    }
    /**
     * @brief
     * \~english Takes over @p o, which is left empty.
     * \~spanish Se queda con lo de @p o, que queda vacio.
     * \~
     *
     * \~english
     * The heap buffer IS stolen; the inline one cannot be -- it lives in the
     * other object -- and has to be copied, which is at most N elements.
     *
     * \~spanish
     * El bufer de fuera SE ROBA; el de dentro no se puede robar -- vive en el
     * otro objeto -- y hay que copiarlo, que son como mucho N elementos.
     *
     * \~
     * @param o
     * \~english the vector to empty out.
     * \~spanish el vector a vaciar.
     * \~
     */
    SmallVector(SmallVector &&o) noexcept { move_from(o); }

    /**
     * @brief
     * \~english Releases what it had and takes over @p o.
     * \~spanish Suelta lo que tenia y se queda con lo de @p o.
     * \~
     * @param o
     * \~english the vector to empty out.
     * \~spanish el vector a vaciar.
     * \~
     * @return
     * \~english this vector, already holding what @p o held.
     * \~spanish este vector, ya con lo que tenia @p o.
     * \~
     */
    SmallVector &operator=(SmallVector &&o) noexcept {
        if (this != &o) {
            release();
            move_from(o);
        }
        return *this;
    }

    // ------------------------------------------------- consulta  --  query
    /**
     * @brief
     * \~english How many elements it holds.
     * \~spanish Cuantos elementos tiene.
     * \~
     * @return
     * \~english the count, which says nothing about where they live.
     * \~spanish la cuenta, que no dice nada de donde viven.
     * \~
     */
    size_t size() const noexcept { return size_; }

    /**
     * @brief
     * \~english Whether it holds nothing.
     * \~spanish Si no tiene nada.
     * \~
     * @return
     * \~english true when @c size is zero.
     * \~spanish true cuando @c size es cero.
     * \~
     */
    bool empty() const noexcept { return size_ == 0; }

    // Sin `reinterpret_cast`: `inline_` YA es un `T[N]`.  Ver la nota de abajo.
    /**
     * @brief
     * \~english The elements, in one contiguous run.
     * \~spanish Los elementos, seguidos en memoria.
     * \~
     * @return
     * \~english a pointer to the first one; it is INVALIDATED on growing, so it
     *           must not be kept across a @c push_back.
     * \~spanish un puntero al primero; se INVALIDA al crecer, asi que no se
     *           guarda de un @c push_back para otro.
     * \~
     */
    T *data() noexcept { return heap_ ? heap_ : inline_; }

    /**
     * @brief
     * \~english The elements, read only.
     * \~spanish Los elementos, solo para leer.
     * \~
     * @return
     * \~english a pointer to the first one, with the same warning as the
     *           non-const one.
     * \~spanish un puntero al primero, con el mismo aviso que el no constante.
     * \~
     */
    const T *data() const noexcept { return heap_ ? heap_ : inline_; }

    /**
     * @brief
     * \~english The element at @p i.  It is NOT range checked.
     * \~spanish El elemento en @p i.  NO se comprueba el rango.
     * \~
     * @param i
     * \~english the index, which the caller guarantees to be below @c size.
     * \~spanish el indice, que quien llama garantiza menor que @c size.
     * \~
     * @return
     * \~english a reference to it.
     * \~spanish una referencia a el.
     * \~
     */
    T &operator[](size_t i) noexcept { return data()[i]; }

    /**
     * @brief
     * \~english The element at @p i, read only.
     * \~spanish El elemento en @p i, solo para leer.
     * \~
     * @param i
     * \~english the index, which the caller guarantees to be below @c size.
     * \~spanish el indice, que quien llama garantiza menor que @c size.
     * \~
     * @return
     * \~english a constant reference to it.
     * \~spanish una referencia constante a el.
     * \~
     */
    const T &operator[](size_t i) const noexcept { return data()[i]; }

    /**
     * @brief
     * \~english The start, so that a range-based `for` works.
     * \~spanish El principio, para que funcione un `for` por rango.
     * \~
     * @return
     * \~english a pointer to the first element.
     * \~spanish un puntero al primer elemento.
     * \~
     */
    T *begin() noexcept { return data(); }

    /**
     * @brief
     * \~english One past the last one.
     * \~spanish Uno mas alla del ultimo.
     * \~
     * @return
     * \~english the end pointer, which is never dereferenced.
     * \~spanish el puntero de fin, que no se desreferencia nunca.
     * \~
     */
    T *end() noexcept { return data() + size_; }

    /**
     * @brief
     * \~english The start, read only.
     * \~spanish El principio, solo para leer.
     * \~
     * @return
     * \~english a constant pointer to the first element.
     * \~spanish un puntero constante al primer elemento.
     * \~
     */
    const T *begin() const noexcept { return data(); }

    /**
     * @brief
     * \~english One past the last one, read only.
     * \~spanish Uno mas alla del ultimo, solo para leer.
     * \~
     * @return
     * \~english the constant end pointer.
     * \~spanish el puntero de fin constante.
     * \~
     */
    const T *end() const noexcept { return data() + size_; }

    // ------------------------------------------ modificacion  --  modifying
    /**
     * @brief
     * \~english Empties it, KEEPING the buffer on purpose.
     * \~spanish Lo vacia, CONSERVANDO el bufer a proposito.
     * \~
     *
     * \~english Keeping it is what makes reuse free on the next round.
     * \~spanish Conservarlo es lo que hace gratis el reuso en la vuelta
     *          siguiente.  \~
     */
    void clear() noexcept { size_ = 0; }

    /**
     * @brief
     * \~english Adds @p v at the end, growing if it no longer fits.
     * \~spanish Anade @p v al final, creciendo si ya no cabe.
     * \~
     * @param v
     * \~english the element to copy in.
     * \~spanish el elemento a copiar dentro.
     * \~
     *
     * \~english
     * @warning It INVALIDATES any pointer taken with @c data or @c begin.
     * \~spanish
     * @warning INVALIDA cualquier puntero tomado con @c data o @c begin.
     * \~
     */
    void push_back(const T &v) {
        if (size_ == cap_) grow(size_ + 1);
        data()[size_++] = v;
    }

    /**
     * @brief
     * \~english Makes room for @p n elements without adding any.
     * \~spanish Hace sitio para @p n elementos sin anadir ninguno.
     * \~
     * @param n
     * \~english how many are going to fit; below N it does nothing.
     * \~spanish cuantos van a caber; por debajo de N no hace nada.
     * \~
     */
    void reserve(size_t n) {
        if (n > cap_) grow(n);
    }

    /**
     * @brief
     * \~english Leaves it with exactly @p n elements.
     * \~spanish Lo deja con exactamente @p n elementos.
     * \~
     *
     * \~english
     * It only GROWS with filler or shrinks; it destroys nothing, since T is
     * trivial.
     *
     * \~spanish
     * Solo CRECE con relleno o encoge; no destruye nada, ya que T es trivial.
     *
     * \~
     * @param n
     * \~english the size wanted.
     * \~spanish el tamano que se quiere.
     * \~
     * @param fill
     * \~english what to put in the new slots when it grows.
     * \~spanish que poner en las posiciones nuevas cuando crece.
     * \~
     */
    void resize(size_t n, const T &fill = T{}) {
        if (n > cap_) grow(n);
        for (size_t i = size_; i < n; ++i) data()[i] = fill;
        size_ = n;
    }

    /**
     * @brief
     * \~english Assignment from a list.
     * \~spanish Asignacion desde lista.
     * \~
     *
     * \~english
     * It is how the operands of an instruction are written (`operands = {a,
     * b}`) in hundreds of places.
     *
     * \~spanish
     * Es como se escriben los operandos de una instruccion (`operands = {a,
     * b}`) en cientos de sitios.
     *
     * \~
     * @param il
     * \~english the new contents, in order.
     * \~spanish el nuevo contenido, en orden.
     * \~
     * @return
     * \~english this vector, already holding the list.
     * \~spanish este vector, ya con la lista.
     * \~
     */
    SmallVector &operator=(std::initializer_list<T> il) {
        assign(il.begin(), il.end());
        return *this;
    }

    /**
     * @brief
     * \~english Assignment from anything that can be walked: a view, another
     *          container.
     * \~spanish Asignacion desde cualquier cosa que se pueda recorrer: una
     *          vista, otro contenedor.
     * \~
     *
     * \~english
     * It excludes `SmallVector` so as not to shadow the copy above.
     *
     * \~spanish
     * Excluye a `SmallVector` para no pisar la copia de arriba.
     *
     * \~
     * @tparam C
     * \~english the source type; anything with @c begin and @c end.
     * \~spanish el tipo de origen; cualquier cosa con @c begin y @c end.
     * \~
     * @param c
     * \~english what to copy from.
     * \~spanish de donde copiar.
     * \~
     * @return
     * \~english this vector, already holding a copy of @p c.
     * \~spanish este vector, ya con una copia de @p c.
     * \~
     *
     * \~english
     * @code
     *   util::SmallVector<uint32_t, 4> v;
     *   v = a_std_vector;         // dumped into the inline slots if it fits
     * @endcode
     *
     * \~spanish
     * @code
     *   util::SmallVector<uint32_t, 4> v;
     *   v = un_std_vector;        // volcado a las posiciones de dentro si cabe
     * @endcode
     *
     * \~
     */
    template <typename C,
              typename = typename std::enable_if<
                  !std::is_same<typename std::decay<C>::type,
                                SmallVector>::value>::type,
              typename = decltype(std::declval<const C &>().begin())>
    SmallVector &operator=(const C &c) {
        assign(c.begin(), c.end());
        return *this;
    }

    /**
     * @brief
     * \~english Replaces the contents with [@p first, @p last).
     * \~spanish Sustituye el contenido por [@p first, @p last).
     * \~
     * @tparam It
     * \~english the iterator type of the source range.
     * \~spanish el tipo de iterador del rango de origen.
     * \~
     * @param first
     * \~english where the range starts.
     * \~spanish donde empieza el rango.
     * \~
     * @param last
     * \~english one past where it ends.
     * \~spanish uno mas alla de donde acaba.
     * \~
     */
    template <typename It> void assign(It first, It last) {
        clear();
        for (It it = first; it != last; ++it) push_back(*it);
    }

    /**
     * @brief
     * \~english Removes the element at @p pos.  Shifts whatever is behind it.
     * \~spanish Quita el elemento en @p pos.  Desplaza lo que haya detras.
     * \~
     * @param pos
     * \~english the index; out of range it does nothing.
     * \~spanish el indice; fuera de rango no hace nada.
     * \~
     */
    void erase_at(size_t pos) {
        if (pos >= size_) return;
        T *p = data();
        if (pos + 1 < size_)
            std::memmove(p + pos, p + pos + 1, (size_ - pos - 1) * sizeof(T));
        --size_;
    }

    /**
     * @brief
     * \~english Inserts @p v at @p pos, shifting whatever is behind it.
     * \~spanish Inserta @p v en @p pos, desplazando lo que haya detras.
     * \~
     *
     * \~english
     * An index, not an iterator: here pointers are invalidated on growing and
     * an index survives.  The shift, at these sizes, is a @c memmove of
     * nothing.
     *
     * \~spanish
     * Un indice, no un iterador: aqui los punteros se invalidan al crecer y un
     * indice sobrevive.  El desplazamiento, con estos tamanos, es un
     * @c memmove de nada.
     *
     * \~
     * @param pos
     * \~english where to put it; at @c size it is the same as @c push_back.
     * \~spanish donde ponerlo; en @c size equivale a @c push_back.
     * \~
     * @param v
     * \~english the element to copy in.
     * \~spanish el elemento a copiar dentro.
     * \~
     */
    void insert_at(size_t pos, const T &v) {
        if (size_ == cap_) grow(size_ + 1);
        T *p = data();
        if (pos < size_)
            std::memmove(p + pos + 1, p + pos, (size_ - pos) * sizeof(T));
        p[pos] = v;
        ++size_;
    }

    /**
     * @brief
     * \~english Swaps contents.
     * \~spanish Intercambia contenidos.
     * \~
     *
     * \~english
     * With inline storage it can NOT be a pointer swap: what lives inside the
     * object cannot be stolen.  When both are on the heap the pointers ARE
     * swapped, which is the case that matters (the big ones); otherwise it
     * copies, and that is at most N elements.
     *
     * \~spanish
     * Con almacenamiento en linea NO puede ser un intercambio de punteros: lo
     * que vive dentro del objeto no se puede robar.  Cuando los dos estan
     * fuera si se cambian los punteros, que es el caso que importa (los
     * grandes); si no, se copia, y son como mucho N elementos.
     *
     * \~
     * @param o
     * \~english the other vector.
     * \~spanish el otro vector.
     * \~
     */
    void swap(SmallVector &o) noexcept {
        if (heap_ != nullptr && o.heap_ != nullptr) {
            T *h = heap_;
            heap_ = o.heap_;
            o.heap_ = h;
            size_t c = cap_;
            cap_ = o.cap_;
            o.cap_ = c;
            size_t s = size_;
            size_ = o.size_;
            o.size_ = s;
            return;
        }
        SmallVector tmp(std::move(*this));
        *this = std::move(o);
        o = std::move(tmp);
    }

    /**
     * @brief
     * \~english Whether both hold the same thing.
     * \~spanish Si los dos tienen lo mismo.
     * \~
     *
     * \~english
     * It compares the bytes, which is legitimate because T is trivially
     * copyable, and it never looks at where they are stored.
     *
     * \~spanish
     * Compara los bytes, que vale porque T es trivialmente copiable, y no mira
     * nunca donde estan guardados.
     *
     * \~
     * @param o
     * \~english the other vector.
     * \~spanish el otro vector.
     * \~
     * @return
     * \~english true when the sizes match and so does every element.
     * \~spanish true cuando coinciden los tamanos y todos los elementos.
     * \~
     */
    bool operator==(const SmallVector &o) const noexcept {
        if (size_ != o.size_) return false;
        return size_ == 0 ||
               std::memcmp(data(), o.data(), size_ * sizeof(T)) == 0;
    }

    /**
     * @brief
     * \~english The negation of @c operator==.
     * \~spanish La negacion de @c operator==.
     * \~
     * @param o
     * \~english the other vector.
     * \~spanish el otro vector.
     * \~
     * @return
     * \~english true when they differ in size or in any element.
     * \~spanish true cuando difieren en tamano o en algun elemento.
     * \~
     */
    bool operator!=(const SmallVector &o) const noexcept {
        return !(*this == o);
    }

  private:
    void release() noexcept {
        if (heap_ != nullptr) {
            util::host_free(heap_);
            heap_ = nullptr;
        }
        cap_ = N;
        size_ = 0;
    }

    void copy_from(const SmallVector &o) {
        if (o.size_ > N) {
            heap_ = static_cast<T *>(util::host_alloc(o.size_ * sizeof(T)));
            cap_ = o.size_;
        }
        std::memcpy(data(), o.data(), o.size_ * sizeof(T));
        size_ = o.size_;
    }

    void move_from(SmallVector &o) noexcept {
        if (o.heap_ != nullptr) {
            /* El bufer de fuera SE ROBA; el de dentro no se puede robar --
             * vive en el otro objeto -- y hay que copiarlo. */
            heap_ = o.heap_;
            cap_ = o.cap_;
            o.heap_ = nullptr;
            o.cap_ = N;
        } else {
            std::memcpy(inline_, o.inline_, o.size_ * sizeof(T));
        }
        size_ = o.size_;
        o.size_ = 0;
    }

    void grow(size_t least) {
        size_t next = cap_ * 2;
        if (next < least) next = least;
        T *fresh = static_cast<T *>(util::host_alloc(next * sizeof(T)));
        std::memcpy(fresh, data(), size_ * sizeof(T));
        if (heap_ != nullptr) util::host_free(heap_);
        heap_ = fresh;
        cap_ = next;
    }

    /* UNION, y no un array de `unsigned char` con `reinterpret_cast`.
     *
     * Las dos formas dejan el hueco SIN INICIALIZAR -- que es el punto: `T` es
     * trivial y solo se leen las `size_` primeras, asi que ponerlas a cero
     * costaria N escrituras por cada estado que se crea --, pero solo esta
     * declara de verdad un `T[N]`.  Con los bytes crudos no existe ningun
     * objeto de tipo `T` ahi, y leerlos o escribirlos por un `T*` es
     * comportamiento INDEFINIDO: con `-fstrict-aliasing` el compilador da por
     * hecho que un acceso por `T*` y otro por `unsigned char*` no se pisan, y
     * puede quedarse con un puntero de fin rancio en un registro.
     *
     * No es teoria.  Costo que el JIT reventara al recorrer las instrucciones
     * de un bloque -- `movzwl (%rdi)` sobre memoria sin mapear --, y solo en
     * Release: la bandera esta en los dos perfiles, pero `-fomit-frame-pointer`
     * cambia la presion de registros lo justo para que se note en uno y no en
     * el otro.  Es como se comporta lo indefinido: esta siempre, se ve a
     * veces.
     *
     * La union no construye nada -- ningun miembro es el activo al empezar --
     * asi que no se paga ninguna inicializacion. */
    union {
        T inline_[N];
    };
    /// \~english nullptr = the inline one is being used.
    /// \~spanish nullptr = se esta usando el de dentro.  \~
    T *heap_ = nullptr;
    size_t size_ = 0;
    size_t cap_ = N;
};

} // namespace util

#endif // UTIL_SMALL_VECTOR_H
