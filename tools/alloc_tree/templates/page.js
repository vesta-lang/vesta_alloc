/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 *
 * The grid, and the panel that follows the selection.
 *
 * TWO RULES SHAPE THIS FILE.
 *
 * The information travels WITH the row.  What a branch was for -- its declared
 * purposes -- is a column, not a table somewhere else: "who allocated this"
 * and "what for" are the same question asked twice, and answering them in two
 * places that cannot be lined up answers neither.
 *
 * And the selection drives everything.  A panel of totals that ignores what
 * you have selected is a second report that happens to share a page; the one
 * below shows the branch you are standing on, down to the raw rows it was
 * folded from.
 */
'use strict';

var unfolded = new Set();   // by IDENTITY: sorting reorders paths
var dir = 'td', grouping = 'stack', byBytes = false, needle = '';
// 'all', 'fold' o 'hide' -- ver `ownChain` en tree.js.  Arranca en 'all'
// porque una pagina que al abrirse esconde la mayor parte de la memoria se lee
// igual que un programa que casi no reserva.
var scope = 'all';
// 'all', 'c' o 'cpp'.  EJE APARTE del anterior a proposito: "es mio" y "es C o
// C++" son dos preguntas distintas, y meterlas en una sola lista dejaria sin
// poder pedir las combinaciones utiles -- mi codigo en C, lo externo en C++.
var langFilter = 'all';
var sortKey = 'total', sortDesc = true, picked = null, rows = [];

/* `esc`, `num`, `human`, `codeHtml` and `langOf` come from `code.js`; `T` from
 * `i18n.js`; the tree and `purposesOf` from `tree.js`; `detail` from
 * `detail.js`.  All loaded before this one. */
function root() { return treeFor(dir, grouping, scope, langFilter); }
function weight(n) { return byBytes ? n.bytes : n.allocs; }

var SORTS = {
  name: function (a, b) { return a.fn < b.fn ? -1 : a.fn > b.fn ? 1 : 0; },
  total: function (a, b) { return weight(a) - weight(b); },
  pct: function (a, b) { return weight(a) - weight(b); },
  self: function (a, b) { return a.own - b.own; },
  bytes: function (a, b) { return a.bytes - b.bytes; },
  selfbytes: function (a, b) { return a.ownb - b.ownb; },
  large: function (a, b) { return a.large - b.large; },
  classes: function (a, b) { return classCount(a) - classCount(b); },
  purpose: function (a, b) {
    var x = purposesOf(a)[0] || '', y = purposesOf(b)[0] || '';
    return x < y ? -1 : x > y ? 1 : 0;
  },
  sites: function (a, b) { return a.nsites - b.nsites; },
  module: function (a, b) { return a.mod < b.mod ? -1 : a.mod > b.mod ? 1 : 0; },
  file: function (a, b) { return a.file < b.file ? -1 : a.file > b.file ? 1 : 0; },
  addr: function (a, b) { return a.sites.length - b.sites.length; }
};

function kidsOf(node) {
  var out = node.kids.filter(function (k) { return k.keep; });
  var cmp = SORTS[sortKey] || SORTS.total;
  out.sort(function (a, b) { return sortDesc ? cmp(b, a) : cmp(a, b); });
  return out;
}

function flatten(node, depth, out) {
  var kids = kidsOf(node);
  for (var i = 0; i < kids.length; i++) {
    out.push({ node: kids[i], depth: depth, kids: keptKids(kids[i]) });
    if (unfolded.has(kids[i])) flatten(kids[i], depth + 1, out);
  }
}

function highlight(text) {
  var safe = esc(text);
  if (!needle) return safe;
  var at = safe.toLowerCase().indexOf(needle);
  if (at < 0) return safe;
  return safe.slice(0, at) + '<span class="hit">' +
         safe.slice(at, at + needle.length) + '</span>' + safe.slice(at + needle.length);
}

/**
 * A number with the bar of its share behind it, and what it INHERITED beside
 * it.  The upper bound never merges into the total: a site that evicted a
 * weaker entry took its counts with it, so `allocs` is a floor and
 * `allocs+over` a ceiling.  Folding them would give one number that looks
 * exact and is neither.
 */
function bar(value, total, self, over, fmt) {
  var pct = total ? Math.min(100, 100 * value / total) : 0;
  var show = fmt ? fmt : num;
  return '<td class="n bar' + (self ? ' self' : '') + '">' +
         (value ? '<i style="width:' + pct.toFixed(2) + '%"></i>' : '') +
         '<span>' + (value ? show(value) : '') +
         (over ? '<em title="upper bound: inherited from an evicted entry">+' +
                 show(over) + '</em>' : '') +
         '</span></td>';
}

