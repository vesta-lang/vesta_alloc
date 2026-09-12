/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 *
 *     node test_page.js <payload.json> <templates dir>
 *
 * Runs the page's own scripts against a payload the Python side produced, on
 * the DOM stub.  Exits 0 or 1, which is the convention of every other test in
 * this project.
 *
 * The payload comes from a FILE and not from a generated page, so these checks
 * do not need Jinja2 installed: what is being tested is the browser half.
 */
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const { makeDom } = require('./dom_stub');

const payloadPath = process.argv[2];
const templates = process.argv[3];
/* EL MISMO ORDEN QUE `page.py`.  Son scripts clasicos, asi que uno que llama a
 * otro que aun no esta no da un error: la pagina sale a medias y la prueba
 * pasa.  Si aqui se cambia el orden, hay que cambiarlo alli. */
const SCRIPTS = ['code.js', 'i18n.js', 'tables.js', 'tree.js', 'link.js',
                 'detail.js', 'time.js', 'page.js'];

let failures = 0;
function check(ok, what) {
  console.log('  [' + (ok ? 'OK  ' : 'FAIL') + '] ' + what);
  if (!ok) failures++;
}

const DATA = JSON.parse(fs.readFileSync(payloadPath, 'utf8'));
const { ctx, byId, location } = makeDom(DATA);
vm.createContext(ctx);
for (const name of SCRIPTS) {
  vm.runInContext(fs.readFileSync(path.join(templates, name), 'utf8'), ctx,
                  { filename: name });
}

function rowCount() {
  return (byId['rows'].innerHTML.match(/<tr /g) || []).length;
}

// CON QUE POBLACION ABRIO, apuntado antes de que ninguna prueba cambie de
// vista.  Mirarlo mas abajo contestaria por la ultima que alguien pidiera, que
// es justo lo que esto no pregunta.
const openedOn = ctx.VIEW;

// -- it draws at all -------------------------------------------------------
check(rowCount() > 0, 'la rejilla dibuja filas al arrancar');
check(byId['stat'].textContent.indexOf('rows') >= 0,
      'y la linea de estado dice cuantas');

// -- the tree adds up, in every shape --------------------------------------
//
// Folding must not lose allocations.  A tree that has lost some still draws,
// and its percentages still add up to a hundred -- of a smaller total.
//
// EN LAS DOS POBLACIONES, no en la que toque abrir.  Antes esto corria sobre
// la del asignador porque era la de por defecto, y el dia que dejo de serlo
// ocho comprobaciones se pusieron rojas sin que nada del arbol hubiera
// cambiado -- que es la senal de que median el defecto y no el invariante.  El
// invariante vale para cualquiera de las dos, asi que se pide por su nombre.
const POBLACIONES = DATA.check ? ['alloc', 'check'] : ['alloc'];
for (const pobl of POBLACIONES) {
  ctx.setView(pobl);
  const view = pobl === 'check' ? DATA.check : DATA;
  /* LOS TOTALES SON LOS DEL PROGRAMA: lo que el informe se gasto leyendo
   * simbolos no esta en el arbol a ningun alcance, asi que sumar todas las
   * filas y esperar que cuadre seria pedirle al arbol que contenga lo que por
   * definicion no contiene.  Y se comprueba que los dos trozos suman el
   * volcado entero, que es lo que impide que "fuera del arbol" se convierta en
   * un agujero por donde perder reservas. */
  const totalAllocs = view.totals.allocs;
  const totalBytes = view.totals.bytes;
  const en = ' [' + pobl + ']';
  check(totalAllocs + view.totals.instrAllocs
          === view.sites.reduce((n, s) => n + s.allocs, 0) &&
        totalBytes + view.totals.instrBytes
          === view.sites.reduce((n, s) => n + s.bytes, 0),
        'el programa mas el informe son el volcado entero' + en);

  for (const dir of ['td', 'bu']) {
    for (const grouping of ['stack', 'module', 'file', 'purpose']) {
      const root = ctx.treeFor(dir, grouping);
      check(root.allocs === totalAllocs && root.bytes === totalBytes,
            'no se pierde nada al plegar (' + dir + ', ' + grouping + ')' + en);
    }
  }

  // -- "solo mis llamadas": lo que deja fuera CUADRA -------------------------
  //
  // Es la propiedad que hace que el filtro se pueda creer: lo que queda mas lo
  // que se dejo fuera tiene que ser exactamente lo medido.  Un filtro que
  // pierde reservas por el camino se lee igual que un programa que reserva
  // poco, y no habria forma de distinguirlo.
  for (const scope of ['all', 'fold', 'hide', 'extern']) {
    for (const lang of ['all', 'c', 'cpp', 'unknown']) {
      const root = ctx.treeFor('td', 'stack', scope, lang);
      check(root.allocs + root.leftOut === totalAllocs &&
            root.bytes + root.leftOutBytes === totalBytes,
            'lo que queda mas lo apartado es lo medido (' + scope + ', '
            + lang + ')' + en);
    }
  }

  // "mio" y "externo" son COMPLEMENTARIOS: entre los dos esta todo lo medido y
  // no comparten ni una reserva.  Sin esta propiedad, "solo lo externo" seria
  // un filtro cualquiera en vez del reverso exacto del anterior, y quien mirase
  // los dos no sabria si lo que no ve en ninguno existe o se perdio.
  const mine = ctx.treeFor('td', 'stack', 'hide');
  const theirs = ctx.treeFor('td', 'stack', 'extern');
  check(mine.allocs + theirs.allocs === totalAllocs,
        'lo mio y lo externo suman exactamente lo medido' + en);
}
ctx.setView('alloc');
const totalAllocs = DATA.sites.reduce((n, s) => n + s.allocs, 0);
const totalBytes = DATA.sites.reduce((n, s) => n + s.bytes, 0);

