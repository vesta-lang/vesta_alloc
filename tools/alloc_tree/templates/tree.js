/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 *
 * Folding the measurement into a tree.  Nothing in here touches the document:
 * it takes `VIEW.sites` and `VIEW.frames` and returns nodes.
 *
 * WHY IT IS BUILT HERE AND NOT IN THE GENERATOR.  Because there is more than
 * one tree.  The same sites read from the binary inwards answer "where does
 * this allocation come from"; read from the allocation outwards they answer
 * "who ends up calling this the most"; and grouped first by module, by source
 * file or by declared purpose they answer "whose memory is this" -- which is
 * the question you actually arrive with.  Shipping one precooked tree means
 * the other five are not available; shipping all of them means writing every
 * function name into the file a dozen times over.
 *
 * WHAT A NODE IS KEYED BY: function and file, NEVER the line.  The line is
 * where the CALL is, so it differs between two sites inside the same function
 * -- and with it in the key the same function came out as several sibling
 * rows with the same name.  That is not a tree of functions.
 */
'use strict';

var F_FN = 0, F_FILE = 1, F_LINE = 2, F_MOD = 3, F_INLINED = 4, F_LIB = 5;
/* Las dos puntas: el aparato de medida por dentro y la entrada del sistema al
 * hilo por fuera.  Ver `ours.py`, que es donde se decide y por que. */
var F_INSTR = 6, F_START = 7;

/// Ni biblioteca, ni instrumento: los marcos por los que el filtro se para.
function isMine(f) { return !f[F_LIB] && !f[F_INSTR] && !f[F_START]; }

/*
 * WHICH POPULATION IS ON SCREEN.  An export can carry two, and they are not
 * two halves of one measurement: the allocator's covers every block through
 * ONE return address, and the checker's covers what fits its shadow through a
 * WALKED stack.  Adding them up would produce a total that is neither.
 *
 * So they are drawn one at a time, by the same code.  Everything below reads
 * `VIEW` and not `DATA`, which is what keeps a second renderer -- and the
 * drift that comes with it -- from ever being needed.
 */
/* \~english AND THE CHECKER'S IS THE ONE THAT OPENS, when the export carried
 * one.  Not because it is more complete -- it is not, it covers what its
 * shadow reached -- but because it is the one that ANSWERS: the allocator's
 * keeps one return address per site, so an allocation made inside a shared
 * out-of-line body names the container and never its owner.  Opening on the
 * population that cannot answer the first question a reader has means the
 * answer is behind a control most readers will not touch.
 *
 * The other one is one click away and the control says what each is, so
 * nothing is hidden -- what changes is which of the two has to be asked for.
 *
 * \~spanish Y LA DEL COMPROBADOR ES LA QUE ABRE, cuando el volcado trajo una.
 * No porque sea mas completa -- no lo es, cubre lo que su sombreado alcanzo --
 * sino porque es la que CONTESTA: la del asignador guarda una direccion de
 * retorno por sitio, asi que una reserva hecha dentro de un cuerpo compartido
 * fuera de linea nombra al contenedor y jamas a su dueno.  Abrir en la
 * poblacion que no puede contestar la primera pregunta que trae quien lee es
 * dejar la respuesta detras de un control que casi nadie va a tocar.
 *
 * La otra esta a un clic y el control dice lo que es cada una, asi que no se
 * esconde nada -- lo que cambia es cual de las dos hay que pedir.  \~ */
var VIEW = DATA.check || DATA;

/**
 * Switch population.  The cache is dropped rather than keyed by dataset: the
 * switch is a deliberate act a reader does a handful of times, while the key
 * would be paid on every redraw, and a stale tree from the other population is
 * the one mistake this must not make.
 */
function setView(which) {
  VIEW = (which === 'check' && DATA.check) ? DATA.check : DATA;
  CACHE = {};
}

