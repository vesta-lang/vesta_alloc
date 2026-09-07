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
const SCRIPTS = ['code.js', 'i18n.js', 'tables.js', 'tree.js', 'link.js',
                 'detail.js', 'page.js'];

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

// -- it draws at all -------------------------------------------------------
check(rowCount() > 0, 'la rejilla dibuja filas al arrancar');
check(byId['stat'].textContent.indexOf('rows') >= 0,
      'y la linea de estado dice cuantas');

// -- the tree adds up, in every shape --------------------------------------
//
// Folding must not lose allocations.  A tree that has lost some still draws,
// and its percentages still add up to a hundred -- of a smaller total.
const totalAllocs = DATA.sites.reduce((n, s) => n + s.allocs, 0);
const totalBytes = DATA.sites.reduce((n, s) => n + s.bytes, 0);
for (const dir of ['td', 'bu']) {
  for (const grouping of ['stack', 'module', 'file', 'purpose']) {
    const root = ctx.treeFor(dir, grouping);
    check(root.allocs === totalAllocs && root.bytes === totalBytes,
          'no se pierde nada al plegar (' + dir + ', ' + grouping + ')');
  }
}

// -- "solo mis llamadas": lo que deja fuera CUADRA ---------------------------
//
// Es la propiedad que hace que el filtro se pueda creer: lo que queda mas lo
// que se dejo fuera tiene que ser exactamente lo medido.  Un filtro que pierde
// reservas por el camino se lee igual que un programa que reserva poco, y no
// habria forma de distinguirlo.
for (const scope of ['all', 'fold', 'hide', 'extern']) {
  for (const lang of ['all', 'c', 'cpp', 'unknown']) {
    const root = ctx.treeFor('td', 'stack', scope, lang);
    check(root.allocs + root.leftOut === totalAllocs &&
          root.bytes + root.leftOutBytes === totalBytes,
          'lo que queda mas lo apartado es lo medido (' + scope + ', '
          + lang + ')');
  }
}

// "mio" y "externo" son COMPLEMENTARIOS: entre los dos esta todo lo medido y
// no comparten ni una reserva.  Sin esta propiedad, "solo lo externo" seria un
// filtro cualquiera en vez del reverso exacto del anterior, y quien mirase los
// dos no sabria si lo que no ve en ninguno existe o se perdio.
{
  const mine = ctx.treeFor('td', 'stack', 'hide');
  const theirs = ctx.treeFor('td', 'stack', 'extern');
  check(mine.allocs + theirs.allocs === totalAllocs,
        'lo mio y lo externo suman exactamente lo medido');
}

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

if (failures === 0) {
  console.log('  TODO OK');
  process.exit(0);
}
console.log('  ' + failures + ' COMPROBACIONES FALLIDAS');
process.exit(1);
