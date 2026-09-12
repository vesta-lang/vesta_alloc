/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 *
 * The panel: the branch you are standing on, and the rows it was folded from.
 *
 * This is what a set of tabs could not do.  Five tables holding the same
 * numbers side by side are five tables you cannot line up with what you have
 * selected -- so the totals in them answer a question nobody asked, about the
 * whole process, while the question in front of you is about the branch.
 *
 * And a summary with no way back to the measurement asks to be taken on
 * faith.  Every site under the selected branch is here in full, with its
 * NAME first: a `site_id` and a `pc_offset` are how the export refers to a
 * place, not what the place is, and a row of digits is nothing anyone can act
 * on.
 */
'use strict';

function detail(node) {
  var ids = sitesUnder(node);
  var head = '<div class="dnums">' +
    '<b class="sym">' + codeHtml(node.fn, '', langOf(node.file, node.fn)) +
    '</b>' +
    '<span>' + esc(T('det.allocations', { n: num(node.allocs) })) +
      (node.over ? ' <em title="' + esc(T('tip.upper')) + '">' +
       esc(T('det.upper', { n: num(node.over) })) + '</em>' : '') +
    '</span>' +
    '<span>' + esc(T('det.requested', { n: human(node.bytes) })) + '</span>' +
    '<span>' + esc(T('det.endhere', { n: num(node.own),
                                      bytes: human(node.ownb) })) + '</span>' +
    '<span>' + esc(T('det.nsites', { n: num(ids.length) })) + '</span>' +
    // Rounded to whole bytes: `285.21469163545567 B` is not more precise, it
    // is the same figure with sixteen digits of noise stapled to it.
    '<span>' + esc(T('det.average', {
      n: node.allocs ? human(Math.round(node.bytes / node.allocs)) : '0 B'
    })) + '</span></div>';

  var total = node.allocs || 1;
  var tags = purposesOf(node).map(function (t) {
    var pct = 100 * node.tags[t] / total;
    return '<div class="pbar"><i style="width:' + pct.toFixed(1) + '%"></i>' +
           '<span>' + esc(t) + '</span><b>' + num(node.tags[t]) + '</b>' +
           '<em>' + pct.toFixed(1) + '%</em></div>';
  }).join('');

  /* THE NAMES GO FIRST, and the raw columns after.  The chain is right there,
   * so the function that contains the site, the function it physically
   * allocates in, and where that is in the source go next to the numbers. */
  var rowsHtml = ids.map(function (s) {
    var site = VIEW.sites[s];
    var outer = VIEW.frames[site.chain[site.chain.length - 1]];
    var inner = VIEW.frames[site.chain[0]];
    var lang = langOf(outer[1], outer[0]);
    var where = inner[1] ? inner[1] + (inner[2] ? ':' + inner[2] : '') : '';
    return '<tr><td class="name"><span class="sym">' +
        codeHtml(outer[0], '', lang) + '</span></td>' +
      '<td class="name"><span class="sym">' +
        (inner === outer ? '' : codeHtml(inner[0], '', langOf(inner[1], inner[0]))) +
      '</span></td>' +
      '<td class="dimmed"><span class="path">' + esc(where) + '</span></td>' +
      '<td class="' + (lang ? 'lang-' + lang : '') + '"><span class="path">' +
        esc(outer[3] || '') + '</span></td>' +
      site.row.map(function (v) { return '<td>' + esc(v) + '</td>'; }).join('') +
      '</tr>';
  }).join('');

  /* The four leading headings are ours and get translated; the ones after are
   * the CSV's own column names and do not -- renaming them would make the
   * page and the file disagree about what a column is called. */
  var heads = [T('det.fn'), T('det.inner'), T('det.source'), T('det.module')]
      .concat(VIEW.siteCols).map(function (c) {
        return '<th>' + esc(c) + '</th>';
      }).join('');

  /* HOW BIG they were, for THIS branch.  The process-wide histogram in the
   * raw appendix answers the same question about the whole run and cannot be
   * handed back out to the branches that formed it, which is why the
   * allocator now records the split per site. */
  var split = sizesOf(node);
  /* POR CUENTA O POR BYTES, y no una de las dos: son lecturas opuestas de las
   * mismas filas.  Por cuenta, la ultima casilla de una rama de verdad son
   * unas pocas reservas entre millones -- una barra de ancho cero -- y por
   * bytes puede ser todo lo que esa rama pidio.
   *
   * Se elige aqui y se recuerda, como el idioma: quien mira por bytes quiere
   * mirar por bytes tambien en la rama siguiente. */
  var byBytes = detailByBytes() && VIEW.hasSizeBytes;
  var szTotal = 0;
  for (var si = 0; si < split.length; si++)
    szTotal += byBytes ? split[si].bytes : split[si].n;
  if (!szTotal) szTotal = 1;
  var sizes = split.length
      ? split.map(function (s) {
          var v = byBytes ? s.bytes : s.n;
          var pct = 100 * v / szTotal;
          return '<div class="pbar"><i style="width:' + pct.toFixed(1) +
                 '%"></i><span>' +
                 (s.upper ? '&le; ' + human(s.upper) : '&gt; 16 MiB') +
                 '</span><b>' + (byBytes ? human(v) : num(v)) + '</b><em>' +
                 pct.toFixed(1) + '%</em></div>';
        }).join('')
      : '<div class="dim">' + esc(T('det.nosizes')) + '</div>';
  /* El control solo si hay las dos medidas.  Una exportacion anterior no trae
   * los bytes por casilla, y ofrecer una vista que saldria a cero seria una
   * promesa que la pagina no puede cumplir. */
  var sizePick = VIEW.hasSizeBytes
      ? '<span class="szpick">' +
        '<a href="#" data-szview="allocs"' +
        (byBytes ? '' : ' class="on"') + '>' + esc(T('sz.allocs')) + '</a>' +
        '<a href="#" data-szview="bytes"' +
        (byBytes ? ' class="on"' : '') + '>' + esc(T('sz.bytes')) + '</a>' +
        '</span>'
      : '';

  document.getElementById('detail').innerHTML = head +
    '<div class="dcols"><div><h2>' + esc(T('det.what')) + '</h2>' + tags +
      '<h2 style="margin-top:10px">' + esc(T('det.sizes')) + sizePick +
      '</h2>' + sizes +
    '</div>' +
    '<div class="dsites"><h2>' +
      esc(T('det.sites', { n: num(ids.length) })) + '</h2>' +
      '<div class="wrap sm"><table class="flat"><thead><tr>' + heads +
      '</tr></thead><tbody>' + rowsHtml + '</tbody></table></div></div></div>';

  /* Y se vuelve a dibujar la MISMA rama al cambiar de vista.  Redibujar el
   * panel entero y no solo las barras: el encabezado dice cual esta elegida, y
   * dos sitios que dicen lo mismo se separan en cuanto uno de los dos cambia. */
  document.querySelectorAll('#detail [data-szview]').forEach(function (a) {
    a.onclick = function (e) {
      e.preventDefault();
      detailSetByBytes(a.dataset.szview === 'bytes');
      detail(node);
    };
  });
}

/// Si el histograma de la rama se dibuja por bytes.  Recordado como el idioma
/// y el tema, y por lo mismo: una eleccion que hay que repetir en cada rama es
/// una eleccion que se deja de hacer.
function detailByBytes() {
  try {
    return window.localStorage.getItem('alloc_tree_detail_bytes') === '1';
  } catch (e) { return false; }
}

function detailSetByBytes(on) {
  try {
    window.localStorage.setItem('alloc_tree_detail_bytes', on ? '1' : '0');
  } catch (e) { /* ventana privada: va igual, solo no lo recuerda */ }
}
