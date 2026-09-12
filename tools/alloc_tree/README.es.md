# alloc_tree

*[English version](README.md)*

De donde nacen las reservas, como un arbol que se puede recorrer.

El asignador sabe imprimir un informe, y ese informe contesta **que reserva
mas**. Lo que no puede contestar es la pregunta siguiente, que es la que lleva
a algun sitio: de *quien* es el codigo que lo pidio, por que camino, y como
queda el reparto al plegar una rama. Eso es recorrer y entrar — trabajo de una
herramienta, no de un volcado.

## Sacar los datos

El volcado lo escribe el propio programa que se esta mirando. Dos variables de
entorno:

```sh
VESTA_HOST_ALLOC_SITES=1 VESTA_HOST_ALLOC_CSV=/tmp/corrida  ./tu_programa
```

`..._SITES` enciende el apuntado — sin ella no se anota nada, y ni siquiera se
instala el salto sobre `operator new` que apunta de donde viene cada reserva,
asi que quien no lo pide no paga nada. `..._CSV` dice donde dejar los ficheros;
la carpeta **se crea**, con todos los niveles que le falten.

Salen seis, que se juntan por `site_id`:

| | |
| :--- | :--- |
| `sites.csv` | una fila por sitio: cuentas, bytes, proposito, forma, direccion, que clases de tamano |
| `frames.csv` | una fila por (sitio, profundidad): la cadena de inline, y de que modulo es cada marco |
| `summary.csv` | clave/valor: totales, region, y lo que **no** se pudo resolver |
| `sizes.csv` | el reparto de tamanos pedidos, de todo el proceso |
| `site_sizes.csv` | el mismo reparto **por sitio**, disperso |
| `tags.csv` | cuanto se lleva cada proposito declarado |

`site_sizes.csv` es lo que permite que una *rama* del arbol conteste "muchas
pequenas o unas pocas grandes". El reparto global no puede: no se puede
repartir hacia atras entre los sitios que lo formaron, y un sitio que hizo un
millon de reservas de 32 bytes se ve igual que uno que hizo una sola de 16 MiB
— misma media, misma mascara de clases.

Varios ficheros y no uno porque un sitio tiene muchos marcos: en una sola tabla
habria que repetir cada sitio tantas veces como marcos tenga.

## Mirarlo

```sh
python -m alloc_tree /tmp/corrida
```

Escribe una pagina que se basta sola y la abre.

La pagina es una **rejilla de arbol**, no un grafico: una pila de llamadas
sangrada que se pliega y se despliega, con los numeros en columnas al lado —
reservas que pasan por el marco y reservas que acaban *en* el, lo mismo en
bytes, cuantos sitios cubre, de que modulo es, el fichero y el desplazamiento.
Los nombres salen ENTEROS: un simbolo de C++ es largo y la cola es donde estan
los argumentos de plantilla, que es justo lo que distingue dos instanciaciones.

Al pulsar una columna, los hermanos se ordenan por ella; al escribir en la
caja, quedan solo las ramas que contienen lo buscado; *weigh by bytes* pesa por
tamano en vez de por cuenta. Un boton cambia entre las dos direcciones:

- **de fuera hacia dentro** — desde la funcion que existe en el binario hacia
  adentro, atravesando todo lo que el optimizador le metio. *De donde viene
  esta reserva.*
- **de dentro hacia fuera** — desde donde la reserva ocurre fisicamente hacia
  afuera. *Quien acaba llamando mas a `operator new`* — es la que junta todo el
  crecimiento de los `std::vector`.

Al pulsar una fila se abre en las filas crudas de las que salio, asi que
cualquier numero del arbol se puede COMPROBAR en vez de creer.

Y debajo del arbol, cada uno en su pestana, estan **los cinco ficheros
enteros**: todas las filas, todas las columnas, incluidas las que esta
herramienta no sabe interpretar. Nada se resume: una herramienta que ensena
parte de una medicion hace imposible buscar el resto, y quien la lee no puede
saber que parte falta.

La pagina lleva sus datos dentro y no carga nada, asi que sigue funcionando sin
red, copiada de una maquina a la que solo se llega por ssh, y dentro de cinco
anos.

### En una maquina donde no se puede instalar nada

```sh
python -m alloc_tree /tmp/corrida --text
```