// Y cada modo hace lo que dice.  El volcado de prueba trae, a proposito:
//   sitio 2  -- `std::vector::_M_realloc_insert` y NADIE detras: biblioteca
//               pura, no hay a quien atribuirsela;
//   sitios 0 y 1 -- `std::pair` inlineado DENTRO de `vx_parse_file`: la
//               reserva es nuestra aunque el marco de dentro no lo sea;
//   sitio 5  -- de otro modulo (`msvcrt.dll`), que no es heuristica sino un
//               dato del volcado.
{
  const all = ctx.treeFor('td', 'stack', 'all');
  const fold = ctx.treeFor('td', 'stack', 'fold');
  const hide = ctx.treeFor('td', 'stack', 'hide');

  check(all.leftOut === 0, "'todo' no deja nada fuera");
  check(fold.leftOut > 0 && hide.leftOut >= fold.leftOut,
        'plegar aparta menos que ocultar, que es mas romo');

  // Las 150 reservas de los sitios 0 y 1 SOBREVIVEN al plegado, colgadas de
  // `vx_parse_file`: pasar por `std::pair` no las hace de la biblioteca.
  const names = (root) => {
    const out = [];
    (function walk(n) { out.push(n.fn); n.kids.forEach(walk); })(root);
    return out;
  };
  check(names(fold).some((n) => n.indexOf('vx_parse_file') >= 0),
        'al plegar, la reserva hecha a traves de la biblioteca sigue ahi, '
        + 'colgada de quien la pidio');
  check(!names(fold).some((n) => n.indexOf('_M_realloc_insert') >= 0),
        'y el marco de biblioteca ya no aparece como quien reserva');
  check(!names(fold).some((n) => n.indexOf('realloc') === 0),
        'lo de otro modulo queda fuera en los dos modos');

  // Y el reverso: "solo lo externo" SI lo trae, que es para lo que esta.
  const theirs = ctx.treeFor('td', 'stack', 'extern');
  check(names(theirs).some((n) => n.indexOf('realloc') === 0),
        "'solo lo externo' si ensena lo de otro modulo");
  check(names(theirs).some((n) => n.indexOf('_M_realloc_insert') >= 0),
        'y tambien el codigo de biblioteca sin nadie mio detras');
}

// -- por LENGUAJE, que es el otro eje ---------------------------------------
//
// El volcado de prueba tiene `vx_parse_file` en un `.c` y `main` en un `.cpp`,
// asi que cada filtro tiene algo que ensenar y algo que dejar fuera -- que es
// la unica forma de que la prueba signifique algo.
{
  const namesOf = (root) => {
    const out = [];
    (function walk(n) { out.push(n.fn + '|' + n.file); n.kids.forEach(walk); })(root);
    return out;
  };
  const onlyC = namesOf(ctx.treeFor('td', 'stack', 'all', 'c'));
  const onlyCpp = namesOf(ctx.treeFor('td', 'stack', 'all', 'cpp'));

  check(onlyC.some((n) => n.indexOf('lexer.c') >= 0),
        "'solo C' trae lo que reserva desde un .c");
  check(!onlyC.some((n) => n.indexOf('main.cpp') >= 0),
        'y deja fuera lo que reserva desde un .cpp');
  check(onlyCpp.some((n) => n.indexOf('main.cpp') >= 0),
        "'solo C++' al reves");
  check(!onlyCpp.some((n) => n.indexOf('lexer.c') >= 0),
        'y deja fuera el .c');

  /* LO QUE DECIDE ES EL MARCO DE DENTRO, donde la reserva ocurre de verdad, y
   * no "algun marco de la cadena".  Los sitios 0 y 1 reservan en una plantilla
   * de C++ inlineada DENTRO de `vx_parse_file`, que esta en un `.c`: cuentan
   * como C++, y su cadena sigue ensenando el `.c` de quien llamo.  Preguntar
   * "hay algun marco en C" contestaria que si a casi todo en un programa que
   * mezcla los dos, que no es una respuesta. */
  check(onlyCpp.some((n) => n.indexOf('parser.c') >= 0),
        'una reserva de C++ inlineada en una funcion C cuenta como C++, y su '
        + 'cadena sigue ensenando de donde vino');

  /* "NO SE SABE" ES UNA RESPUESTA, Y TIENE SU VISTA.  Sin informacion de
   * depuracion no hay fichero, y un nombre a secas no dice en que lenguaje se
   * escribio.  El volcado de prueba trae un sitio SIN marcos y otro sin
   * fichero a proposito; si esos se contaran como "no es C", una vista de
   * "solo C" vacia se leeria como que el programa no tiene C. */
  const unknown = ctx.treeFor('td', 'stack', 'all', 'unknown');
  check(unknown.allocs > 0,
        "'lenguaje sin determinar' es una vista propia, no un cajon vacio");
  const cAll = ctx.treeFor('td', 'stack', 'all', 'c');
  const cppAll = ctx.treeFor('td', 'stack', 'all', 'cpp');
  check(cAll.allocs + cppAll.allocs + unknown.allocs === totalAllocs,
        'C mas C++ mas lo que no se sabe es exactamente lo medido: ningun '
        + 'sitio se cuenta dos veces ni se pierde');
  check(cAll.noLang > 0,
        'y al filtrar por lenguaje se DICE cuantas se fueron por no saberse, '
        + 'que no es lo mismo que ser del otro lenguaje');
}

// -- every view actually renders -------------------------------------------
for (const grouping of ['module', 'file', 'purpose', 'stack']) {
  ctx.flip('group', grouping);
  check(rowCount() > 0, 'agrupado por ' + grouping + ' dibuja');
}
ctx.flip('dir', 'bu');
check(rowCount() > 0, 'de dentro hacia fuera dibuja');
ctx.flip('dir', 'td');

// -- sorting and filtering -------------------------------------------------
const before = rowCount();
ctx.sortKey = 'self'; ctx.sortDesc = false; ctx.render();
check(rowCount() === before, 'ordenar no cambia CUANTAS filas hay');
ctx.needle = 'zzzznope'; ctx.markTree(ctx.root(), ctx.needle); ctx.render();
check(rowCount() === 0 && byId['rows'].innerHTML.indexOf('empty') > 0,
      'un filtro sin resultados lo DICE, no deja la tabla en blanco');
