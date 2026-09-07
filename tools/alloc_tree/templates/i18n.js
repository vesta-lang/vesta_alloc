/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 *
 * The page in whichever language, switched WITHOUT reloading.
 *
 * The whole catalogue travels inside the file, so this works with no network
 * like the rest of the page -- and, more to the point, the reader who wants
 * the other language is not going to go back and run the tool again with a
 * different flag.  A report that has to be regenerated to be read is a report
 * that stays in the language of whoever generated it.
 *
 * Static text is marked in the markup with `data-t`, so adding a label does
 * not mean remembering to translate it somewhere else: a key with no entry
 * comes out as the key, in brackets, which is ugly on purpose.
 */
'use strict';

var LANG = 'en';

/// The string for a key, with `{name}` parameters filled in.
function T(key, params) {
  var table = DATA.strings[LANG] || DATA.strings.en;
  var text = table[key];
  if (text === undefined) text = DATA.strings.en[key];
  if (text === undefined) return '[' + key + ']';
  if (!params) return text;
  return text.replace(/\{(\w+)\}/g, function (whole, name) {
    return params[name] !== undefined ? params[name] : whole;
  });
}

/**
 * Puts every marked string into the current language.
 *
 * `data-t` fills the text, `data-tph` the placeholder of a search box and
 * `data-tt` the tooltip.  Three attributes and not one because they are three
 * different places a string can live in HTML, and a single one would silently
 * do nothing on two of them.
 */
function applyLang() {
  document.querySelectorAll('[data-t]').forEach(function (el) {
    el.textContent = T(el.dataset.t);
  });
  document.querySelectorAll('[data-tph]').forEach(function (el) {
    el.placeholder = T(el.dataset.tph, { what: el.dataset.tphWhat || '' });
  });
  document.querySelectorAll('[data-tt]').forEach(function (el) {
    el.title = T(el.dataset.tt);
  });
  // "N rows" is a sentence with a number in it, and the number is already in
  // the document: the count of a table cannot change, only its wording.
  document.querySelectorAll('[data-trows]').forEach(function (el) {
    el.textContent = T('raw.rows', { n: Number(el.dataset.trows).toLocaleString('en-US') });
  });

  var meta = DATA.meta;
  var sub = T('sub', { sites: meta.sites, allocs: meta.allocs, bytes: meta.bytes });
  if (meta.unresolved && meta.unresolved !== '0')
    sub += ' · ' + T('sub.unresolved', { n: meta.unresolved });
  document.getElementById('sub').textContent = '· ' + sub;

  /* The warnings arrive as a key and its numbers, never as a sentence: what
   * could NOT be done has to be readable in the language the page is in, and
   * a formatted sentence would be stuck in the one it was generated with. */
  document.getElementById('warns').innerHTML = DATA.warnings.map(function (w) {
    return '<div class="warn">' + esc(T(w[0], w[1])) + '</div>';
  }).join('');

  document.documentElement.lang = LANG;
}

/// Remembers the choice, so the next report opens in the same language.
function loadLang() {
  try {
    var saved = window.localStorage.getItem('alloc_tree_lang');
    if (saved && DATA.strings[saved]) LANG = saved;
  } catch (e) {
    /* Private windows and browsers with site data blocked throw on the
     * accessor itself.  The page works, it just does not remember. */
  }
}

function saveLang(lang) {
  LANG = lang;
  try { window.localStorage.setItem('alloc_tree_lang', lang); } catch (e) {}
}