function rowHtml(e) {
  var n = e.node, top = weight(root()) || 1;
  var lang = langOf(n.file, n.fn);
  var tog = e.kids
      ? '<span class="tog">' + (unfolded.has(n) ? '&#9662;' : '&#9656;') + '</span>'
      : '<span class="tog leaf">&#9656;</span>';
  /* Every purpose, not the first one and a count: the whole point of the tag
   * is to say what the memory was for, and "+2" says nothing. */
  var purposes = purposesOf(n).map(function (t) {
    return '<span class="tag">' + esc(t) + '</span>';
  }).join('');
  var offs = n.sites.map(function (s) { return DATA.sites[s].off; }).join(' ');
  var classes = classCount(n);
  return '<tr data-i="' + e.i + '"' + (picked === n ? ' class="sel"' : '') + '>' +
    '<td class="name"><span class="tw" style="padding-left:' + (e.depth * 14) + 'px">' +
      tog + (n.inlined ? '<span class="inl">&#8618; </span>' : '') +
      '<span class="sym">' + codeHtml(n.fn, needle, lang) + '</span>' +
      '</span></td>' +
    bar(n.allocs, root().allocs, false, n.over) +
    '<td class="n">' + (100 * weight(n) / top).toFixed(2) + '%</td>' +
    bar(n.own, root().allocs, true) +
    bar(n.bytes, root().bytes, false, n.overb, human) +
    '<td class="n">' + (n.ownb ? human(n.ownb) : '') + '</td>' +
    '<td class="n">' + (n.large || '') + '</td>' +
    '<td class="n" title="distinct size classes touched by this branch">' +
      (classes || '') + '</td>' +
    '<td>' + purposes + '</td>' +
    '<td class="n">' + (n.nsites || '') + '</td>' +
    '<td class="' + (lang ? 'lang-' + lang : '') + '"><span class="path">' +
      paint(n.mod || '', '', needle) + '</span></td>' +
    '<td class="dimmed"><span class="path">' +
      paint(n.file ? n.file + (n.line ? ':' + n.line : '') : '', '', needle) +
      '</span></td>' +
    '<td class="n dimmed"><span class="off">' + esc(offs) +
    '</span></td></tr>';
}

/// The view, into the address bar, so a finding can be sent to someone.
function shareState() {
  writeHash({ grouping: grouping, dir: dir, needle: needle, byBytes: byBytes,
              scope: scope, langFilter: langFilter,
              path: picked ? pathOf(picked) : null });
}

function render() {
  markTree(root(), needle);
  shareState();
  rows = [];
  flatten(root(), 0, rows);
  for (var i = 0; i < rows.length; i++) rows[i].i = i;
  document.getElementById('rows').innerHTML = rows.length
      ? rows.map(rowHtml).join('')
      : '<tr><td colspan="13" class="empty">' +
        esc(T('empty', { needle: needle })) + '</td></tr>';
  var stat = T('stat', {
    rows: num(rows.length), nodes: num(countNodes(root())),
    allocs: num(root().allocs), bytes: human(root().bytes)
  });
  /* Y LO QUE EL FILTRO DEJA FUERA, siempre que deje algo.  Un arbol que se
   * encoge sin decirlo se lee igual que un programa que reserva poco, y quien
   * mira no tiene forma de distinguir las dos cosas.  Va pegado a la misma
   * linea de totales para que no haya que ir a buscarlo. */
  if (root().leftOut)
    stat += '  |  ' + T('scope.left_out', {
      allocs: num(root().leftOut), bytes: human(root().leftOutBytes)
    });
  /* Y CUANTAS DE ESAS SE FUERON POR NO SABERSE SU LENGUAJE, que es otra cosa
   * que ser del otro.  Sin esto, una vista de "solo C" vacia se lee como "este
   * programa no tiene C", cuando lo que falta es la informacion para decirlo
   * -- y el mensaje dice ademas como conseguirla. */
  if (root().noLang)
    stat += '  |  ' + T('lang.no_info', { allocs: num(root().noLang) });
  document.getElementById('stat').textContent = stat;
  document.querySelectorAll('#grid th').forEach(function (th) {
    var d = th.querySelector('.dir');
    if (d) d.innerHTML = th.dataset.k === sortKey ? (sortDesc ? '&#9662;' : '&#9652;') : '';
  });
  detail(picked || root());
}

