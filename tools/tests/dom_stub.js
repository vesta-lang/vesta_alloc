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

/**
 * A 2D context that DRAWS NOTHING AND REMEMBERS EVERYTHING.
 *
 * Nothing here can say the chart looks right -- there is no layout and no
 * pixels.  What it can say is what the code decided to draw: how many bands it
 * filled, how many lines it stroked, what text it put on the phases.  Those are
 * the things that break, and they break without a browser.
 */
function context2d() {
  const calls = [];
  const rec = name => function () {
    calls.push([name, Array.prototype.slice.call(arguments)]);
  };
  return {
    calls: calls,
    setTransform: rec('setTransform'), clearRect: rec('clearRect'),
    beginPath: rec('beginPath'), closePath: rec('closePath'),
    moveTo: rec('moveTo'), lineTo: rec('lineTo'), arc: rec('arc'),
    fill: rec('fill'), stroke: rec('stroke'), fillText: rec('fillText'),
    save: rec('save'), restore: rec('restore'), translate: rec('translate'),
    // `scale` hace falta desde que las burbujas se amplian: la vista es una
    // transformacion del lienzo, no otra colocacion de los circulos.
    scale: rec('scale'),
    setLineDash: rec('setLineDash'),
    font: '', textAlign: '', textBaseline: '',
    fillStyle: '', strokeStyle: '', lineWidth: 1, globalAlpha: 1,
  };
}

/// A box with a size, which is all the chart asks the layout for.
function rect() {
  return { width: 900, height: 360, left: 0, top: 0, right: 900, bottom: 360 };
}

function element(id, data) {
  return {
    id: id,
    dataset: data || {},
    style: {},
    checked: false,
    width: 0,
    height: 0,
    // El lienzo cuelga de una caja, y su ancho sale de ella: sin padre, el
    // grafico se dibuja a lo ancho de cero y no se nota en ninguna prueba.
    parentNode: { getBoundingClientRect: rect },
    getBoundingClientRect: rect,
    getContext() { return this._ctx || (this._ctx = context2d()); },
    querySelectorAll() { return []; },
    innerHTML: '',
    textContent: '',
    placeholder: '',
    title: '',
    hidden: false,
    open: false,
    value: '',
    classList: {
      add() {}, remove() {}, toggle() {}, contains() { return false; }
    },
    // Los oyentes se GUARDAN, no se tiran.  Lo que el navegador hace al
    // interactuar -- plegar un bloque, cambiar una pestana -- es codigo que
    // solo corre desde aqui: un stub que acepta el oyente y lo olvida deja sin
    // probar justo la mitad que responde al lector.
    _on: {},
    addEventListener(type, fn) {
      (this._on[type] || (this._on[type] = [])).push(fn);
    },
    fire(type) {
      (this._on[type] || []).forEach(function (fn) { fn({ type: type }); });
    },
    insertAdjacentHTML(where, html) { this.innerHTML += html; },
    /* UNA TABLA, lo justo que la pagina le pide.  Desde que el volcado crudo
       viaja como DATOS y lo pinta `tables.js`, arrancar la pagina recorre las
       tablas de verdad: un elemento sin cabecera ni cuerpo reventaba el
       arranque entero, y eso no es un stub incompleto sino una prueba que no
       llega a correr.  El cuerpo ACUMULA lo que se le inserta, que es lo unico
       que hace falta para poder mirar lo que se pinto. */
    tHead: { rows: [{ cells: [] }] },
    tBodies: [{
      innerHTML: '',
      insertAdjacentHTML(where, html) { this.innerHTML += html; },
    }],
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
    // La raiz lleva el IDIOMA y el TEMA, y los dos se ponen y se quitan: `auto`
    // es no tener atributo, que es lo que deja mandar a la hoja de estilos.
    documentElement: {
      lang: 'en',
      attrs: {},
      setAttribute(name, value) { this.attrs[name] = value; },
      removeAttribute(name) { delete this.attrs[name]; },
      getAttribute(name) {
        return name in this.attrs ? this.attrs[name] : null;
      },
    },
    getElementById(id) { return byId[id] || (byId[id] = element(id)); },
    // Solo se busca por clase el bloque plegable de avisos, y solo existe si
    // la pagina lo ha pintado: devolver uno siempre haria pasar una prueba
    // sobre algo que no esta en el documento.
    querySelector(sel) {
      /* La pestana del eje del tiempo: `time.js` la esconde cuando la corrida
       * no llevaba eje, y eso solo se puede probar si el stub la devuelve. */
      if (sel === '[data-panel="p-time"]')
        return byId['#tab-time'] || (byId['#tab-time'] = element('tab-time'));
      /* Y la de los tamanos, por lo mismo: `sizes.js` la esconde cuando el
       * volcado no trae lo que cada casilla se queda. */
      if (sel === '[data-panel="p-sizes"]')
        return byId['#tab-sizes'] || (byId['#tab-sizes'] = element('tab-sizes'));
      if (sel !== '.warnbox') return null;
      var warns = byId['warns'];
      if (!warns || warns.innerHTML.indexOf('class="warnbox"') < 0) return null;
      return byId['.warnbox'] || (byId['.warnbox'] = element('.warnbox'));
    },
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
    devicePixelRatio: 1,
    // El grafico se rehace al cambiar el tamano de la ventana.  Aceptar el
    // oyente y olvidarlo dejaria sin probar la mitad que responde.
    _on: {},
    addEventListener(type, fn) {
      (this._on[type] || (this._on[type] = [])).push(fn);
    },
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