/**
 * The chain to show for a site under the "only my own calls" filter, or null
 * to leave the site out entirely.
 *
 * `scope` is 'all', 'fold' or 'hide', and the two that filter are two honest
 * readings of the same question:
 *
 *   fold  an allocation made THROUGH the library is still the program's: walk
 *         OUT to the first frame its author wrote and hang it there.  Nothing
 *         is lost, it only moves.
 *   hide  drop it unless it already starts in the program's own code.  Blunter,
 *         and this one does lose allocations -- which is why what it leaves out
 *         is counted and shown.
 *
 * WHAT IS LIBRARY CODE IS NOT DECIDED HERE.  It arrives as a bit per frame,
 * worked out when the page was written (`ours.py`), so the rule -- names,
 * header paths -- lives in one place with its reasons instead of being copied
 * into JavaScript where it would quietly drift from the other copy.
 */
function ownChain(site, chain, scope) {
  if (scope === 'all') return chain;

  /* "extern" is the exact opposite question: show me what I did NOT write.
   * Another module, or library code with nobody of mine behind it -- which is
   * precisely what the two modes above throw away, so the two views together
   * cover everything measured and overlap in nothing. */
  if (scope === 'extern') {
    if (site.foreign) return chain;
    if (!chain.length) return null;
    return isMine(VIEW.frames[chain[0]]) ? null : chain;
  }

  // Another module entirely -- the C runtime, a system library.  This one is
  // not the heuristic talking: it is a measured fact from the export.
  if (site.foreign) return null;
  if (!chain.length) return chain;
  if (scope === 'hide')
    return isMine(VIEW.frames[chain[0]]) ? chain : null;

  /* PLEGAR LAS DOS PUNTAS, no solo la de dentro.
   *
   * Hasta aqui se salia hacia fuera hasta el primer marco del autor, que
   * resuelve el extremo interior -- la biblioteca, y ahora tambien el propio
   * asignador, que el comprobador pone en las 9.542 pilas por igual.  El
   * extremo EXTERIOR tenia el mismo problema por el otro lado: de fuera hacia
   * dentro toda cadena arranca con siete marcos de como el sistema entro en el
   * hilo, asi que el arbol de arriba abajo tampoco se separaba hasta el
   * septimo nivel.
   *
   * Se recorta, no se borra: 'todo' sigue ensenando la cadena entera, que es
   * la medida.  Esto es una forma de MIRAR. */
  var lo = 0, hi = chain.length;
  while (lo < hi && !isMine(VIEW.frames[chain[lo]])) lo++;
  while (hi > lo && !isMine(VIEW.frames[chain[hi - 1]])) hi--;
  // All the way out and still nobody's: there is nothing to hang it on.
  // Inventing a parent would put the allocation under a function that never
  // asked for it.
  if (lo >= hi) return null;
  return chain.slice(lo, hi);
}

/**
 * Whether the site should be shown when only one LANGUAGE is asked for.
 *
 * A SEPARATE AXIS from the scope above, and separate on purpose: "is this mine"
 * and "is this C or C++" are different questions, and folding them into one
 * list of choices would make the useful combinations -- my own C code, external
 * C++ -- impossible to ask for.
 *
 * Decided on the INNERMOST frame of the chain being shown, which is where the
 * allocation physically happens.  Asking "is any frame C" would answer yes for
 * nearly everything in a program that mixes the two, which is no answer.
 *
 * `langOf` is the same one that paints the names, from `code.js`.  A frame it
 * cannot place -- a bare name with no file, which is most of a stripped build
 * -- belongs to no language, and asking for a specific one leaves it out: it is
 * not being claimed for either side.
 */
