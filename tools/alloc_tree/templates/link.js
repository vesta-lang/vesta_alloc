/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 *
 * The view, in the address bar.
 *
 * WHY.  Because a finding has to be passable to someone else.  Without this,
 * "look at what util is doing" comes with instructions -- open it, group by
 * module, type this, unfold that -- and whoever receives it has to follow them
 * correctly to see the same thing.  With it, it is a link.
 *
 * WHAT TRAVELS: the grouping, the direction, the filter, whether it is weighed
 * by bytes, and the selected row.  Not the fold state: unfolding is how you
 * READ a tree, not what you found, and it would make the link enormous.
 *
 * The selection travels as a path of NAMES and not as an index.  An index
 * would point at a different row the moment somebody sorts by another column,
 * and at nothing at all in a report generated the next day.
 */
'use strict';

/// Path separator inside the hash.  A `>` cannot appear at the start of a
/// symbol and reads as a path, which matters because these end up in chat
/// messages and bug reports where a person has to see what they are sending.
var LINK_SEP = '>';

function readHash() {
  var state = {};
  var raw = '';
  try {
    raw = (window.location.hash || '').replace(/^#/, '');
  } catch (e) {
    return state;                 // no address bar (a test harness); fine
  }
  if (!raw) return state;
  raw.split('&').forEach(function (pair) {
    var at = pair.indexOf('=');
    if (at < 0) return;
    var key = pair.slice(0, at);
    var value = decodeURIComponent(pair.slice(at + 1).replace(/\+/g, ' '));
    if (key === 'g') state.grouping = value;
    /* Which POPULATION, because a link that does not carry it opens on the
     * default -- and since the default is now the checker's when there is one,
     * a link made while looking at the allocator's would land the reader on
     * different numbers under the same address. */
    else if (key === 'v') state.dataset = value;
    else if (key === 'd') state.dir = value;
    else if (key === 'q') state.needle = value;
    else if (key === 'b') state.byBytes = value === '1';
    else if (key === 'p') state.path = value.split(LINK_SEP);
  });
  return state;
}

/**
 * Writes the view into the address bar.
 *
 * `replaceState` and not an assignment to `location.hash`: every change of
 * grouping or keystroke in the filter would otherwise be a history entry, and
 * Back would walk out of the report one letter at a time.
 *
 * A very long selection is DROPPED rather than making an unusable link -- some
 * chat clients cut a URL at a couple of thousand characters, and a link that
 * arrives truncated selects the wrong row instead of none.  The rest of the
 * view still travels, which is the part that matters.
 */
function writeHash(state) {
  var parts = [];
  if (state.grouping && state.grouping !== 'stack')
    parts.push('g=' + encodeURIComponent(state.grouping));
  /* Written whenever the file HAS two populations, even for the one that
   * opens by default: which one that is depends on the export, so leaving it
   * out would make the link mean one thing here and another elsewhere. */
  if (state.dataset && DATA.check)
    parts.push('v=' + encodeURIComponent(state.dataset));
  if (state.dir && state.dir !== 'td')
    parts.push('d=' + encodeURIComponent(state.dir));
  if (state.needle) parts.push('q=' + encodeURIComponent(state.needle));
  if (state.byBytes) parts.push('b=1');
  if (state.path && state.path.length) {
    var joined = 'p=' + encodeURIComponent(state.path.join(LINK_SEP));
    if (joined.length < 1500) parts.push(joined);
  }
  var hash = parts.length ? '#' + parts.join('&') : '';
  try {
    window.history.replaceState(null, '',
        window.location.pathname + window.location.search + hash);
  } catch (e) {
    /* A page opened straight off the disk can refuse `replaceState` in some
     * browsers.  The report works; only the link does not update. */
  }
}