/// Unfolds the heaviest path: the first question anyone asks of a tree, and a
/// dozen clicks by hand.
function hotPath() {
  var node = root(), top = weight(node) || 1;
  while (node.kids.length) {
    var best = node.kids.reduce(function (a, b) { return weight(b) > weight(a) ? b : a; });
    if (100 * weight(best) / top < 1) break;
    unfolded.add(best);
    node = best;
  }
}

function expandAll(node, depth) {
  if (depth > 40) return;
  node.kids.forEach(function (k) {
    if (k.keep) { unfolded.add(k); expandAll(k, depth + 1); }
  });
}

function flip(what, value) {
  if (what === 'dir') dir = value;
  else if (what === 'scope') scope = value;
  else if (what === 'lang') langFilter = value;
  else grouping = value;
  unfolded.clear();
  picked = null;
  document.querySelectorAll('[data-dir]').forEach(function (b) {
    b.classList.toggle('on', b.dataset.dir === dir);
  });
  hotPath();
  render();
}

function boot() {
  document.getElementById('rows').addEventListener('click', function (ev) {
    var tr = ev.target.closest('tr[data-i]');
    if (!tr) return;
    var node = rows[+tr.dataset.i].node;
    if (ev.target.classList.contains('tog')) {
      if (unfolded.has(node)) unfolded.delete(node); else unfolded.add(node);
    } else {
      picked = node;
    }
    render();
  });
  document.querySelectorAll('#grid th').forEach(function (th) {
    th.insertAdjacentHTML('beforeend', ' <span class="dir"></span>');
    th.addEventListener('click', function () {
      if (sortKey === th.dataset.k) sortDesc = !sortDesc;
      else { sortKey = th.dataset.k; sortDesc = true; }
      render();
    });
  });
  document.querySelectorAll('[data-dir]').forEach(function (b) {
    b.onclick = function () { flip('dir', b.dataset.dir); };
  });
  document.getElementById('grouping').onchange = function (e) {
    flip('group', e.target.value);
  };
  document.getElementById('scope').onchange = function (e) {
    flip('scope', e.target.value);
  };
  document.getElementById('langfilter').onchange = function (e) {
    flip('lang', e.target.value);
  };
  document.getElementById('bybytes').onchange = function (e) {
    byBytes = e.target.checked;
    render();
  };
  document.getElementById('filter').addEventListener('input', function (e) {
    needle = e.target.value.trim().toLowerCase();
    markTree(root(), needle);
    if (needle) expandAll(root(), 0);
    render();
  });
  document.getElementById('expand').onclick = function () { hotPath(); render(); };
  document.getElementById('collapse').onclick = function () {
    unfolded.clear();
    render();
  };
  document.querySelectorAll('.tabs button').forEach(function (b) {
    b.onclick = function () {
      document.querySelectorAll('.tabs button').forEach(function (o) {
        o.classList.remove('on');
      });
      document.querySelectorAll('.panel').forEach(function (p) {
        p.classList.remove('on');
      });
      b.classList.add('on');
      document.getElementById(b.dataset.panel).classList.add('on');
    };
  });
  document.querySelectorAll('table.flat').forEach(wireTable);
  document.querySelectorAll('input[data-for-table]').forEach(wireFilter);

  loadLang();
  var pickLang = document.getElementById('lang');
  pickLang.value = LANG;
  pickLang.onchange = function (e) {
    saveLang(e.target.value);
    applyLang();
    render();      // the status line and the panel are built here, not marked
  };
  applyLang();

  /* A link decides the view before anything is drawn.  The selection is
   * looked up in the tree the link asks for, not the default one, and a path
   * that is not in it selects NOTHING -- a link made while grouping by module
   * means nothing on the plain call stack, and picking the wrong row would be
   * worse than picking none. */
  var link = readHash();
  if (link.grouping) grouping = link.grouping;
  if (link.scope) scope = link.scope;
  if (link.langFilter) langFilter = link.langFilter;
  if (link.dir) dir = link.dir;
  if (link.byBytes) byBytes = true;
  if (link.needle) {
    needle = link.needle;
    document.getElementById('filter').value = needle;
  }
  document.getElementById('grouping').value = grouping;
  document.getElementById('scope').value = scope;
  document.getElementById('langfilter').value = langFilter;
  document.querySelectorAll('[data-dir]').forEach(function (b) {
    b.classList.toggle('on', b.dataset.dir === dir);
  });
  markTree(root(), needle);
  if (link.path) {
    picked = nodeAt(root(), link.path);
    // Unfold down to it, or the link would land on a row nobody can see.
    for (var n = picked; n && n.parent; n = n.parent) unfolded.add(n);
    if (picked) unfolded.delete(picked);
  }
  if (needle) expandAll(root(), 0);

  if (!picked && !needle) hotPath();
  render();
}

boot();