function langOfChain(chain) {
  /* EL PRIMERO DEL QUE SE SEPA ALGO, saliendo hacia fuera.  Lo natural seria
   * mirar solo el marco de dentro, y estaria mal en un caso que se da mucho:
   * una funcion inlineada de la que no hay fichero, llamada desde una que si
   * lo tiene.  Rendirse en el primero contestaria "no se" teniendo la
   * respuesta un paso mas alla.
   *
   * Un fichero `.c` es C con certeza y un `.cpp` es C++ con certeza; eso lo
   * decide `langOf`, la misma que pinta los nombres. */
  for (var i = 0; i < chain.length; i++) {
    var f = VIEW.frames[chain[i]];
    var l = langOf(f[F_FILE], f[F_FN]);
    if (l) return l;
  }
  return '';
}

/**
 * Whether the site should be shown when only one LANGUAGE is asked for.
 *
 * `want` is 'all', 'c', 'cpp' or 'unknown'.  THAT LAST ONE IS NOT FILLER: with
 * no debug information there is no file, and without a file the language of a
 * plain name cannot be told -- a stripped build has hundreds of those.  Folding
 * them into "not C" would make an empty "C only" view look like a program with
 * no C in it, when what is missing is the information to say.  So they are a
 * choice of their own, and the page says how many there are.
 */
function langMatches(chain, want) {
  if (want === 'all') return true;
  if (!chain.length) return want === 'unknown';
  return langOfChain(chain) === (want === 'unknown' ? '' : want);
}

/**
 * What separates the two halves of a child's key.
 *
 * It has to be a character that cannot occur in a symbol or in a path, or a
 * function `a` in file `b/c` and a function `a/b` in file `c` would collapse
 * onto the same node.  A space will not do: a demangled C++ name is full of
 * them.
 *
 * BUILT, never written as the character itself.  A literal NUL inside a
 * string is perfectly good JavaScript and works, which is exactly the trap:
 * `grep`, `diff` and half the tooling then decide the file is binary and stop
 * showing it.  That happened to this very file.
 */
var KEY_SEP = String.fromCharCode(0);

function makeNode(fn, file, line, mod, inlined) {
  return { fn: fn, file: file, line: line, mod: mod, inlined: inlined,
           allocs: 0, bytes: 0, own: 0, ownb: 0, over: 0, overb: 0,
           // Allocations too big for the slab, and WHICH size classes the
           // branch touched.  The classes travel as bits and not as a count
           // because counts cannot be added up: two sites using the same class
           // would come out as two.  ORed, then counted.
           large: 0, maskLo: 0, maskHi: 0,
           // How many sites are UNDER this node.  Counted while folding, not
           // by walking the subtree later: the column is asked for once per
           // visible row on every redraw, and a walk per row turns a redraw
           // into a crawl on a tree this size.
           nsites: 0,
           // bucket -> allocations, summed up the tree.  This is what lets a
           // BRANCH answer "many small or a few large": the process-wide
           // histogram cannot be handed back out to the sites that formed it.
           sizes: {},
           // bucket -> BYTES, summed the same way.  Apart from the counts and
           // never derived from them: a bucket holds a range, so a count gives
           // an interval and the last bucket -- the one that decides the peak
           // -- has no ceiling to bound it with at all.
           sizeBytes: {},
           sites: [], tags: {}, kidsBy: {}, kids: [], parent: null,
           keep: true, selfHit: false };
}

