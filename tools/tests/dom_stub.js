/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 *
 * Just enough browser to RUN the page scripts.
 *
 * WHY NOT A REAL BROWSER.  Because the point is not to see the page, it is to
 * catch the things that break it: a name that comes back altered, a tree that
 * lost allocations, a grouping that throws.  Those are answered by executing
 * the code, and requiring a headless browser would mean these tests only run
 * where one is installed -- which is where tests quietly stop running.
 *
 * What it does NOT do is lay anything out, so nothing here can tell you the
 * page looks right.  It tells you it WORKS.
 */
'use strict';

function element(id, data) {
  return {
    id: id,
    dataset: data || {},
    innerHTML: '',
    textContent: '',
    placeholder: '',
    title: '',
    hidden: false,
    value: '',
    classList: {
      add() {}, remove() {}, toggle() {}, contains() { return false; }
    },
    addEventListener() {},
    insertAdjacentHTML() {},
    querySelector() { return { innerHTML: '' }; },
    set onclick(f) {},
    set onchange(f) {},
  };
}

/// The column keys of the grid, in the order the template writes them.
const TH_KEYS = ['name', 'total', 'pct', 'self', 'bytes', 'selfbytes',
                 'large', 'classes', 'purpose', 'sites', 'module', 'file',
                 'addr'];

function makeDom(DATA) {
  const byId = {};
  const ths = TH_KEYS.map(k => element(k, { k: k }));
  const dirs = [element('td', { dir: 'td' }), element('bu', { dir: 'bu' })];

  const document = {
    documentElement: { lang: 'en' },
    getElementById(id) { return byId[id] || (byId[id] = element(id)); },
    querySelectorAll(sel) {
      if (sel === '#grid th') return ths;
      if (sel === '[data-dir]') return dirs;
      return [];          // tabs, panels and the raw tables: nothing to lay out
    },
  };

  const store = {};
  // An address bar that behaves like one: `replaceState` changes the hash and
  // adds no history entry, which is what the page relies on.
  const location = { hash: '', pathname: '/alloc_tree.html', search: '' };
  const win = {
    location: location,
    history: {
      replaceState(state, title, url) {
        const at = String(url).indexOf('#');
        location.hash = at < 0 ? '' : String(url).slice(at);
      },
    },
    localStorage: {
      getItem: k => (k in store ? store[k] : null),
      setItem: (k, v) => { store[k] = String(v); },
    },
  };
  const ctx = {
    DATA: DATA,
    document: document,
    console: console,
    window: win,
  };
  return { ctx: ctx, byId: byId, ths: ths, location: location };
}

module.exports = { makeDom: makeDom, TH_KEYS: TH_KEYS };
