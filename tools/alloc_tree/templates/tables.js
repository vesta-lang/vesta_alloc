/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 *
 * The files of the export, as plain tables.
 *
 * LAS FILAS SON DATOS, NO DOCUMENTO.  Antes el generador escribia cada fila en
 * la pagina y esto solo ordenaba y filtraba lo que ya estaba.  Medido sobre una
 * compilacion de 144.000 lineas, eso costaba 97 MB de los 104 que pesaba el
 * fichero: una fila de `check_frames.csv` ocupa 441 bytes de marcado contra 66
 * de dato, y hay 185.000.  Ahora las filas viajan en `DATA.raw` y se pintan
 * aqui.
 *
 * Y SE PINTAN A TROZOS, segun se baja.  Dos millones de celdas en el documento
 * no solo pesan: el navegador las construye todas antes de ensenar nada.  Se
 * pinta lo que se va a mirar y se sigue pintando al llegar abajo, asi que no se
 * esconde ni una fila -- la cuenta de al lado dice siempre cuantas hay de
 * cuantas.
 *
 * Aparte de la rejilla del arbol a proposito: la rejilla tiene modelo, estado
 * de plegado y forma propia, y estas no tienen nada de eso.
 */
'use strict';

/// Cuantas filas se pintan de una vez, y cuando se pide el siguiente trozo.
var RAW_CHUNK = 300;
var RAW_NEAR = 500;

/// id de tabla -> su estado.  Vive aqui y no en el documento porque el orden y
/// el filtro son del LECTOR, no de los datos.
var RAWS = {};

/**
 * El texto de una celda.
 *
 * Una columna puede venir empaquetada -- las cadenas distintas una vez y un
 * numero por fila -- porque en `check_frames.csv` la ruta del fichero es la
 * misma en miles de filas.  Se deshace aqui, y el resultado es exacto.
 */
function rawCell(t, row, c) {
  var v = row[c];
  var words = t.words[c];
  return words ? words[v] : v;
}

/// Pinta el siguiente trozo de una tabla, o todo si `all`.
function rawPaint(id) {
  var t = RAWS[id];
  if (!t) return;
  var body = t.table.tBodies[0];
  var upto = Math.min(t.order.length, t.shown + RAW_CHUNK);
  var html = '';
  for (var i = t.shown; i < upto; ++i) {
    var row = t.rows[t.order[i]];
    html += '<tr>';
    for (var c = 0; c < t.cols.length; ++c) {
      var text = esc(String(rawCell(t, row, c)));
      /* La misma regla que el arbol: un numero es un numero, y cualquier texto
         se envuelve en vez de estirar su columna por toda la pantalla.  Nada
         se acorta. */
      if (t.cols[c].numeric) html += '<td class="n">' + text + '</td>';
      else if (t.cols[c].mono)
        html += '<td class="name"><span class="sym">' + text + '</span></td>';
      else html += '<td><span class="path">' + text + '</span></td>';
    }
    html += '</tr>';
  }
  body.insertAdjacentHTML('beforeend', html);
  t.shown = upto;
}

/// Vacia y vuelve a empezar: cambio de filtro o de orden.
function rawReset(id) {
  var t = RAWS[id];
  if (!t) return;
  t.table.tBodies[0].innerHTML = '';
  t.shown = 0;
  rawPaint(id);
  rawCount(id);
}

/**
 * Dice cuantas se ven de cuantas hay.
 *
 * Las DOS cifras siempre: una tabla que encoge en silencio se lee como una
 * exportacion mas corta, y ese es justo el modo de fallo que no puede tener una
 * herramienta de medida.
 */
function rawCount(id) {
  var t = RAWS[id];
  var note = document.getElementById('c-' + id);
  if (!t || !note) return;
  note.textContent = t.order.length === t.rows.length
    ? T('raw.rows', { n: t.rows.length.toLocaleString('en-US') })
    : T('raw.some', { n: t.order.length.toLocaleString('en-US'),
                      all: t.rows.length.toLocaleString('en-US') });
}

/// Se queda con las filas que contengan el texto, mirando TODAS sus columnas.
function rawFilter(id, needle) {
  var t = RAWS[id];
  if (!t) return;
  needle = (needle || '').toLowerCase();
  if (!needle) {
    t.order = t.rows.map(function (_, i) { return i; });
  } else {
    t.order = [];
    for (var i = 0; i < t.rows.length; ++i) {
      var row = t.rows[i];
      for (var c = 0; c < t.cols.length; ++c) {
        if (String(rawCell(t, row, c)).toLowerCase().indexOf(needle) >= 0) {
          t.order.push(i);
          break;
        }
      }
    }
  }
  rawSortApply(id);
  rawReset(id);
}

/// Aplica el orden guardado sobre las filas que hayan pasado el filtro.
function rawSortApply(id) {
  var t = RAWS[id];
  if (!t || t.sortBy < 0) return;
  var c = t.sortBy, desc = t.sortDesc, numeric = t.cols[c].numeric;
  t.order.sort(function (a, b) {
    var x = rawCell(t, t.rows[a], c), y = rawCell(t, t.rows[b], c);
    var r;
    if (numeric) {
      r = (parseFloat(String(x).replace(/[^\d.eE+-]/g, '')) || 0) -
          (parseFloat(String(y).replace(/[^\d.eE+-]/g, '')) || 0);
    } else {
      x = String(x); y = String(y);
      r = x < y ? -1 : x > y ? 1 : 0;
    }
    return desc ? -r : r;
  });
}

/// Monta una tabla del volcado: cabeceras que ordenan, y el primer trozo.
function rawWire(spec) {
  var table = document.getElementById('t-' + spec.id);
  if (!table) return;
  var t = RAWS[spec.id] = {
    id: spec.id, cols: spec.cols, rows: spec.rows, words: spec.words,
    table: table, shown: 0, sortBy: -1, sortDesc: false,
    order: spec.rows.map(function (_, i) { return i; })
  };

  Array.prototype.forEach.call(table.tHead.rows[0].cells, function (th, i) {
    th.addEventListener('click', function () {
      t.sortDesc = t.sortBy === i ? !t.sortDesc : true;
      t.sortBy = i;
      rawSortApply(spec.id);
      rawReset(spec.id);
    });
  });

  /* AL LLEGAR ABAJO, MAS.  El contenedor que se desplaza es el de la tabla, no
     la pagina: cada tabla tiene su propio final. */
  var wrap = table.parentNode;
  if (wrap && wrap.addEventListener) {
    wrap.addEventListener('scroll', function () {
      if (t.shown >= t.order.length) return;
      if (wrap.scrollTop + wrap.clientHeight >= wrap.scrollHeight - RAW_NEAR)
        rawPaint(spec.id);
    });
  }

  rawPaint(spec.id);
  rawCount(spec.id);
}

/// Monta todas las tablas del volcado.  Sin `DATA.raw` no hay nada que montar.
function rawInit() {
  (DATA.raw || []).forEach(rawWire);
  document.querySelectorAll('input[data-for-table]').forEach(function (input) {
    var id = (input.dataset.forTable || '').replace(/^t-/, '');
    input.addEventListener('input', function () {
      rawFilter(id, input.value);
    });
  });
}