/// The extra level that goes ON TOP of the chain, or null for no grouping.
function groupNode(site, chain, how) {
  if (how === 'purpose') {
    return makeNode(site.tag || '(sin declarar)', '', 0, '', 0);
  }
  /* \~english THE FIRST FRAME THE AUTHOR WROTE, walking OUT of the library
   * code -- the same rule `timeOwnFrame` and the query mode follow, so the
   * three views name the same site the same way.
   *
   * IT USED TO BE THE OUTERMOST ONE, and that was wrong in a way that only
   * showed up with a deep stack walk: the further out you go, the more that
   * frame looks like how the system entered the thread, which is the same for
   * the whole program and tells nothing apart.  Measured on a compile of
   * 144.000 lines with eight frames walked, 7.536 of 8.611 sites changed
   * module -- 61% of them landing under `KERNEL32.DLL`, `ntdll.dll` or the C
   * runtime's start-up.  Grouping by module answered, and said nothing.
   *
   * With no frame of the author's, the innermost is used: a stack that is
   * genuinely all library code belongs to that library, not to the thread's
   * entry point.
   *
   * \~spanish EL PRIMER MARCO QUE ESCRIBIO EL AUTOR, saliendo de la libreria
   * hacia fuera -- la misma regla que siguen `timeOwnFrame` y el modo consulta,
   * para que las tres vistas nombren igual al mismo sitio.
   *
   * ERA EL DE MAS AFUERA, y estaba mal de una forma que solo se veia con un
   * recorrido de pila profundo: cuanto mas lejos se va, mas se parece ese marco
   * a como entro el sistema en el hilo, que es el mismo para todo el programa y
   * no distingue nada.  Medido en una compilacion de 144.000 lineas con ocho
   * marcos recorridos, 7.536 de 8.611 sitios cambiaban de modulo -- el 61% de
   * ellos cayendo bajo `KERNEL32.DLL`, `ntdll.dll` o el arranque del CRT.
   * Agrupar por modulo contestaba, y no decia nada.
   *
   * Sin ningun marco del autor se usa el de mas adentro: una pila que de verdad
   * es toda libreria es de esa libreria, no del arranque del hilo.  \~ */
  var owner = null;
  for (var i = 0; i < chain.length; i++) {
    var f = VIEW.frames[chain[i]];
    if (isMine(f)) { owner = f; break; }
  }
  if (!owner && chain.length) owner = VIEW.frames[chain[0]];
  if (owner) {
    if (how === 'module' && owner[F_MOD])
      return makeNode(owner[F_MOD], '', 0, owner[F_MOD], 0);
    if (how === 'file' && owner[F_FILE])
      return makeNode(owner[F_FILE], owner[F_FILE], 0, owner[F_MOD], 0);
  }
  return makeNode(how === 'module' ? '(sin clasificar)' : '(sin fichero)',
                  '', 0, '', 0);
}

/// The child for this frame, made if it is not there yet.  See `KEY_SEP` for
/// why the key is joined the way it is.
function childOf(node, fn, file, line, mod, inlined) {
  var key = fn + KEY_SEP + file;
  var kid = node.kidsBy[key];
  if (kid === undefined) {
    kid = makeNode(fn, file, line, mod, inlined);
    /* Upwards too, so a selected row can say WHERE it is -- which is what a
     * shareable link needs.  An index would change the moment somebody sorts
     * by another column; a path of names does not. */
    kid.parent = node;
    node.kidsBy[key] = kid;
    node.kids.push(kid);
  }
  return kid;
}

function addTag(node, tag, n) {
  node.tags[tag] = (node.tags[tag] || 0) + n;
}

/**
 * Everything a site adds to a node it passes through.  In ONE place because
 * it is applied three times per site -- to the root, to the grouping level and
 * to every frame of the chain -- and three copies of a list of counters is
 * three chances for one of them to be forgotten in a way nothing reports.
 */
function accum(node, site) {
  node.allocs += site.allocs;
  node.bytes += site.bytes;
  node.over += site.over;
  node.overb += site.overBytes;
  node.large += site.large;
  node.maskLo |= site.mask[0];
  node.maskHi |= site.mask[1];
  node.nsites++;
  addTag(node, site.tag, site.allocs);
  for (var b in site.sizes)
    node.sizes[b] = (node.sizes[b] || 0) + site.sizes[b];
  for (var bb in site.sizeBytes)
    node.sizeBytes[bb] = (node.sizeBytes[bb] || 0) + site.sizeBytes[bb];
}

/**
 * The size split of a branch, with its bound, counted AND weighed.
 *
 * The two together and not one or the other, because they answer opposite
 * questions about the same rows and the reader switches between them: by count
 * the last bucket of a real run is thirty-three out of ninety million, and by
 * bytes it is an eighth of everything the program asked for.
 *
 * `bytes` is zero when the export has no such column, which is what an older
 * one looks like.  The caller checks `VIEW.hasSizeBytes` before offering the
 * view: a bar of zero and "there is no measurement" must not look the same.
 */