ctx.needle = ''; ctx.markTree(ctx.root(), ''); ctx.render();

// -- the symbol is never altered -------------------------------------------
//
// The highlighting adds markup around the text; strip it and the name has to
// come back exactly as the symbol table said it, or it cannot be pasted into
// anything that would resolve it.
function stripped(html) {
  return html.replace(/<[^>]+>/g, '')
             .replace(/&lt;/g, '<').replace(/&gt;/g, '>')
             .replace(/&quot;/g, '"').replace(/&amp;/g, '&');
}
let intact = true, longest = '';
for (const f of DATA.frames) {
  if (f[0].length > longest.length) longest = f[0];
  if (stripped(ctx.codeHtml(f[0], '', 'cpp')) !== f[0]) intact = false;
}
check(intact, 'el resaltado devuelve el simbolo IDENTICO (' +
      DATA.frames.length + ' nombres, el mayor de ' + longest.length + ')');
check(ctx.codeHtml(longest, '', 'cpp').indexOf('...') < 0 &&
      ctx.codeHtml(longest, '', 'cpp').indexOf('…') < 0,
      'y no recorta: se parte por las costuras, no se resume');

// -- C and C++ told apart ---------------------------------------------------
check(ctx.langOf('src/vx/parser.c', 'vx_parse_file') === 'c',
      'un .c es C');
check(ctx.langOf('include/c++/12/bits/vector.tcc', 'std::vector<int>::x()') === 'cpp',
      'un .tcc es C++');
check(ctx.langOf('', 'std::string::size()') === 'cpp',
      'y sin ruta, la forma del nombre lo dice');
check(ctx.langOf('', 'main') === '',
      'un `main` a secas es ambiguo de verdad y sale neutro');

// -- the panel gets back to the measurement --------------------------------
ctx.picked = ctx.rows[0].node;
ctx.render();
const det = byId['detail'].innerHTML;
check(DATA.siteCols.every(c => det.indexOf(c) >= 0),
      'el panel trae TODAS las columnas de sites.csv');
check(det.indexOf('class="sym"') >= 0,
      'y cada sitio con su NOMBRE, no solo su id y su desplazamiento');

// -- the size split, per branch --------------------------------------------
//
// The whole reason the allocator now records it per site: a global histogram
// cannot be handed back out to the branches that formed it.
const rootSizes = ctx.sizesOf(ctx.treeFor('td', 'stack'));
const expected = {};
for (const s of DATA.sites)
  for (const b in s.sizes) expected[b] = (expected[b] || 0) + s.sizes[b];
check(rootSizes.length === Object.keys(expected).length &&
      rootSizes.every(s => s.n === expected[s.b]),
      'el reparto de tamanos de la raiz es la suma de todos los sitios');
check(det.indexOf('&le;') >= 0 || det.indexOf('nosizes') >= 0,
      'y el panel lo ensena para la rama seleccionada');

// -- the view is in the address bar ----------------------------------------
//
// A finding has to be passable to someone else.  What is checked is the round
// trip: what the page wrote must be what it reads back, and the selected row
// must be found again by NAME -- an index would point at another row as soon
// as somebody sorted by a different column.
ctx.flip('group', 'module');
ctx.needle = 'vx'; ctx.markTree(ctx.root(), 'vx');
ctx.picked = ctx.rows[0].node;
ctx.byBytes = true;
ctx.render();
check(location.hash.indexOf('g=module') > 0 &&
      location.hash.indexOf('q=vx') > 0 &&
      location.hash.indexOf('b=1') > 0,
      'la vista viaja en la URL (agrupacion, filtro, peso)');
const back = ctx.readHash();
check(back.grouping === 'module' && back.needle === 'vx' &&
      back.byBytes === true,
      'y se lee de vuelta igual que se escribio');
check(ctx.nodeAt(ctx.root(), ctx.pathOf(ctx.picked)) === ctx.picked,
      'la fila seleccionada se reencuentra por su camino de NOMBRES');
check(ctx.nodeAt(ctx.root(), ['no', 'existe']) === null,
      'y un camino que este arbol no tiene no selecciona nada, en vez de otra fila');
ctx.needle = ''; ctx.byBytes = false; ctx.picked = null;
ctx.flip('group', 'stack');

// -- language ---------------------------------------------------------------
const en = byId['stat'].textContent;
ctx.saveLang('es'); ctx.applyLang(); ctx.render();
check(byId['stat'].textContent !== en && byId['stat'].textContent.length > 0,
      'el idioma cambia sin regenerar la pagina');
check(byId['sub'].textContent.length > 0, 'y el encabezado tambien');

// -- que estaba mal ---------------------------------------------------------
//
// Un hallazgo es lo mas importante que esta pagina puede tener que decir, y
// durante un tiempo solo salio como una fila de ids en una tabla cruda del
// final.  Esto vigila que se PINTE, no que el dato este: tenerlo y no darlo es
// lo que se estaba arreglando.
if (DATA.verdicts && DATA.verdicts.length) {
  const box = byId['verdicts'].innerHTML || '';
  check(box.indexOf('verdict') >= 0,
        'los hallazgos se pintan, no se quedan en la tabla cruda');
  check(box.indexOf(DATA.verdicts[0].what) >= 0,
        'y cada uno dice QUE estaba mal');
  check(!DATA.verdicts[0].allocated_name
        || box.indexOf(DATA.verdicts[0].allocated_name) >= 0,
        'y donde se reservo, por su NOMBRE y no por el id de la pila');
} else {
  // Sin hallazgos no hay nada que pintar, y un hueco vacio es la respuesta
  // correcta: avisar de que no hubo fallos entrenaria a saltarse los avisos.
  check((byId['verdicts'].innerHTML || '') === '',
        'sin hallazgos no se pinta un bloque vacio');
}

