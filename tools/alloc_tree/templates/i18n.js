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

/**
 * \~english The THEME: light, dark, or whatever the system says.
 *
 * Remembered like the language and for the same reason: whoever opens one
 * report in dark opens the next one in dark, and a choice that has to be made
 * again in every file is a choice people stop making.
 *
 * `auto` writes no attribute, which is what leaves the stylesheet's media query
 * in charge.  The other two DO write it, and that is why they win in both
 * directions -- including setting light on a machine set to dark.
 *
 * \~spanish El TEMA: claro, oscuro, o lo que diga el sistema.
 *
 * Se guarda como el idioma y por la misma razon: quien abre un informe en
 * oscuro abre el siguiente en oscuro, y una eleccion que hay que repetir en
 * cada fichero es una eleccion que se deja de hacer.
 *
 * `auto` no escribe el atributo, que es lo que deja mandar a la consulta de
 * medios de la hoja de estilos.  Los otros dos SI lo escriben, y por eso ganan
 * en las dos direcciones -- incluido poner claro en una maquina en oscuro.  \~
 */
function applyTheme(theme) {
  if (theme === 'light' || theme === 'dark')
    document.documentElement.setAttribute('data-theme', theme);
  else
    document.documentElement.removeAttribute('data-theme');
}

function loadTheme() {
  try {
    return window.localStorage.getItem('alloc_tree_theme') || 'auto';
  } catch (e) { return 'auto'; }
}

function saveTheme(theme) {
  try { window.localStorage.setItem('alloc_tree_theme', theme); } catch (e) {}
  applyTheme(theme);
}

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
  /* Que cifra se anuncia lo decide quien la leyo del volcado, no esta linea:
   * un volcado nuevo trae el maximo que el proceso llego a tener y uno viejo
   * solo el acumulado, y NO se llaman igual porque no significan lo mismo. */
  var sub = T(meta.bytes_key || 'sub',
              { sites: meta.sites, allocs: meta.allocs, bytes: meta.bytes });
  if (meta.unresolved && meta.unresolved !== '0')
    sub += ' · ' + T('sub.unresolved', { n: meta.unresolved });
  document.getElementById('sub').textContent = '· ' + sub;

  /* The warnings arrive as a key and its numbers, never as a sentence: what
   * could NOT be done has to be readable in the language the page is in, and
   * a formatted sentence would be stuck in the one it was generated with. */
  /* PLEGADOS, PERO NUNCA ESCONDIDOS.  Siete cajas a todo lo ancho se comian la
   * primera pantalla y empujaban el arbol fuera de la vista, que es lo que uno
   * viene a mirar.  Quitarlos no es una opcion -- lo que no se pudo cubrir tiene
   * que verse o el informe miente por omision --, asi que se pliegan y el
   * resumen sigue diciendo CUANTOS y DE QUE TIPO.
   *
   * La distincion entre las dos clases se mantiene en el resumen porque no son
   * lo mismo: un aviso es algo que se intento y no cupo; un hueco es algo que
   * este nivel ni mira.  Plegado sin esa cuenta seria un triangulo que no dice
   * nada, y entonces si estaria escondido. */
  var ws = DATA.warnings || [];
  var gaps = ws.filter(function (w) { return w[0].indexOf('gap.') === 0; });
  var warns = ws.filter(function (w) { return w[0].indexOf('gap.') !== 0; });
  document.getElementById('warns').innerHTML = !ws.length ? '' :
    '<details class="warnbox"' + (openWarns() ? ' open' : '') + '>'
    + '<summary>' + esc(T('warns.summary',
                          { n: warns.length, gaps: gaps.length })) + '</summary>'
    + ws.map(function (w) {
        return '<div class="warn' + (w[0].indexOf('gap.') === 0 ? ' gap' : '')
          + '">' + esc(T(w[0], w[1])) + '</div>';
      }).join('') + '</details>';
  var box = document.querySelector('.warnbox');
  if (box) box.addEventListener('toggle', function () {
    try { window.localStorage.setItem('alloc_tree_warns',
                                      box.open ? '1' : '0'); } catch (e) {}
  });

  /* Y QUE ESTABA MAL.  Se repinta con el idioma por lo mismo que los avisos: la
   * cabecera se arma aqui y no al generar, para que cambiar de idioma no exija
   * regenerar la pagina.  Las direcciones y los nombres de las pilas no se
   * traducen -- son datos -- y por eso van fuera de `T`. */
  var vbox = document.getElementById('verdicts');
  if (vbox) {
    var vs = DATA.verdicts || [];
    vbox.innerHTML = !vs.length ? '' :
      '<div class="verdicts"><h2>' + esc(T('chk.verdicts', { n: vs.length }))
      + '</h2>' + vs.map(function (v) {
        var who = v.allocated_name
          ? '<div class="vwhere">' + esc(T('chk.verdict.at')) + ' '
            + esc(v.allocated_name) + '</div>'
          : '';
        return '<div class="verdict ' + esc(v.certainty) + '">'
          + '<span class="vkind">' + esc(T('cert.' + v.certainty)) + '</span> '
          + esc(v.what) + ' <code>' + esc(v.address) + '</code>' + who
          + '</div>';
      }).join('') + '</div>';
  }

  document.documentElement.lang = LANG;
}

/* Si el bloque de avisos se abre o no.  Se recuerda, como el idioma: quien los
 * quiere abiertos los quiere abiertos en todos los informes, y quien ya los ha
 * leido no quiere volver a apartarlos.  Cerrado por defecto -- el resumen sigue
 * diciendo cuantos hay, asi que cerrado no es callado. */
function openWarns() {
  try { return window.localStorage.getItem('alloc_tree_warns') === '1'; }
  catch (e) { return false; }
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