function sizesOf(node) {
  var out = [];
  for (var i = 0; i < VIEW.buckets.length; i++) {
    var b = VIEW.buckets[i][0];
    if (node.sizes[b]) out.push({ b: b, upper: VIEW.buckets[i][1],
                                  n: node.sizes[b],
                                  bytes: node.sizeBytes[b] || 0 });
  }
  return out;
}

/// How many DISTINCT size classes this branch touched.  `>>>` and not `>>`:
/// the top bit of each half would make the number negative and the loop would
/// never end.
function classCount(node) {
  var n = 0, m;
  for (m = node.maskLo >>> 0; m !== 0; m >>>= 1) n += m & 1;
  for (m = node.maskHi >>> 0; m !== 0; m >>>= 1) n += m & 1;
  return n;
}

var CACHE = {};

/**
 * The tree for one (direction, grouping).  Memoised: folding 475 sites is
 * fast, but it is asked for on every redraw and there is no reason to.
 */
function treeFor(dir, grouping, scope, lang) {
  scope = scope || 'all';
  lang = lang || 'all';
  var key = dir + '|' + grouping + '|' + scope + '|' + lang;
  if (CACHE[key]) return CACHE[key];

  var root = makeNode('(everything)', '', 0, '', 0);
  // What the filter left out.  Kept on the root and always shown, because a
  // tree that quietly shrinks reads exactly like a program that allocates
  // little -- and there would be no way to tell the two apart.
  root.leftOut = 0;
  root.leftOutBytes = 0;
  /* Y DE ESAS, cuantas se quedaron fuera porque NO SE SABE su lenguaje, que es
   * muy distinto de que sean del otro.  Sin informacion de depuracion no hay
   * fichero, y sin fichero un nombre a secas no dice en que lenguaje se
   * escribio: una vista de "solo C" vacia se leeria como "aqui no hay C"
   * cuando lo que falta es con que saberlo. */
  root.noLang = 0;
  /* \~english WHAT THE REPORT SPENT ON ITSELF, out of the tree at every scope
   * and counted here.  This is NOT the scope filter: folding the standard
   * library away is a way of LOOKING and the reader decides it, while the
   * measuring apparatus is simply not part of what it measures.  Leaving it in
   * made it the biggest branch of the tree -- 1.253 MB of 1.961 on a 24k-line
   * compile, 63,9 % of the bytes, more than the program -- so the first thing
   * anybody saw was the report reading DWARF to write the report.
   *
   * Out, and SAID: the figure is on the status line.  Quantifiable and
   * subtractable is the rule; declared away is not.
   *
   * \~spanish LO QUE EL INFORME SE GASTO EN SI MISMO, fuera del arbol en todos
   * los alcances y contado aqui.  Esto NO es el filtro de alcance: plegar la
   * biblioteca estandar es una forma de MIRAR y la decide quien lee, mientras
   * que el aparato de medida sencillamente no es parte de lo que mide.
   * Dejarlo dentro lo convertia en la rama mas grande del arbol -- 1.253 MB de
   * 1.961 en una compilacion de 24k lineas, el 63,9 % de los bytes, mas que el
   * programa --, asi que lo primero que veia cualquiera era el informe leyendo
   * DWARF para escribir el informe.
   *
   * Fuera, y DICHO: la cifra va en la linea de estado.  La regla es
   * cuantificable y restable; declarado inexistente no.  \~ */
  root.instr = 0;
  root.instrBytes = 0;
  for (var s = 0; s < VIEW.sites.length; s++) {
    var site = VIEW.sites[s];
    if (site.instr) {
      root.instr += site.allocs;
      root.instrBytes += site.bytes;
      continue;
    }
    var chain = ownChain(site, site.chain, scope);
    /* El idioma se mira SOBRE lo que el ambito dejo, no sobre la cadena
     * entera: si el ambito plego hacia fuera, la funcion que reserva es otra,
     * y preguntarle el lenguaje a la de antes contestaria por una que ya no se
     * ensena. */
    if (chain === null || !langMatches(chain, lang)) {
      root.leftOut += site.allocs;
      root.leftOutBytes += site.bytes;
      if (chain !== null && lang !== 'all' && !langOfChain(chain))
        root.noLang += site.allocs;
      continue;
    }
    var node = root;

    accum(root, site);

    if (grouping !== 'stack') {
      var head = groupNode(site, chain, grouping);
      node = childOf(node, head.fn, head.file, 0, head.mod, 0);
      accum(node, site);
    }

    for (var k = 0; k < chain.length; k++) {
      // `chain` is innermost first; top-down walks it backwards.
      var f = VIEW.frames[chain[dir === 'td' ? chain.length - 1 - k : k]];
      node = childOf(node, f[F_FN], f[F_FILE], f[F_LINE], f[F_MOD], f[F_INLINED]);
      accum(node, site);
    }
    node.own += site.allocs;
    node.ownb += site.bytes;
    node.sites.push(s);
  }
  CACHE[key] = root;
  return root;
}

