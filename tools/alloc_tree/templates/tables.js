/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 *
 * The five files of the export, as plain tables.  The generator already wrote
 * every row into the document; this only sorts and filters what is there, and
 * it never REMOVES a row -- filtering hides, so the count of what is hidden
 * stays available and nothing can quietly disappear from the page.
 *
 * Apart from the tree grid on purpose: the grid has a data model, a fold state
 * and a shape of its own, and these have none of that.  Sorting a table by a
 * column is the same three lines whichever table it is.
 */
'use strict';

/// Sorts the rows in place, numerically when the header says the column is one.
function tableSort(table, index, numeric, desc) {
  var body = table.tBodies[0];
  var list = Array.prototype.slice.call(body.rows);
  list.sort(function (a, b) {
    var x = a.cells[index].textContent, y = b.cells[index].textContent;
    /* The number is dug out of the text rather than kept alongside it: these
     * cells hold what the export wrote, and re-parsing costs nothing at the
     * few thousand rows this ever sees. */
    var r = numeric ? (parseFloat(x.replace(/[^\d.eE+-]/g, '')) || 0) -
                      (parseFloat(y.replace(/[^\d.eE+-]/g, '')) || 0)
                    : (x < y ? -1 : x > y ? 1 : 0);
    return desc ? -r : r;
  });
  list.forEach(function (r) { body.appendChild(r); });
}

/// Makes every header of @p table sort by its column, flipping on each click.
function wireTable(table) {
  Array.prototype.forEach.call(table.tHead.rows[0].cells, function (th, i) {
    th.addEventListener('click', function () {
      th.dataset.desc = th.dataset.desc === '1' ? '0' : '1';
      tableSort(table, i, th.classList.contains('n'), th.dataset.desc === '1');
    });
  });
}

/**
 * Wires a search box to its table.  It hides rows instead of dropping them,
 * and SAYS how many of how many are showing: a table that silently shrinks
 * reads like a shorter export.
 */
function wireFilter(input) {
  var table = document.getElementById(input.dataset.forTable);
  input.addEventListener('input', function () {
    var q = input.value.toLowerCase(), shown = 0;
    Array.prototype.forEach.call(table.tBodies[0].rows, function (r) {
      var on = !q || r.textContent.toLowerCase().indexOf(q) >= 0;
      r.hidden = !on;
      if (on) shown++;
    });
    var note = document.getElementById(input.dataset.forCount);
    if (note) {
      note.textContent = shown.toLocaleString('en-US') + ' of ' +
          table.tBodies[0].rows.length.toLocaleString('en-US') + ' rows';
    }
  });
}