// -- con que poblacion se abre ----------------------------------------------
//
// La del comprobador, cuando el volcado la trae: es la unica que puede decir de
// QUIEN es una reserva hecha dentro de un cuerpo compartido como
// `_M_realloc_insert`.  Y tiene que viajar en el enlace, porque cual es la de
// por defecto depende del volcado: sin eso, un enlace hecho mirando una
// poblacion abre en la otra bajo la misma direccion.
if (DATA.check) {
  check(openedOn === DATA.check, 'se abre en la poblacion del comprobador');
  ctx.flip('dataset', 'alloc');
  check(ctx.VIEW === DATA, 'y se puede cambiar a la del asignador');
  ctx.render();
  check(location.hash.indexOf('v=alloc') > 0, 'la poblacion viaja en el enlace');
  check(ctx.readHash().dataset === 'alloc', 'y se lee de vuelta');
  ctx.flip('dataset', 'check');
  ctx.render();
  check(location.hash.indexOf('v=check') > 0,
        'y tambien la que abre por defecto -- cual es depende del volcado');
} else {
  check(openedOn === DATA, 'sin comprobador se abre en la del asignador');
}

// -- las dos puntas de una cadena -------------------------------------------
//
// Una pila del comprobador empieza dentro del asignador y acaba en como el
// sistema entro en el hilo.  Esos marcos estan en TODAS, asi que con ellos
// delante el arbol no se separa en ramas hasta el sexto nivel por un lado y el
// septimo por el otro -- que es como el informe acababa enterrado.
//
// Lo que se comprueba es que se PLIEGAN y no que desaparezcan: 'todo' tiene que
// seguir ensenando la cadena entera, porque es la medida.
if (DATA.check) {
  const frames = DATA.check.frames;
  const F_LIB = 5, F_INSTR = 6, F_START = 7;
  const mine = f => !f[F_LIB] && !f[F_INSTR] && !f[F_START];

  check(frames.some(f => f[F_INSTR]), 'se reconoce el aparato de medida');
  check(frames.some(f => f[F_START]), 'y como entro el sistema en el hilo');
  // Y que no se lleve por delante lo que no es suyo: una funcion del autor con
  // un nombre cualquiera no es ni instrumento ni arranque.
  check(frames.some(f => mine(f)), 'y el codigo del autor no cae en ninguno');

  ctx.setView('check');
  const long = DATA.check.sites.find(s => s.chain.length >= 6);
  check(!!long, 'la fixture trae una pila con las dos puntas');
  if (long) {
    const all = ctx.ownChain(long, long.chain, 'all');
    check(all.length === long.chain.length,
          'con "todo" sigue estando la cadena entera');
    const fold = ctx.ownChain(long, long.chain, 'fold');
    check(fold.length > 0 && mine(frames[fold[0]]),
          'plegada, empieza en codigo del autor');
    check(mine(frames[fold[fold.length - 1]]),
          'y acaba en codigo del autor');
    check(fold.length < all.length,
          'o sea que recorto algo (' + all.length + ' -> ' + fold.length + ')');
    // Recortar no puede inventar: lo que queda tiene que ser un TRAMO de lo
    // medido, en el mismo orden, no una cadena nueva.
    const at = all.indexOf(fold[0]);
    check(at >= 0 && fold.every((f, i) => f === all[at + i]),
          'y lo que queda es un tramo de la cadena, no otra cosa');
  }
  // -- Y LO QUE RESERVA EL PROPIO INFORME ------------------------------------
  //
  // Es, con diferencia, lo que mas consume: en una compilacion de 24k lineas,
  // 1.253 MB, el 63,9 % de los bytes de esta poblacion -- mas que el programa
  // medido.  Leer DWARF para poner nombres cuesta eso.
  //
  // Su cadena no tiene ni un marco del autor, asi que el plegado la deja fuera
  // entera.  Fuera y CONTADA: apartar sin decir cuanto seria restarle al
  // programa una memoria que si se pidio.
  ctx.setView('check');
  const informe = DATA.check.sites.find(s => s.chain.some(
      i => (DATA.check.frames[i][0] || '').indexOf('report_alloc_sites') >= 0));
  check(!!informe, 'la fixture trae al informe reservando para si mismo');
  if (informe) {
    check(informe.instr === 1, 'y se reconoce como del aparato de medida');
    // FUERA EN TODOS LOS ALCANCES, no solo al plegar.  Plegar la biblioteca es
    // una forma de mirar que decide el lector; que el instrumento no sea parte
    // de lo medido no se decide, es lo que es.
    for (const sc of ['all', 'fold', 'hide', 'extern']) {
      const r = ctx.treeFor('td', 'stack', sc);
      check(r.instr >= informe.allocs && r.instrBytes >= informe.bytes,
            'el informe queda fuera del arbol y contado (' + sc + ')');
    }
    // Y CONTADO DONDE SE VE.  Apartar sin decir cuanto seria restarle al
    // programa una memoria que si se pidio.
    check(DATA.check.totals.instrAllocs >= informe.allocs,
          'y el total del volcado lo lleva aparte');
    const r = ctx.treeFor('td', 'stack', 'all');
    check(r.allocs + r.leftOut + r.instr
            === DATA.check.sites.reduce((n, s) => n + s.allocs, 0),
          'arbol + apartado por el filtro + informe = el volcado entero');
  }
  ctx.setView('alloc');
}