El mismo arbol **y las mismas cinco tablas** en la terminal, solo con la
libreria estandar. No es un modo degradado que se conserva por cortesia: la
maquina donde aparece un problema de memoria no suele ser la que tiene
navegador, y un modo de terminal que ensenara menos haria imposible ver la
diferencia entre los dos.

### Opciones

| | |
| :--- | :--- |
| `--text`, `-t` | el volcado entero en la terminal, sin dependencias |
| `--tree-only` | en `--text`, el arbol sin las cinco tablas de debajo |
| `--bottom-up` | empezar por el otro extremo |
| `--limit PCT` | en `--text`, plegar las ramas por debajo de ese %, diciendo cuantas. Por defecto 0: sale todo |
| `--names BIN` | poner nombres a los desplazamientos de una construccion despojada (ver abajo) |
| `--out FILE` | donde escribir la pagina |
| `--no-open` | escribirla y no abrir el navegador |

## Preguntarle a las tablas

El arbol contesta "quien reserva". La otra mitad de las preguntas que se le
hacen a una medicion no es un dibujo sino una consulta — los cortes donde la
holgura fue peor, los sitios de un modulo ordenados por bytes, en que tabla
aparece un simbolo — y eso se acababa contestando con un `python -c` distinto
cada vez.

```sh
# que tablas hay, con sus columnas
python -m alloc_tree /tmp/corrida --tables

# los cortes con mas memoria guardada y sin entregar
python -m alloc_tree /tmp/corrida --table epochs \
    --cols 'epoch,mark,mib(live_bytes) as vivo,mib(region_bytes-live_bytes) as holgura' \
    --sort=-holgura

# los sitios de un modulo, por bytes, con el nombre de quien los pidio
python -m alloc_tree /tmp/corrida --table sites --where 'module=="ir"' \
    --cols 'func,where,allocs,mib(bytes) as mib' --sort=-mib

# los bytes por modulo
python -m alloc_tree /tmp/corrida --table sites --group-by module \
    --cols 'allocs,mib(bytes) as mib' --sort=-mib

# donde aparece esto, en todas las tablas a la vez
python -m alloc_tree /tmp/corrida --grep VelNodeStream
```

| | |
| :--- | :--- |
| `--tables` | que tablas hay, sus columnas y las que se **pegan** al cruzar los marcos |
| `--table N` | consultar esa tabla; vale la abreviatura (`epochs` por `check_epochs.csv`) |
| `--where E` | quedarse con las filas que cumplan la expresion |
| `--cols E` | columnas de salida, con `expresion as nombre` |
| `--group-by E` | agrupar por eso y **sumar** las columnas numericas |
| `--sort=-COL` | ordenar por una columna de SALIDA; el `-` la invierte, y va pegado con `=` |
| `--rows N` | cuantas filas ensenar (0 = todas) |
| `--csv` | sacarlo como CSV, para encadenarlo |

Dentro de una expresion hay `kib()`, `mib()`, `gib()`, `pct(parte, total)`,
`like(texto, patron)` y `has(texto, trozo)`, ademas de los operadores de
siempre.

**Las columnas pegadas** son la razon de que esto sea util: a una fila con un
sitio se le anaden `func`, `inner`, `outer`, `file`, `where`, `module` y
`chain`. Sin ellas, una consulta devuelve un `site_id` y hay que ir a buscarlo a
otro fichero.

`func` es **el primer marco que escribio el autor**, saliendo de la libreria
hacia fuera — la misma regla que usa la pagina y que usa el arbol al plegar: una
reserva hecha a traves de `std::vector` ES nuestra, lo que no es nuestro es el
marco de dentro. `inner` es el de mas adentro (contesta `std::string`, cierto e
inutil) y `outer` el de mas afuera.

**`outer` no sirve para atribuir, y se midio**: con el, subir la profundidad del
recorrido de pila de 4 a 16 movia el pico de `vx::Lowering::emit` a
`RtlUserThreadStart`. Cuanto mas lejos se camina, mas se parece el marco de
fuera al arranque del hilo — que es el mismo para todo el programa y por tanto
no distingue nada. Con MAS datos, peor respuesta.

Y se cruzan contra **la poblacion que toca**: las tablas del asignador contra
`frames.csv` y las del comprobador contra `check_frames.csv`. Son dos
poblaciones distintas — una direccion de retorno contra una pila recorrida — y
mezclarlas contestaria otra pregunta.

### Las tres trampas, resueltas una vez

Son las que un guion escrito a mano se come en silencio:

- **Una celda vacia es NULO, no cero.** Las tablas lo usan a proposito: en
  `check_sizes.csv` lo vivo va vacio cuando la corrida no pudo saberlo, y un
  cero ahi se leeria como "este tamano no se queda nada". Un nulo no suma, no
  ordena y no casa — y se **cuenta**, para que el pie diga cuantas filas no se
  pudieron juzgar en vez de dejarlas fuera sin avisar.
- **Los numeros se comparan como numeros.** Ordenando por texto, `640` va por
  delante de `40000`.
- **Los nulos se ordenan al final**, se pida el orden que se pida: encabezar
  una tabla de mayor a menor con las filas de las que no se sabe nada es lo
  contrario de lo que se pidio.

De aqui **no sale ningun numero que no salga de la exportacion**: una columna
calculada es una cuenta sobre lo que hay.

## Una construccion sin simbolos

Una construccion despojada — `Release` — no tiene tabla de simbolos ni
informacion de depuracion, asi que el volcado sale con desplazamientos en vez
de nombres. Sigue sirviendo: `.pdata` es una *seccion*, no una tabla de
simbolos, y `--strip-all` no se la lleva, asi que cada sitio dice todavia que
funcion lo contiene. El arbol los agrupa por ahi — medido sobre este
compilador, 480 sitios en 255 funciones.

Para ponerles nombre, se apunta al binario sin despojar **de esa misma
construccion**:

```sh
python -m alloc_tree /tmp/corrida --names ./build/vm
```

Lo de la misma construccion importa. Dos construcciones del mismo fuente
colocan el codigo de otra forma, asi que un desplazamiento de una resuelto
contra la otra da un nombre equivocado que parece correcto. Desde aqui no hay
manera de comprobarlo.

## Leerlo sin enganarse

Lo que **no** se pudo hacer sale primero, y tambien en la pagina:

- no habia resolutor de simbolos — el arbol lleva direcciones, no nombres;
- sitios que no se pudieron resolver;
- la foto se lleno, asi que puede haber mas sitios;
- reservas que desalojaron a una entrada mas floja, lo que convierte esas
  cuentas en cotas inferiores;
- alguna cadena llego al tope de marcos y puede estar cortada.

Nada de eso es decoracion. Un arbol sin nombres parece un programa que reserva
desde ninguna parte, y uno cortado parece un programa con menos sitios — ni una
cosa ni la otra son ciertas, y ninguna se ve si no se dice.

## Que hay en cada fichero

| | |
| :--- | :--- |
| `report.py` | los cinco ficheros, cargados y juntados — se guardan TODAS las columnas, no solo las que se usan |
| `tree.py` | las cadenas de todos los sitios fundidas en un arbol |
| `text.py` | el volcado entero escrito para una terminal |
| `symbols.py` | nombres a posteriori, con `addr2line` |
| `page.py` | el volcado como pagina |
| `templates/` | `page.html.j2`, `page.css`, `page.js` — el marcado, el estilo y la rejilla |
| `__main__.py` | la linea de ordenes |

Solo `page.py` necesita algo instalado (`../requirements.txt`: Jinja2, y nada
mas).

## Los nombres, y quien los decide

El asignador no sabe desmanglar, y no debe: el manglado es del lenguaje y de su
compilador, y esta libreria la enlazan proyectos que no se ponen de acuerdo en
eso — C++ mangla de una forma, Rust de otra, un lenguaje que trae su propio
compilador mangla como le parece, y C no mangla. Quien la enlaza instala un
formateador de nombres y decide que significa *legible* para su propio codigo.
Por eso los nombres del CSV ya son los que ese proyecto escribe, y esta
herramienta no necesita desmanglador propio.

Lo mismo vale para la columna **Module**. Que cuenta como modulo es una
propiedad del arbol de cada proyecto — aqui un directorio bajo `src/`, en otro
sitio un paquete, un crate o un bundle —, asi que el volcado pregunta y el
proyecto contesta.

Los dos ganchos, y el propio volcado, se alcanzan **desde C igual que desde
C++** (`util/alloc_csv_c.h`): un programa que no toca `operator new` en su vida
reserva igualmente a traves de esta libreria, y tiene que poder pedir su
informe. `examples/c_report.c` es todo esto en C, y se compila COMO C
precisamente para que esa cabecera no pueda dejar de serlo sin que nadie se
entere.