/**
 * Marks the node and every ancestor of a match.  A branch is kept when the
 * needle is anywhere INSIDE it: hiding a parent whose child matches would cut
 * the path to the match, which is the one thing being looked for.
 */
function markTree(node, needle) {
  var hit = !needle;
  if (needle) {
    var purposes = Object.keys(node.tags).join(' ');
    hit = (node.fn + ' ' + node.file + ' ' + node.mod + ' ' + purposes)
              .toLowerCase().indexOf(needle) >= 0;
  }
  var deep = false;
  for (var i = 0; i < node.kids.length; i++)
    if (markTree(node.kids[i], needle)) deep = true;
  node.keep = hit || deep;
  node.selfHit = hit;
  return node.keep;
}

/// How many children a row would show if it were unfolded.  Counted, never
/// obtained by sorting: this is asked once per visible row.
function keptKids(node) {
  var n = 0;
  for (var i = 0; i < node.kids.length; i++) if (node.kids[i].keep) n++;
  return n;
}

/// Every site under this node, not only the ones that END in it.  This is what
/// makes a folded row answerable: what is in the branch you are looking at.
function sitesUnder(node, out) {
  out = out || [];
  for (var i = 0; i < node.sites.length; i++) out.push(node.sites[i]);
  for (var k = 0; k < node.kids.length; k++) sitesUnder(node.kids[k], out);
  return out;
}

/// The purposes of a branch, heaviest first.  Every one of them: what the
/// memory was for is the question the tag system exists to answer, and a
/// "+2" answers none of it.
function purposesOf(node) {
  return Object.keys(node.tags).sort(function (a, b) {
    return node.tags[b] - node.tags[a];
  });
}

function countNodes(node) {
  var n = node.kids.length;
  for (var i = 0; i < node.kids.length; i++) n += countNodes(node.kids[i]);
  return n;
}

/// The path from the root to this node, as names.  Stable across sorting and
/// across regenerating the report, which an index would not be.
function pathOf(node) {
  var out = [];
  for (var n = node; n && n.parent; n = n.parent) out.unshift(n.fn);
  return out;
}

/// The node at that path, or null if this tree does not have it -- which is
/// normal: a link made while grouping by module means nothing on the plain
/// call stack, and saying nothing is better than selecting the wrong row.
function nodeAt(root, path) {
  var node = root;
  for (var i = 0; i < path.length; i++) {
    var found = null;
    for (var k = 0; k < node.kids.length; k++)
      if (node.kids[k].fn === path[i]) { found = node.kids[k]; break; }
    if (!found) return null;
    node = found;
  }
  return node;
}