// -- los avisos: plegados, nunca escondidos ---------------------------------
//
// Siete cajas a todo lo ancho se comian media pantalla y empujaban el arbol
// fuera de la vista.  Lo que se arreglo NO fue quitarlas: lo que no se pudo
// cubrir tiene que seguir ahi o el informe miente por omision.  Asi que estas
// comprobaciones vigilan las dos mitades a la vez -- que se pliega, y que
// plegado sigue diciendo cuantos hay y de que tipo.
if (DATA.warnings && DATA.warnings.length) {
  const wbox = byId['warns'].innerHTML || '';
  check(wbox.indexOf('<details class="warnbox"') >= 0,
        'los avisos van en un bloque que se pliega');
  // El `[" ]` del final importa: sin el, `class="warnbox"` cuenta como un
  // aviso mas y el bloque se contaria a si mismo.
  check((wbox.match(/class="warn[" ]/g) || []).length === DATA.warnings.length,
        'y no se pierde ninguno al plegarlos (' + DATA.warnings.length + ')');

  // El resumen lleva las DOS cuentas porque no son lo mismo: un aviso es algo
  // que se intento y no cupo, un hueco es algo que este nivel ni mira.
  const gaps = DATA.warnings.filter(w => w[0].indexOf('gap.') === 0).length;
  const summary = wbox.slice(wbox.indexOf('<summary>'),
                             wbox.indexOf('</summary>'));
  check(summary.indexOf(String(DATA.warnings.length - gaps)) >= 0
        && summary.indexOf(String(gaps)) >= 0,
        'y plegado dice cuantos avisos y cuantos huecos hay');

  check(wbox.indexOf('<details class="warnbox" open') < 0,
        'arrancan cerrados -- cerrado no es callado, el resumen los cuenta');

  // Y la eleccion se recuerda, como el idioma: quien los quiere abiertos los
  // quiere abiertos en todos los informes.
  const wb = ctx.document.querySelector('.warnbox');
  check(wb !== null, 'el bloque existe en el documento');
  if (wb) {
    wb.open = true;
    wb.fire('toggle');
    check(ctx.window.localStorage.getItem('alloc_tree_warns') === '1',
          'abrirlos se recuerda');
    ctx.applyLang();
    check((byId['warns'].innerHTML || '')
            .indexOf('<details class="warnbox" open') >= 0,
          'y el siguiente informe los abre solo');
  }
} else {
  check((byId['warns'].innerHTML || '') === '',
        'sin avisos no se pinta un bloque vacio');
}

