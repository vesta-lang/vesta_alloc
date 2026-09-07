/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 *
 * Folding the measurement into a tree.  Nothing in here touches the document:
 * it takes `DATA.sites` and `DATA.frames` and returns nodes.
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
    return DATA.frames[chain[0]][F_LIB] ? chain : null;
  }

  // Another module entirely -- the C runtime, a system library.  This one is
  // not the heuristic talking: it is a measured fact from the export.
  if (site.foreign) return null;
  if (!chain.length) return chain;
  if (scope === 'hide')
    return DATA.frames[chain[0]][F_LIB] ? null : chain;
  for (var i = 0; i < chain.length; i++)
    if (!DATA.frames[chain[i]][F_LIB]) return chain.slice(i);
  // All the way out and still library code: there is nobody to hang it on.
  // Inventing a parent would put the allocation under a function that never
  // asked for it.
  return null;
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
    var f = DATA.frames[chain[i]];
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
           sites: [], tags: {}, kidsBy: {}, kids: [], parent: null,
           keep: true, selfHit: false };
}

/// The extra level that goes ON TOP of the chain, or null for no grouping.
function groupNode(site, chain, how) {
  if (how === 'purpose') {
    return makeNode(site.tag || '(sin declarar)', '', 0, '', 0);
  }
  /* The OUTERMOST frame is the one that exists in the binary, so its module
   * and its file are whose code ASKED -- which is the question.  The innermost
   * would answer `std::string`, which is true and useless. */
  for (var i = chain.length - 1; i >= 0; i--) {
    var f = DATA.frames[chain[i]];
    if (how === 'module' && f[F_MOD]) return makeNode(f[F_MOD], '', 0, f[F_MOD], 0);
    if (how === 'file' && f[F_FILE]) return makeNode(f[F_FILE], f[F_FILE], 0, f[F_MOD], 0);
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
}

/// The size split of a branch, biggest bucket first, with its bound.
function sizesOf(node) {
  var out = [];
  for (var i = 0; i < DATA.buckets.length; i++) {
    var b = DATA.buckets[i][0];
    if (node.sizes[b]) out.push({ b: b, upper: DATA.buckets[i][1],
                                  n: node.sizes[b] });
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
  for (var s = 0; s < DATA.sites.length; s++) {
    var site = DATA.sites[s];
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
      var f = DATA.frames[chain[dir === 'td' ? chain.length - 1 - k : k]];
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