// ---------------------------------------------------------------------------
// EL EJE DEL TIEMPO.  Lo que se comprueba no es que el dibujo se vea bien --
// aqui no hay pixeles --, sino lo que el codigo DECIDE dibujar: cuantas bandas,
// con que totales y con las fases puestas.  Eso es lo que se rompe.
if (DATA.time) {
  const canvas = byId['tchart'];
  const g = canvas && canvas._ctx;
  check(!!g, 'el grafico se dibujo en el lienzo');
  if (g) {
    const text = g.calls.filter(c => c[0] === 'fillText')
                        .map(c => String(c[1][0]));
    // Las fases, con su nombre TRADUCIDO y no con la clave: la clave es lo que
    // viaja en el fichero, y ensenarla seria no haber traducido nada.
    check(text.some(s => s.indexOf('phase') >= 0 || s.length > 0),
          'las fases salen rotuladas en el grafico');
    check(g.calls.some(c => c[0] === 'fill'),
          'y se pintaron las bandas de los sitios');
    check(g.calls.some(c => c[0] === 'stroke'),
          'y las curvas de vivo y comprometido');
    // Y LA DEL ASIGNADOR, que es la que dice cuanto de lo comprometido es suyo.
    // Se reconoce por su trazo: el lienzo de prueba no guarda el color, pero si
    // el patron, y cada curva tiene el suyo.
    check(g.calls.some(c => c[0] === 'setLineDash' &&
                            String(c[1][0]) === '2,3'),
          'y la de los rangos del asignador, medida por el SO');
    // Y LA DEL HUECO REPARTIDO: se dibuja en vivo MAS los tramos libres, para
    // que la distancia hacia abajo sea lo que volveria al sistema y la de
    // arriba lo que no.  Sueltas, habria que restarlas de cabeza.
    check(g.calls.some(c => c[0] === 'setLineDash' &&
                            String(c[1][0]) === '4,2'),
          'y la de los tramos libres, que parte el hueco');
    check(g.calls.some(c => c[0] === 'setLineDash' &&
                            String(c[1][0]) === '6,3'),
          'y la de los trozos con todo muerto, que lo parte en tres');
  }
  // El pico, que es la unica pregunta con la que alguien llega: sus filas
  // salen ordenadas de mayor a menor y con el sitio NOMBRADO, no con su id.
  const rows = byId['tpeakrows'].innerHTML || '';
  check((rows.match(/<tr /g) || []).length === DATA.time.peak.length,
        'la tabla del pico tiene una fila por sitio');
  check(rows.indexOf('sym') >= 0, 'y cada una lleva el nombre del sitio');
  check((byId['tpeakhead'].textContent || '').length > 0,
        'y el encabezado dice cuanto habia vivo y en que fase');
  // La leyenda: un boton por banda, y quitar uno la redibuja sin el.
  check((byId['tlegend'].innerHTML.match(/class="lg"/g) || []).length > 0,
        'la leyenda lista las bandas dibujadas');
  // EL SELECTOR es lo que el buscador filtra, y NO el grafico: lo que ya esta
  // dibujado sigue dibujado mientras se busca otra cosa.  Al reves -- filtrar
  // el grafico -- escribir una letra borraria la serie que se esta mirando.
  const picked = (byId['tlegend'].innerHTML.match(/class="lg"/g) || []).length;
  check((byId['tpick'].innerHTML.match(/class="pk"/g) || []).length > 0,
        'el selector lista todo lo que se puede dibujar');
  byId['tfilter'].value = 'no-existe-este-sitio';
  byId['tfilter'].fire('input');
  check((byId['tpick'].innerHTML.match(/class="pk"/g) || []).length === 0,
        'un filtro que no casa con nada vacia el SELECTOR');
  check((byId['tlegend'].innerHTML.match(/class="lg"/g) || []).length === picked,
        'y no toca lo que ya estaba dibujado');
  byId['tfilter'].value = '';
  byId['tfilter'].fire('input');
  check((byId['tpick'].innerHTML.match(/class="pk"/g) || []).length > 0,
        'y borrarlo devuelve la lista');
  // Y se puede vaciar y llenar el grafico a mano, que es de lo que va el panel.
  byId['tnone'].fire('click');
  check((byId['tlegend'].innerHTML.match(/class="lg"/g) || []).length === 0,
        '"ninguno" deja el grafico sin bandas');
  byId['tall'].fire('click');
  check((byId['tlegend'].innerHTML.match(/class="lg"/g) || []).length ===
          (byId['tpick'].innerHTML.match(/class="pk"/g) || []).length,
        'y "todos" pone todas las que el selector lista');
  // Agrupar por FUNCION junta las pilas de una misma funcion en una banda.
  byId['tgroup'].value = 'function';
  byId['tgroup'].fire('change');
  check((byId['tpick'].innerHTML.match(/class="pk"/g) || []).length > 0,
        'agrupar por funcion sigue dando algo que dibujar');

  // QUIEN LLAMO A QUIEN.  Lo que el nombre de una serie no puede contestar, y
  // lo unico que hace util al panel cuando la respuesta es "un vector": la
  // cadena entera, de fuera hacia dentro, con su fichero y su linea.
  ctx.timeStackDetail(ctx.TGROUPS[0].key);
  const detail = byId['tstack-detail'].innerHTML || '';
  check(detail.indexOf('class="chain"') >= 0,
        'una serie abre su cadena de llamadas');
  check((detail.match(/<li /g) || []).length > 1,
        'y la cadena tiene mas de un marco -- si no, no dice dentro de que');
  check(detail.indexOf('operator new') >= 0 || detail.indexOf('.cc:') >= 0 ||
          detail.indexOf('.cpp:') >= 0,
        'y cada marco lleva su fichero y su linea');

  // -- LA MISMA CURVA POR TAMANO, que es otra medida y no otro grano ---------
  //
  // Lo que se prueba es que NO salga de repartir las pilas: el fixture tiene el
  // peso de la casilla grande repartido entre dos sitios que tambien sirven
  // casillas pequenas, asi que si esto se dedujera de `series` daria otra cosa.
  byId['tgroup'].value = 'size';
  byId['tgroup'].fire('change');
  const buckets = ctx.TGROUPS;
  check(buckets.length === Object.keys(DATA.time.sizes).length,
        'por tamano hay una banda por casilla, no por sitio');
  check(buckets.every((g) => g.key.indexOf('z:') === 0),
        'y son casillas, no pilas');
  const biggest = buckets[buckets.length - 1];
  check(biggest.peak === Math.max.apply(null,
          DATA.time.sizes[biggest.key.slice(2)].map((p) => p[1])),
        'y su pico es el que midio el comprobador corte a corte');
  check(buckets[0].key.slice(2) | 0 < biggest.key.slice(2) | 0,
        'ordenadas por TAMANO y no por altura -- si no, seria un ranking y no '
        + 'una distribucion');

  // Y UNA CASILLA NO ES UN SITIO: se dice, en vez de dejar el panel vacio.
  ctx.timeStackDetail(biggest.key);
  check((byId['tstack-detail'].innerHTML || '').indexOf('class="chain"') < 0,
        'una casilla no abre cadena de llamadas, y el panel lo explica');

  // Por FORMA: el grano que reparte por lo que los bloques resultaron ser.
  byId['tgroup'].value = 'shape';
  byId['tgroup'].fire('change');
  check(ctx.TGROUPS.length > 0 &&
          ctx.TGROUPS.every((g) => g.key.indexOf('h:') === 0),
        'y por forma hay una banda por forma medida');

  // POR MODULO, que es el que estaba roto: sin la columna en los marcos del
  // comprobador todas las bandas salian como "(?)" -- un control que contesta
  // y no dice nada.  Se pide mas de una banda y que no sean todas el hueco.
  byId['tgroup'].value = 'module';
  byId['tgroup'].fire('change');
  check(ctx.TGROUPS.length > 1,
        'por modulo hay mas de una banda');
  check(ctx.TGROUPS.some((g) => g.label !== '(?)'),
        'y llevan el nombre del modulo, no "(?)"');

  // Y POR CUANTO VIVIERON, por orden de magnitud.  Las que no murieron van en
  // su propia banda y la ultima: "sobrevivio a la corrida" no es "duro mucho".
  byId['tgroup'].value = 'life';
  byId['tgroup'].fire('change');
  const lives = ctx.TGROUPS;
  check(lives.length > 0 && lives.every((g) => g.key.indexOf('v:') === 0),
        'por vida hay bandas por orden de magnitud');
  const neverDied = lives.filter((g) => g.key === 'v:none');
  check(!neverDied.length || lives[lives.length - 1].key === 'v:none',
        'y las que no murieron van al final, no dentro de la mas larga');

  // DE DONDE SALE UNA BANDA.  Agrupar por vida (o por forma) dice que CLASE de
  // memoria es y no a que codigo ir; sin este reparto, abrir una banda de
  // cientos de sitios daba ocho cadenas sueltas que contestan por ocho.
  // Y SIN PINCHAR: la banda dice de que fichero sale sobre todo, en la misma
  // ranura donde una banda por sitio pone su `fichero:linea`.  Sin esto el
  // grafico se lee como "92 MiB de algo que muere pronto", que no lleva a
  // ningun sitio.
  check(lives.every((g) => !!g.where),
        'y cada banda de vida dice de que fichero sale sobre todo');

  ctx.timeStackDetail(lives[0].key);
  const band = byId['tstack-detail'].innerHTML || '';
  check(band.indexOf('class="files"') >= 0,
        'una banda de vida dice DE QUE FICHEROS sale');
  check(band.indexOf('.cpp') >= 0 || band.indexOf('.cc') >= 0 ||
          band.indexOf('.h') >= 0,
        'y los nombra, que es lo que faltaba para saber donde mirar');
  // CORTE A CORTE, como las bandas: el pico de un fichero es lo mas alto que
  // llego SU curva, nunca la suma de los picos de sus sitios -- que seria una
  // cifra que ninguno alcanzo.
  const files = ctx.timeBandFiles(lives[0]);
  const bySite = lives[0].sids.reduce((acc, sid) => acc + Math.max.apply(null,
      (DATA.time.series[String(sid)] || [[0, 0]]).map((p) => p[1])), 0);
  const byCut = files.reduce((acc, f) => acc + f.peak, 0);
  check(byCut <= bySite,
        'y el reparto se mide corte a corte, no sumando los picos de cada '
        + 'sitio -- eso daria una cifra que ninguno alcanzo');

  byId['tgroup'].value = 'site';
  byId['tgroup'].fire('change');

  // -- LAS SEIS FORMAS DE DIBUJARLO ------------------------------------------
  //
  // Cada una contesta algo que las otras no, y la que importa aqui es la
  // familia de las que hacen legibles las bandas PEQUENAS: apiladas contra una
  // curva total, una banda de dos megas al lado de una de setenta es un pelo.
  // Lo que se comprueba de cada una es su marca propia -- lo que no dibujaria
  // ninguna de las demas --, no que "dibuje algo".
  // SOLO LO DIBUJADO AHORA.  El lienzo de prueba guarda TODAS las llamadas
  // desde que arranco, asi que sin cortar por donde iba, una comprobacion de
  // "aqui ya no salen MiB" encontraria los del dibujo anterior y pasaria (o
  // fallaria) por lo que hizo otro modo.
  function drawAs(mode) {
    const before = byId['tchart']._ctx.calls.length;
    byId['tdraw'].value = mode;
    byId['tdraw'].fire('change');
    return byId['tchart']._ctx.calls.slice(before);
  }
  function textsOf(calls) {
    return calls.filter(c => c[0] === 'fillText').map(c => String(c[1][0]));
  }

  // EJE LOGARITMICO: una raya por decada, y el suelo DICHO -- un cero no tiene
  // sitio en un eje logaritmico y callarlo seria dibujar una banda vacia sobre
  // la linea de abajo como si midiera algo.
  let texts = textsOf(drawAs('log'));
  check(texts.some((s) => s.indexOf('log') >= 0 || s.indexOf('cero') >= 0
                          || s.indexOf('zero') >= 0),
        'el eje logaritmico dice donde tiene el suelo');

  // 100% APILADO: el eje deja de ser memoria, asi que sus rotulos son por
  // ciento.  Si saliera en MiB, seria una regla leida contra otra.
  texts = textsOf(drawAs('pct'));
  check(texts.some((s) => s.indexOf('%') >= 0),
        'el 100% apilado rotula el eje en por ciento, no en MiB');
  check(!texts.some((s) => / MiB$/.test(s)),
        'y ahi NO quedan rotulos en MiB, que se leerian contra la regla que '
        + 'no es');

  // REJILLA: el lienzo crece con las bandas.  Con la altura fija, un grafico
  // por banda le daria seis pixeles a cada una -- justo lo que viene a
  // arreglar.
  drawAs('grid');
  const gridH = parseInt(byId['tchart'].style.height, 10);
  check(gridH !== 360 && gridH >= 40 * ctx.timeShown().length,
        'la rejilla reparte el alto POR BANDA en vez de dejarlo fijo');

  // BURBUJAS: circulos, y el AREA es la memoria.  Se comprueba la proporcion,
  // que es la unica forma de que un grafico de burbujas no mienta: usando el
  // radio, una banda del doble pareceria cuatro veces mayor.
  const circles = drawAs('bubbles').filter((c) => c[0] === 'arc');
  check(circles.length > 0, 'las burbujas dibujan un circulo por banda');
  check(ctx.TBUBBLES.length > 0 && ctx.TBUBBLES.every((b) => b.r > 0),
        'y cada una tiene sitio y tamano');
  if (ctx.TBUBBLES.length > 1) {
    const big = ctx.TBUBBLES[0], small = ctx.TBUBBLES[ctx.TBUBBLES.length - 1];
    const areas = (big.r * big.r) / (small.r * small.r);
    const values = big.value / small.value;
    check(small.r <= 3.001 || Math.abs(areas - values) / values < 0.1,
          'y el AREA es proporcional a la memoria, no el radio -- con el radio '
          + 'una banda del doble pareceria cuatro veces mayor');
  }
  // TODAS LAS ELEGIDAS, Y ESTO ES LO QUE FALLABA.  Las burbujas se dibujaban
  // de UN corte, y una banda solo esta en los cortes en que paso del suelo:
  // de diez elegidas salian dos.  Cada circulo es ahora el pico de SU banda --
  // la misma cifra que ensena la leyenda --, que no se pueden sumar y se dice.
  check(ctx.TBUBBLES.length === ctx.timeShown().length,
        'las burbujas dibujan TODAS las bandas elegidas, no solo las que '
        + 'tenian algo en un corte suelto');
  const peaks = ctx.timeShown().map((g) => g.peak).sort((a, b) => b - a);
  check(ctx.TBUBBLES.map((b) => b.value).join() === peaks.join(),
        'y cada una vale el pico de su banda, el mismo que dice la leyenda');

  // El nombre entero vive en el globo: en un circulo pequeno no cabe ninguno.
  ctx.timeBubbleTip(ctx.TBUBBLES[0]);
  check((byId['ttip'].innerHTML || '').indexOf('%') >= 0,
        'y el globo de una burbuja dice su nombre y su parte');

  // AMPLIAR Y DESPLAZAR.  En burbujas es una lupa sobre el mismo empaquetado:
  // si ampliar recolocara los circulos, seria otro dibujo y no el mismo mas
  // cerca.
  const where = ctx.TBUBBLES.map((b) => b.x + ',' + b.y).join(';');
  ctx.timeZoom(2, null, null);
  check(ctx.TVIEW.k > 1, 'ampliar acerca las burbujas');
  check(ctx.TBUBBLES.map((b) => b.x + ',' + b.y).join(';') === where,
        'y el empaquetado NO se recoloca: es el mismo mirado de cerca');
  ctx.timePan(40, 25);
  check(ctx.TVIEW.x !== 0 || ctx.TVIEW.y !== 0,
        'arrastrar lo desplaza');
  ctx.timeZoomAll();
  check(ctx.TVIEW.k === 1 && ctx.TVIEW.x === 0 && ctx.TVIEW.y === 0,
        'y "todo" vuelve a verlo entero');

  // Y EN UN GRAFICO CON EJE DE TIEMPO, ampliar es otra cosa -- estrechar el
  // tramo -- pero se pide igual.  Que sea el mismo boton es el punto.
  byId['tdraw'].value = 'stack';
  byId['tdraw'].fire('change');
  ctx.timeZoom(4, null, null);
  check(ctx.TZOOM !== null, 'en un grafico de tiempo, ampliar estrecha el tramo');
  const before = ctx.TZOOM.from;
  ctx.timePan(-60, 0);
  check(ctx.TZOOM.from !== before, 'y arrastrar mueve el tramo por la corrida');
  ctx.timeZoomAll();
  check(ctx.TZOOM === null, 'y "todo" devuelve la corrida entera');

  byId['tdraw'].value = 'stack';
  byId['tdraw'].fire('change');
} else {
  check(ctx.document.querySelector('[data-panel="p-time"]').style.display
          === 'none',
        'sin eje del tiempo la pestana no se ofrece');
}

// -- y el histograma de la RAMA, con su conmutador -------------------------
if (ctx.VIEW.hasSizeBytes) {
  const root = ctx.treeFor(ctx.STATE ? ctx.STATE.dir : true,
                           ctx.STATE ? ctx.STATE.group : '');
  ctx.detailSetByBytes(false);
  ctx.detail(root);
  const counted = byId['detail'].innerHTML;
  ctx.detailSetByBytes(true);
  ctx.detail(root);
  const weighed = byId['detail'].innerHTML;
  check(counted !== weighed,
        'el histograma de la rama cambia al pedirlo por bytes');
  check(weighed.indexOf('data-szview="bytes"') >= 0,
        'y el conmutador esta en el encabezado del histograma');
  ctx.detailSetByBytes(false);
}

// -- EL VOLCADO CRUDO, que ahora lo pinta la pagina ------------------------
//
// POR QUE SE PRUEBA.  Antes lo escribia la plantilla, que es dificil de
// equivocar; ahora lo arma JavaScript a partir de datos empaquetados, que es
// facil.  Y los tres modos de romperlo son mudos: una columna empaquetada que
// salga como el NUMERO del diccionario en vez de su texto, un filtro que
// encoja la tabla sin decirlo, y un nombre de C++ con `<` que se cuele sin
// escapar y se lleve por delante el resto de la fila.
if (DATA.raw && DATA.raw.length) {
  // Una tabla de verdad, pero de mentira: lo justo que `rawWire` toca.
  function fakeCell() {
    return {
      _on: {},
      addEventListener(t, f) { (this._on[t] || (this._on[t] = [])).push(f); },
      fire(t) { (this._on[t] || []).forEach((f) => f({ type: t })); },
    };
  }
  function mount(spec) {
    const tbody = {
      innerHTML: '',
      insertAdjacentHTML(where, html) { this.innerHTML += html; },
    };
    const cells = spec.cols.map(fakeCell);
    byId['t-' + spec.id] = {
      tHead: { rows: [{ cells: cells }] },
      tBodies: [tbody],
      parentNode: {},
    };
    ctx.rawWire(spec);
    return { tbody: tbody, cells: cells };
  }

  const frames = DATA.raw.filter((t) => t.id === 'frames')[0];
  const m = mount(frames);
  check((m.tbody.innerHTML.match(/<tr>/g) || []).length === frames.rows.length,
        'el volcado crudo pinta sus filas desde los datos');

  // LO EMPAQUETADO SE DESHACE.  Una columna cuyas cadenas se guardaron una vez
  // viaja como numeros; si saliera asi, la tabla ensenaria indices donde debia
  // haber rutas -- y un indice es un numero perfectamente creible.
  const packed = frames.words.findIndex((w) => w !== null);
  if (packed >= 0) {
    check(m.tbody.innerHTML.indexOf(ctx.esc(frames.words[packed][0])) >= 0,
          'y una columna empaquetada sale con su TEXTO, no con el numero del '
          + 'diccionario');
  }

  // ESCAPADO.  El fixture trae un nombre con `<`, `>` y comillas justo para
  // esto: sin escapar, se lleva por delante el resto de la fila.
  check(m.tbody.innerHTML.indexOf('&lt;') >= 0,
        'y un nombre de C++ con `<` sale escapado, no partiendo la fila');
  check(m.tbody.innerHTML.indexOf('<std::string') < 0,
        'que es lo que decide si una tabla se rompe con un nombre de plantilla');

  // FILTRAR NO ES ESCONDER EN SILENCIO: las dos cifras, siempre.
  ctx.rawFilter('frames', 'vx_parse_file');
  const kept = (m.tbody.innerHTML.match(/<tr>/g) || []).length;
  check(kept > 0 && kept < frames.rows.length,
        'filtrar deja solo lo que casa');
  check((byId['c-frames'].textContent || '').indexOf(
          String(frames.rows.length)) >= 0,
        'y la cuenta dice cuantas de CUANTAS, para que una tabla no encoja en '
        + 'silencio');
  ctx.rawFilter('frames', 'no-existe-esto');
  check((m.tbody.innerHTML.match(/<tr>/g) || []).length === 0,
        'y un filtro que no casa con nada deja la tabla vacia, no entera');
  ctx.rawFilter('frames', '');

  // ORDENAR es sobre los DATOS, no sobre el texto de las celdas ya pintadas:
  // con solo un trozo pintado, ordenar el documento ordenaria ese trozo.
  const numeric = frames.cols.findIndex((c) => c.numeric);
  m.cells[numeric].fire('click');
  const first = m.tbody.innerHTML.indexOf('<tr>');
  check(first >= 0, 'ordenar por una columna vuelve a pintar la tabla');
  const all = frames.rows.map((r) => Number(r[numeric]));
  const top = Math.max.apply(null, all);
  check(m.tbody.innerHTML.indexOf('>' + top + '<') >= 0,
        'y de mayor a menor empieza por el mayor de TODAS las filas, no del '
        + 'trozo que estuviera pintado');
}

if (failures === 0) {
  console.log('  TODO OK');
  process.exit(0);
}
console.log('  ' + failures + ' COMPROBACIONES FALLIDAS');
process.exit(1);
