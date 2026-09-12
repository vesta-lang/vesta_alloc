/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 *
 * \~english
 * THE TIME AXIS: memory against the run, and who is underneath it.
 *
 * A PANEL OF ITS OWN, and not a column of the tree.  The tree answers WHO
 * allocates and cannot answer WHEN: its figures are totals over the whole run,
 * so two sites that were never live at the same moment add up in one branch
 * exactly like two that overlapped completely.  Those are two questions, and
 * mixing them into one view is how the second one never gets asked.
 *
 * WHAT IS DRAWN IS THE MEASUREMENT.  The curve is the checker's own cuts --
 * every one a copy of what was live at that instant -- so nothing here is
 * interpolated, smoothed or guessed.  Where a site is missing from a cut it
 * held less than the floor, and that is drawn as the gap it is instead of
 * being filled in.
 *
 * AND IT IS PICKED, NOT PRESCRIBED.  A chart that decides for you which ten
 * series matter answers one question well and every other one badly.  What goes
 * on it is chosen: by stack, by function, by file or by module -- which is how
 * the SAME function allocating from twenty places becomes one band instead of
 * twenty -- and any of them can be switched on and off one at a time.
 *
 * \~spanish
 * EL EJE DEL TIEMPO: la memoria contra la corrida, y quien esta debajo.
 *
 * PANEL PROPIO, y no una columna del arbol.  El arbol contesta QUIEN reserva y
 * no puede contestar CUANDO: sus cifras son totales de toda la corrida, asi que
 * dos sitios que nunca estuvieron vivos a la vez se suman en la misma rama
 * igual que dos que se solapaban enteros.  Son dos preguntas, y mezclarlas en
 * una vista es como la segunda no se contesta nunca.
 *
 * LO QUE SE DIBUJA ES LA MEDIDA.  La curva son los cortes del propio
 * comprobador -- cada uno una copia de lo que habia vivo en ese instante --,
 * asi que aqui no hay nada interpolado, suavizado ni supuesto.  Donde un sitio
 * falta de un corte es que tenia menos que el suelo, y eso se dibuja como el
 * hueco que es en vez de rellenarse.
 *
 * Y SE ELIGE, NO SE IMPONE.  Un grafico que decide por ti que diez series
 * importan contesta bien una pregunta y mal todas las demas.  Lo que entra se
 * escoge: por pila, por funcion, por fichero o por modulo -- que es como la
 * MISMA funcion reservando desde veinte sitios pasa a ser una banda en vez de
 * veinte -- y cualquiera se enciende y se apaga por su cuenta.
 * \~
 */
'use strict';

/* \~english The columns of a cut, by name instead of by number: an `e[2]` in
 * the middle of the drawing code is how a column added later ends up plotted as
 * another one.
 * \~spanish Las columnas de un corte, por nombre y no por numero: un `e[2]` en
 * mitad del codigo de dibujo es como una columna anadida despues acaba pintada
 * como otra.  \~ */
var TI = 0, TA = 1, TLIVE = 2, TCOMM = 3, TBORN = 4, TDIED = 5, TOTHER = 6,
    TMARK = 7, TREG = 8, TFREE = 9, TEMPTY = 10;

var TIME = null;     // DATA.time, or null when the run had no axis
var TGROUPS = [];    // what CAN be drawn, biggest first
var TBYKEY = {};     // key -> group
var TPICK = null;    // keys switched on; null until the first build
var TZOOM = null;    // {from, to} in allocations, or null for the whole run
var TDRAG = null;    // an in-flight zoom drag
var TPAN = null;     // where the pointer was when it started dragging to move
var THOVER = -1;     // the cut under the pointer, -1 for none
/* Los circulos del modo burbujas, TAL COMO SE DIBUJARON.  El puntero tiene que
 * poder decir sobre cual esta, y eso no se puede recalcular desde los datos: la
 * colocacion sale de un empaquetado, no de una formula. */
var TBUBBLES = [];
/* \~english Where the bubbles are being looked at from: a scale and a shift.
 * A view and not a re-layout, so zooming shows the same packing closer instead
 * of a different one.
 * \~spanish Desde donde se miran las burbujas: una escala y un desplazamiento.
 * Una vista y no otra colocacion, para que ampliar ensene el mismo empaquetado
 * mas de cerca en vez de uno distinto.  \~ */
var TVIEW = { k: 1, x: 0, y: 0 };

/// The frame of a stack that is OURS: not `operator new`, which every stack
/// ends in and which would label every series the same.
function timeOwnFrame(sid) {
  var site = TIME.byId[sid];
  if (!site) return null;
  var frames = DATA.check.frames;
  for (var i = 0; i < site.chain.length; ++i) {
    var f = frames[site.chain[i]];
    if (!f[5] && !f[6] && !f[7]) return f;
  }
  return frames[site.chain[0]] || null;
}

function timeName(sid) {
  var f = timeOwnFrame(sid);
  return f ? f[0] : '#' + sid;
}

function timeWhere(sid) {
  var f = timeOwnFrame(sid);
  return f && f[1] ? f[1].split(/[\\/]/).pop() + ':' + f[2] : '';
}

/* \~english A COLOUR FROM THE KEY, never from the position in the list.  The
 * position changes with the filter, and a band that changes colour because
 * something unrelated was typed is a band nobody can follow across two
 * screenshots.
 * \~spanish UN COLOR SACADO DE LA CLAVE, nunca de la posicion en la lista.  La
 * posicion cambia con el filtro, y una banda que cambia de color porque se
 * escribio algo que no le incumbe es una banda que nadie puede seguir entre dos
 * capturas.  \~ */
function timeColor(key) {
  var h = 0;
  for (var i = 0; i < key.length; ++i) h = (h * 31 + key.charCodeAt(i)) | 0;
  h = Math.abs(h);
  return 'hsl(' + (h % 360) + ',62%,' + (52 + (h % 4) * 6) + '%)';
}

/**
 * \~english Builds what CAN be drawn, at the grain that was asked for.
 *
 * GROUPING IS THE POINT, not a convenience.  One function that allocates from
 * twenty places is twenty stacks and one answer, and a chart that can only
 * draw stacks makes the reader add twenty bands up by eye -- which is the same
 * as not showing it.  The grain is chosen here and everything downstream draws
 * whatever it produced.
 *
 * \~spanish Construye lo que SE PUEDE dibujar, al grano que se haya pedido.
 *
 * AGRUPAR ES EL PUNTO, no una comodidad.  Una funcion que reserva desde veinte
 * sitios son veinte pilas y una sola respuesta, y un grafico que solo sabe
 * dibujar pilas obliga a sumar veinte bandas a ojo -- que es lo mismo que no
 * ensenarlo.  El grano se elige aqui y todo lo de abajo dibuja lo que salga.
 * \~
 */
/**
 * \~english The shape a stack's blocks turned out to have, MEASURED.
 *
 * Not the purpose the programmer declared -- that is the allocator's `tag` --
 * but what the checker saw the blocks do: fixed, growing, one-shot.  It is read
 * out of the checker's own column, so a run whose export has no such column
 * answers "(?)" and not a made-up shape.
 *
 * \~spanish La forma que resultaron tener los bloques de una pila, MEDIDA.
 *
 * No el proposito que declaro el programador -- eso es el `tag` del asignador
 * -- sino lo que el comprobador vio hacer a los bloques: fija, creciente, de un
 * solo uso.  Sale de la columna del propio comprobador, asi que una corrida
 * cuyo volcado no la traiga contesta "(?)" y no una forma inventada.  \~
 */
function timeShape(sid) {
  var site = TIME.byId[sid];
  if (!site) return '(?)';
  var cols = (DATA.check && DATA.check.siteCols) || [];
  var i = cols.indexOf('shape');
  if (i >= 0 && site.row && site.row[i]) return site.row[i];
  return site.tag || '(?)';
}

/// El valor de una columna del comprobador para una pila, o '' si no esta.
/// Por NOMBRE y no por posicion: el orden de las columnas es del volcado, y
/// leerlo por indice fijo se rompe callando en cuanto alguien anade una.
function timeCol(sid, name) {
  var site = TIME.byId[sid];
  var cols = (DATA.check && DATA.check.siteCols) || [];
  var i = cols.indexOf(name);
  if (!site || i < 0 || !site.row) return '';
  return site.row[i];
}

/**
 * \~english How long this stack's blocks LIVED, as a band.
 *
 * The life the checker measures is not a clock: it is how many allocations of
 * the owning thread went by between a block being handed out and given back.
 * That is what makes it comparable between runs and across machines, and it is
 * why the bands are named in allocations and not in milliseconds.
 *
 * Powers of ten, because a life is read by its ORDER of magnitude: the question
 * is "does this die right away or outlive the phase", and forty bands of equal
 * width would answer it by making the reader add them up.
 *
 * A stack whose blocks NEVER died has no measured life, and gets its own band
 * instead of falling into the longest one -- "it outlived the run" and "it
 * lasted a long time" are different answers, and folding them together would
 * quietly turn what leaked into what is merely long-lived.
 *
 * \~spanish Cuanto VIVIERON los bloques de esta pila, como banda.
 *
 * La vida que mide el comprobador no es un reloj: son cuantas reservas del hilo
 * dueno pasaron entre que un bloque se entrego y volvio.  Eso es lo que la hace
 * comparable entre corridas y entre maquinas, y por eso las bandas se nombran
 * en reservas y no en milisegundos.
 *
 * Potencias de diez, porque una vida se lee por su ORDEN de magnitud: la
 * pregunta es "esto muere enseguida o sobrevive a la fase", y cuarenta bandas
 * de igual ancho la contestan obligando a sumarlas.
 *
 * Una pila cuyos bloques no murieron NUNCA no tiene vida medida, y se lleva su
 * propia banda en vez de caer en la mas larga -- "sobrevivio a la corrida" y
 * "duro mucho" son respuestas distintas, y juntarlas convertiria en silencio lo
 * que se quedo dentro en algo meramente longevo.  \~
 */
function timeLife(sid) {
  var deaths = Number(timeCol(sid, 'deaths') || 0);
  if (!deaths) return { key: 'v:none', label: T('time.life.none') };
  var avg = Number(timeCol(sid, 'life_avg') || 0);
  var top = 10;
  while (top < 1000000000 && avg > top) top *= 10;
  return { key: 'v:' + top, label: T('time.life.upto', { n: num(top) }) };
}

/**
 * \~english The curve split by SIZE, which is a different measurement and not
 * another grain of the same one.
 *
 * The series above are per stack, and a stack serves several buckets -- so no
 * grouping of them can answer "when does each size fill up".  These come from
 * the checker's own per-cut tally, which is why they are built apart instead of
 * folded out of `TIME.series`.
 *
 * \~spanish La curva repartida por TAMANO, que es otra medida y no otro grano
 * de la misma.
 *
 * Las series de arriba son por pila, y una pila sirve varias casillas -- asi
 * que ninguna agrupacion de ellas puede contestar "cuando se llena cada
 * tamano".  Estas salen del recuento por corte del propio comprobador, y por
 * eso se arman aparte en vez de sacarse de `TIME.series`.  \~
 */
function timeBuildSizes(groups) {
  var bounds = {};
  (TIME.buckets || []).forEach(function (b) { bounds[b[0]] = b[1]; });
  Object.keys(TIME.sizes || {}).forEach(function (bucketText) {
    var upper = bounds[bucketText | 0];
    var label = upper ? '≤ ' + human(upper) : '> 16 MiB';
    var g = { key: 'z:' + bucketText, label: label, where: '', at: {},
              peak: 0, sids: [] };
    TIME.sizes[bucketText].forEach(function (p) { g.at[p[0]] = p[1]; });
    groups[g.key] = g;
  });
}

function timeBuild() {
  var how = document.getElementById('tgroup').value || 'site';
  var hideOwn = document.getElementById('tmine').checked;
  var groups = {};
  /* POR TAMANO NO SE RECORREN LOS SITIOS: la medida es otra.  Y se sale antes
   * de mirar `hideOwn`, porque lo del informe ya queda fuera donde se cuenta
   * -- ver `g_in_report` en el comprobador --, asi que aqui no hay nada que
   * filtrar y una casilla no tiene de quien ser. */
  if (how === 'size') {
    timeBuildSizes(groups);
    TGROUPS = [];
    Object.keys(groups).forEach(function (k) {
      var g = groups[k];
      Object.keys(g.at).forEach(function (e) {
        if (g.at[e] > g.peak) g.peak = g.at[e];
      });
      TGROUPS.push(g);
    });
    /* Por tamano se ordena por TAMANO y no por pico, al reves que todo lo
     * demas: el eje de esto es la casilla, y ordenarlo por altura convierte una
     * distribucion en un ranking -- se deja de ver que lo grande esta todo a un
     * lado. */
    TGROUPS.sort(function (a, b) {
      return (a.key.slice(2) | 0) - (b.key.slice(2) | 0);
    });
    TBYKEY = {};
    TGROUPS.forEach(function (g) { TBYKEY[g.key] = g; });
    return;
  }
  Object.keys(TIME.series).forEach(function (sidText) {
    var sid = sidText | 0;
    var site = TIME.byId[sid];
    if (hideOwn && site && site.instr) return;
    var frame = timeOwnFrame(sid);
    var key, label, where;
    if (how === 'function') {
      label = frame ? frame[0] : '#' + sid;
      key = 'f:' + label;
      where = timeWhere(sid);
    } else if (how === 'file') {
      label = frame && frame[1] ? frame[1].split(/[\\/]/).pop() : '(?)';
      key = 'p:' + label;
      where = frame && frame[1] ? frame[1] : '';
    } else if (how === 'module') {
      label = (frame && frame[3]) || '(?)';
      key = 'm:' + label;
      where = '';
    } else if (how === 'life') {
      /* Cuanto vivieron, por orden de magnitud.  Es el otro eje de la misma
       * pregunta que la forma: no quien reserva, sino que clase de memoria es
       * -- la que muere en la misma fase frente a la que sobrevive a todo. */
      var band = timeLife(sid);
      label = band.label;
      key = band.key;
      where = '';
    } else if (how === 'shape') {
      /* La forma MEDIDA, no la declarada: dos sitios distintos con la misma
       * forma son una sola banda, que es justo la pregunta -- cuanto de la
       * curva es memoria que crece frente a memoria de un solo uso. */
      label = timeShape(sid);
      key = 'h:' + label;
      where = '';
    } else {
      label = timeName(sid);
      key = 's:' + sid;
      where = timeWhere(sid);
    }
    var g = groups[key];
    if (!g) {
      g = groups[key] = { key: key, label: label, where: where, at: {},
                          peak: 0, sids: [] };
    }
    g.sids.push(sid);
    /* \~english Added up cut BY CUT, never as totals.  Two stacks of the same
     * function that were never live together must not stack into a band
     * neither of them ever reached -- which is exactly the mistake the whole
     * panel exists to stop.
     * \~spanish Sumado corte A CORTE, nunca por totales.  Dos pilas de la misma
     * funcion que nunca estuvieron vivas a la vez no pueden apilarse en una
     * banda que ninguna alcanzo -- que es justo el error que este panel existe
     * para evitar.  \~ */
    TIME.series[sidText].forEach(function (p) {
      g.at[p[0]] = (g.at[p[0]] || 0) + p[1];
    });
  });
  TGROUPS = [];
  Object.keys(groups).forEach(function (k) {
    var g = groups[k];
    Object.keys(g.at).forEach(function (e) {
      if (g.at[e] > g.peak) g.peak = g.at[e];
    });
    /* \~english AND WHERE IT COMES FROM, without having to click.  Grouping by
     * shape or by lifetime says what KIND of memory a band is and nothing
     * about whose it is -- so the chart read as "92 MiB of something that dies
     * quickly", which is a fact nobody can act on.  The band's biggest file
     * goes in the same slot the per-site bands use for their `file:line`, so
     * the picker and the legend answer "who" in every grouping instead of only
     * in some.
     *
     * \~spanish Y DE DONDE SALE, sin tener que pinchar.  Agrupar por forma o
     * por vida dice que CLASE de memoria es una banda y nada de quien es -- asi
     * que el grafico se leia como "92 MiB de algo que muere pronto", que es un
     * dato con el que nadie puede hacer nada.  El fichero mayor de la banda va
     * en la misma ranura que las bandas por sitio usan para su `fichero:linea`,
     * de forma que el selector y la leyenda contestan "quien" en todas las
     * agrupaciones y no solo en algunas.  \~ */
    if (!g.where && g.sids.length) {
      var top = timeBandFiles(g)[0];
      if (top && top.path) g.where = T('time.mostly', { file: top.name });
    }
    TGROUPS.push(g);
  });
  /* POR VIDA SE ORDENA POR VIDA, como el tamano y por lo mismo: su eje es una
   * magnitud, y ordenarla por altura convierte una distribucion en un ranking
   * -- se deja de ver si lo que pesa muere pronto o dura toda la corrida.  La
   * banda de las que no murieron va al final: no es la mas larga, es otra
   * cosa. */
  if (how === 'life') {
    TGROUPS.sort(function (a, b) {
      var x = a.key === 'v:none' ? Infinity : a.key.slice(2) | 0;
      var y = b.key === 'v:none' ? Infinity : b.key.slice(2) | 0;
      return x - y;
    });
  } else {
    TGROUPS.sort(function (a, b) { return b.peak - a.peak; });
  }
  TBYKEY = {};
  TGROUPS.forEach(function (g) { TBYKEY[g.key] = g; });
}

/// The top N by peak, which is where a reader starts before picking.
function timePickTop() {
  var n = Number(document.getElementById('ttop').value) || 10;
  TPICK = {};
  TGROUPS.slice(0, n).forEach(function (g) { TPICK[g.key] = true; });
}

/// What is on the chart right now, in the order it is stacked.
function timeShown() {
  return TGROUPS.filter(function (g) { return TPICK && TPICK[g.key]; });
}

/// The cuts inside the zoom, or all of them.
function timeCuts() {
  if (!TZOOM) return TIME.epochs;
  return TIME.epochs.filter(function (e) {
    return e[TA] >= TZOOM.from && e[TA] <= TZOOM.to;
  });
}

/**
 * \~english Whether this run measured what the allocator's ranges cost.
 *
 * ANY cut with something in it, not all of them: the first cuts happen before
 * the allocator has a range to ask about, so requiring every cut would throw
 * away the whole curve for the sake of its first two points.  The answer is
 * cached because it is asked once per draw and the tables run to hundreds of
 * cuts.
 *
 * \~spanish Si esta corrida midio lo que cuestan los rangos del asignador.
 *
 * ALGUN corte con algo, no todos: los primeros cortes pasan antes de que el
 * asignador tenga un rango por el que preguntar, asi que exigirlos todos
 * tiraria la curva entera por sus dos primeros puntos.  La respuesta se guarda
 * porque se pregunta una vez por dibujo y las tablas llegan a cientos de
 * cortes.  \~
 */
/**
 * \~english How the panel is being drawn.  Six ways, and each one answers
 * something the others cannot:
 *
 *   stack    how much they ADD UP to -- the shape of the whole run.
 *   lines    each band on its own, which is how two shapes are compared.
 *   log      a decade per stripe: the 0,5 MiB band and the 70 MiB one are both
 *            readable at once, which on a linear axis is impossible.
 *   pct      what the memory is MADE OF: a band that is a steady 2% stays a
 *            steady ribbon instead of shrinking as the total grows.
 *   grid     one chart per band, each with its OWN ruler: every band is as
 *            readable as the biggest, at the cost of comparing them.
 *   bubbles  who weighs what AT ONE MOMENT, with the area saying how much.
 *
 * \~spanish Como se esta dibujando el panel.  Seis formas, y cada una contesta
 * algo que las otras no:
 *
 *   stack    cuanto SUMAN -- la forma de la corrida entera.
 *   lines    cada banda por su cuenta, que es como se comparan dos formas.
 *   log      una decada por franja: la banda de 0,5 MiB y la de 70 se leen las
 *            dos a la vez, que en un eje lineal es imposible.
 *   pct      de QUE esta hecha la memoria: una banda que es un 2% constante se
 *            queda como una cinta pareja en vez de encogerse al crecer el
 *            total.
 *   grid     un grafico por banda, cada uno con SU regla: todas se leen igual
 *            de bien, a cambio de no poder compararlas entre si.
 *   bubbles  quien pesa que EN UN INSTANTE, con el area diciendo cuanto.  \~
 */
function timeHow() {
  var select = document.getElementById('tdraw');
  return (select && select.value) || 'stack';
}

/**
 * \~english Zooms by a factor, around the middle of what is on screen.
 *
 * ONE PLACE FOR BOTH KINDS OF CHART, because "zoom" means two different things
 * here and the reader should not have to know which: on a chart with a time
 * axis it narrows the stretch of the run being shown, and on the bubbles it is
 * a magnifying glass over the packing.  The buttons, the wheel and the double
 * click all come through here.
 *
 * \~spanish Amplia por un factor, alrededor del centro de lo que se ve.
 *
 * UN SOLO SITIO PARA LAS DOS CLASES DE GRAFICO, porque "ampliar" significa aqui
 * dos cosas y quien mira no tiene por que saber cual: en un grafico con eje de
 * tiempo estrecha el tramo de la corrida que se ensena, y en las burbujas es
 * una lupa sobre el empaquetado.  Los botones, la rueda y el doble clic pasan
 * todos por aqui.  \~
 *
 * @param factor mayor que uno acerca, menor aleja.
 * @param ax     donde apunta el puntero, en pixeles, o null para el centro.
 */
function timeZoom(factor, ax, ay) {
  if (timeHow() === 'bubbles') {
    var k = Math.min(20, Math.max(1, TVIEW.k * factor));
    if (ax === null || ax === undefined) {
      var canvas = document.getElementById('tchart');
      ax = canvas.clientWidth / 2;
      ay = canvas.clientHeight / 2;
    }
    /* El punto bajo el puntero se queda DONDE ESTA: es lo que hace que ampliar
     * sobre algo lo acerque en vez de mandarlo fuera de la pantalla. */
    TVIEW.x = ax - (ax - TVIEW.x) * (k / TVIEW.k);
    TVIEW.y = ay - (ay - TVIEW.y) * (k / TVIEW.k);
    TVIEW.k = k;
    if (TVIEW.k === 1) { TVIEW.x = 0; TVIEW.y = 0; }
    timeDraw();
    return;
  }
  var all = TIME.epochs;
  if (!all.length) return;
  var lo = TZOOM ? TZOOM.from : all[0][TA];
  var hi = TZOOM ? TZOOM.to : all[all.length - 1][TA];
  var mid = ax === null || ax === undefined ? (lo + hi) / 2
                                            : timeAtValue(ax, lo, hi);
  var span = (hi - lo) / factor;
  var from = mid - (mid - lo) / factor;
  var to = from + span;
  var first = all[0][TA], last = all[all.length - 1][TA];
  if (to - from >= last - first) { TZOOM = null; timeDraw(); return; }
  if (from < first) { to += first - from; from = first; }
  if (to > last) { from -= to - last; to = last; }
  TZOOM = { from: from, to: to };
  // Menos de dos cortes dentro no es una ampliacion, es un grafico vacio.
  if (timeCuts().length < 2) TZOOM = { from: lo, to: hi };
  timeDraw();
}

/// Que valor del eje hay bajo `px`, dada la ventana que se esta mirando.
function timeAtValue(px, lo, hi) {
  var canvas = document.getElementById('tchart');
  var L = 74, R = 14;
  var w = Math.max(1, canvas.clientWidth - L - R);
  var t = Math.min(1, Math.max(0, (px - L) / w));
  return lo + (hi - lo) * t;
}

/// Desplaza lo que se ve.  `dx`/`dy` en pixeles, lo que movio el puntero.
function timePan(dx, dy) {
  if (timeHow() === 'bubbles') {
    TVIEW.x += dx;
    TVIEW.y += dy;
    timeDraw();
    return;
  }
  var all = TIME.epochs;
  if (!TZOOM || !all.length) return;   // sin ampliar no hay a donde moverse
  var canvas = document.getElementById('tchart');
  var w = Math.max(1, canvas.clientWidth - 74 - 14);
  var step = (TZOOM.to - TZOOM.from) * (-dx / w);
  var first = all[0][TA], last = all[all.length - 1][TA];
  var from = TZOOM.from + step, to = TZOOM.to + step;
  if (from < first) { to += first - from; from = first; }
  if (to > last) { from -= to - last; to = last; }
  TZOOM = { from: from, to: to };
  timeDraw();
}

/// Vuelve a verlo todo, se estuviera mirando lo que se estuviera mirando.
function timeZoomAll() {
  TZOOM = null;
  TVIEW = { k: 1, x: 0, y: 0 };
  timeDraw();
}

var THASREG = null;
function timeHasRegion() {
  if (THASREG === null)
    THASREG = TIME.epochs.some(function (e) { return (e[TREG] || 0) > 0; });
  return THASREG;
}

/// Si la corrida midio ademas cuanto de la region estaba en listas de libres.
var THASFREE = null;
function timeHasFree() {
  if (THASFREE === null)
    THASFREE = TIME.epochs.some(function (e) { return (e[TFREE] || 0) > 0; });
  return THASFREE;
}

/// Y si midio los trozos de clase a los que se les murio TODO.
var THASEMPTY = null;
function timeHasEmpty() {
  if (THASEMPTY === null)
    THASEMPTY = TIME.epochs.some(function (e) { return (e[TEMPTY] || 0) > 0; });
  return THASEMPTY;
}

/**
 * \~english One of the cumulative curves that split the gap, drawn at live plus
 * everything up to @p upto.
 *
 * CUMULATIVE AND NOT SEPARATE, so each GAP between two curves is one of the
 * three things the slack is made of, read straight off the chart.  Drawn apart,
 * they would have to be subtracted by hand -- and the whole point of measuring
 * the split was that getting it the wrong way round is a week on the wrong cure.
 *
 * \~spanish Una de las curvas acumuladas que parten el hueco, dibujada en lo
 * vivo mas todo lo que llegue hasta @p upto.
 *
 * ACUMULADAS Y NO SUELTAS, para que cada HUECO entre dos curvas sea una de las
 * tres cosas de las que se compone la holgura, leida del grafico.  Sueltas,
 * habria que restarlas de cabeza -- y la razon entera de medir el reparto era
 * que confundirlo son una semana en la cura equivocada.  \~
 */
function timeSplitCurve(g, eps, X, Y, colour, upto, dash) {
  g.strokeStyle = colour;
  g.lineWidth = 1.5;
  g.setLineDash(dash);
  g.beginPath();
  eps.forEach(function (e, i) {
    var v = e[TLIVE] || 0;
    if (upto >= TFREE) v += e[TFREE] || 0;
    if (upto >= TEMPTY) v += e[TEMPTY] || 0;
    var y = Y(v);
    if (i === 0) g.moveTo(X(e[TA]), y); else g.lineTo(X(e[TA]), y);
  });
  g.stroke();
  g.setLineDash([]);
}

/**
 * \~english Draws the panel: bands per group, the two totals, and the phases.
 *
 * ONE PASS AND NO LIBRARY.  The page has to work from a file opened off a disc
 * with no network, so a chart library is not an option -- and what this draws
 * is a stack of areas and two lines, which is the part of a chart library that
 * is thirty lines long.
 *
 * \~spanish Dibuja el panel: bandas por grupo, los dos totales y las fases.
 *
 * UNA PASADA Y SIN LIBRERIA.  La pagina tiene que funcionar abierta de un disco
 * sin red, asi que una libreria de graficos no es una opcion -- y lo que esto
 * dibuja es una pila de areas y dos lineas, que es la parte de una libreria de
 * graficos que ocupa treinta lineas.  \~
 */
function timeDraw() {
  if (!TIME) return;
  var canvas = document.getElementById('tchart');
  var box = canvas.parentNode.getBoundingClientRect();
  var dpr = window.devicePixelRatio || 1;
  var W = Math.max(320, Math.floor(box.width));
  var how = timeHow();
  /* \~english THE HEIGHT IS THE BOX'S, and the box can be dragged taller -- a
   * chart locked to one height is one that cannot be looked at properly when
   * it holds forty bands.
   *
   * THE GRID IS THE EXCEPTION: it is as tall as it needs, one chart per band,
   * and the box scrolls.  A fixed height there would give each band six
   * pixels, which is the very problem that mode exists to fix.
   *
   * \~spanish EL ALTO ES EL DE LA CAJA, y la caja se puede estirar -- un
   * grafico clavado a una altura es uno que no se puede mirar bien cuando
   * lleva cuarenta bandas.
   *
   * LA REJILLA ES LA EXCEPCION: mide lo que haga falta, un grafico por banda,
   * y la caja se recorre por dentro.  Una altura fija ahi le daria seis
   * pixeles a cada banda, que es justo el problema que ese modo arregla.  \~ */
  var H = Math.max(200, Math.floor(box.height) - 2);
  if (how === 'grid')
    H = Math.max(H, 22 + Math.max(1, timeShown().length) * 46 + 26);
  canvas.style.width = W + 'px';
  canvas.style.height = H + 'px';
  canvas.width = Math.floor(W * dpr);
  canvas.height = Math.floor(H * dpr);
  var g = canvas.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect(0, 0, W, H);
  /* Los colores del tema, releidos EN CADA dibujo: cambiar de claro a oscuro
   * solo vuelve a pintar, no recarga nada, asi que un color cacheado al
   * arrancar se quedaria del tema anterior para siempre. */
  var INK = {
    grid: timeInk('--grid', 'rgba(128,128,128,.18)'),
    axis: timeInk('--axis', 'rgba(128,128,128,.85)'),
    live: timeInk('--live', '#178f6e'),
    comm: timeInk('--comm', '#c97b45'),
    region: timeInk('--region', '#8a5cd6'),
    free: timeInk('--free', '#4a90c2'),
    empty: timeInk('--empty', '#c2a04a'),
    mark: timeInk('--mark', 'rgba(120,120,200,.95)'),
    markline: timeInk('--markline', 'rgba(128,128,255,.55)'),
    rest: timeInk('--rest', 'rgba(128,128,128,.5)'),
    rule: timeInk('--rule', 'rgba(200,200,200,.5)'),
    band: timeInk('--band', 'rgba(120,160,255,.18)')
  };

  var eps = timeCuts();
  if (!eps.length) return;
  var L = 74, R = 14, Tp = 18, B = 34;
  var w = W - L - R, h = H - Tp - B;
  var show = document.getElementById('tseries').value || 'both';
  var stack = how === 'stack' || how === 'pct';
  var shown = timeShown();

  /* Los dos modos que no son una curva contra el tiempo se van por su cuenta:
   * uno reparte la caja en tantas cajas como bandas, y el otro no tiene eje de
   * tiempo porque contesta sobre UN instante. */
  if (how === 'grid') {
    timeDrawGrid(g, INK, eps, shown, L, R, Tp, B, W, H);
    return;
  }
  if (how === 'bubbles') {
    timeDrawBubbles(g, INK, eps, shown, L, R, Tp, B, W, H);
    return;
  }

  /* \~english THE SCALE IS A CHOICE, and it is the one that makes thin bands
   * readable.  Measured against the whole run, forty stacked bands leave the
   * small ones a hairline -- and the small ones are most of the questions.
   * Measured against what was PICKED, the same two bands fill the box.
   * \~spanish LA ESCALA SE ELIGE, y es lo que hace legibles las bandas finas.
   * Medidas contra la corrida entera, cuarenta bandas apiladas dejan a las
   * pequenas en un pelo -- y las pequenas son casi todas las preguntas.
   * Medidas contra lo ELEGIDO, esas mismas dos bandas llenan la caja.  \~ */
  var scale = document.getElementById('tscale').value || 'all';

  /* \~english THE 100% MODE DIVIDES, it does not rescale.  What each cut is
   * divided by is what was LIVE at it -- not the sum of the picked bands --
   * because the remainder up to the live curve is part of the composition: a
   * chart where the picked bands always fill the box would say the program
   * holds only what was picked.
   * \~spanish EL MODO 100% DIVIDE, no reescala.  Lo que divide cada corte es lo
   * que habia VIVO en el -- no la suma de las bandas elegidas -- porque el
   * resto hasta la curva de vivos es parte de la composicion: un grafico donde
   * las elegidas llenaran siempre la caja diria que el programa solo tiene lo
   * que se eligio.  \~ */
  var denom = null;
  if (how === 'pct') {
    denom = eps.map(function (e) {
      var sum = 0;
      shown.forEach(function (grp) { sum += grp.at[e[TI]] || 0; });
      return Math.max(sum, e[TLIVE]) || 1;
    });
  }
  function valueAt(grp, i) {
    var v = grp.at[eps[i][TI]] || 0;
    return denom ? v / denom[i] : v;
  }

  var top = 0;
  if (denom) {
    top = 1;
  } else if (scale === 'picked' && shown.length) {
    eps.forEach(function (e) {
      var sum = 0;
      shown.forEach(function (grp) {
        var v = grp.at[e[TI]] || 0;
        if (stack) sum += v; else sum = Math.max(sum, v);
      });
      top = Math.max(top, sum);
    });
  } else {
    eps.forEach(function (e) {
      if (show !== 'committed') top = Math.max(top, e[TLIVE]);
      if (show !== 'live') top = Math.max(top, e[TCOMM], e[TREG] || 0);
    });
  }
  if (top <= 0) top = 1;

  /* \~english THE FLOOR OF THE LOG AXIS, and a zero has no place on it.  Four
   * decades below the top: below that the axis would spend half its height on
   * bands of a few kilobytes, and the question here is which bands are worth
   * looking at, not how small the smallest is.  A band under the floor is drawn
   * ON the floor -- flattened, never dropped -- and the caption says so.
   * \~spanish EL SUELO DEL EJE LOGARITMICO, y un cero no tiene sitio en el.
   * Cuatro decadas por debajo del techo: mas abajo, el eje se gastaria media
   * altura en bandas de unos kilobytes, y la pregunta aqui es cuales merecen
   * mirarse, no cuanto mide la mas pequena.  Una banda por debajo del suelo se
   * dibuja SOBRE el -- aplastada, nunca tirada -- y el pie lo dice.  \~ */
  var floor = top / 10000;

  var x0 = eps[0][TA], x1 = eps[eps.length - 1][TA];
  if (x1 <= x0) x1 = x0 + 1;
  function X(a) { return L + (a - x0) / (x1 - x0) * w; }
  function Y(v) {
    if (how === 'log') {
      if (!(v > floor)) v = floor;
      return Tp + h - (Math.log(v / floor) / Math.log(top / floor)) * h;
    }
    return Tp + h - (v / top) * h;
  }

  g.font = '11px ui-monospace,Menlo,Consolas,monospace';
  g.textBaseline = 'middle';
  if (how === 'log') {
    // Una raya por DECADA, que es lo que hace legible un eje logaritmico: sin
    // ellas, la altura de una banda no se puede convertir en una cifra.
    for (var d = floor; d <= top * 1.0001; d *= 10) {
      var ly = Y(d);
      g.strokeStyle = INK.grid;
      g.beginPath(); g.moveTo(L, ly); g.lineTo(L + w, ly); g.stroke();
      g.fillStyle = INK.axis;
      g.textAlign = 'right';
      g.fillText(human(d), L - 8, ly);
    }
  } else {
    for (var k = 0; k <= 5; ++k) {
      var v = top * k / 5, y = Y(v);
      g.strokeStyle = INK.grid;
      g.beginPath(); g.moveTo(L, y); g.lineTo(L + w, y); g.stroke();
      g.fillStyle = INK.axis;
      g.textAlign = 'right';
      g.fillText(denom ? Math.round(v * 100) + ' %'
                       : (v / 1048576).toFixed(v > 4194304 ? 0 : 1) + ' MiB',
                 L - 8, y);
    }
  }

  /* \~english THE STACK, bottom up and in the legend's order, so a band keeps
   * its place between draws.  The remainder up to the live curve is drawn too,
   * flat grey: a stack that stops where the chosen groups end looks like a
   * program that holds only what was chosen.
   * \~spanish LA PILA, de abajo arriba y en el orden de la leyenda, para que una
   * banda conserve su sitio entre dos dibujos.  El resto hasta la curva de
   * vivos se dibuja tambien, en gris plano: una pila que acaba donde acaban los
   * grupos elegidos parece un programa que solo tiene lo que se eligio.  \~ */
  if (stack && show !== 'committed' && shown.length) {
    var base = eps.map(function () { return 0; });
    shown.slice().reverse().forEach(function (grp) {
      g.fillStyle = timeColor(grp.key);
      g.globalAlpha = 0.85;
      g.beginPath();
      for (var i = 0; i < eps.length; ++i)
        g.lineTo(X(eps[i][TA]), Y(base[i] + valueAt(grp, i)));
      for (var j = eps.length - 1; j >= 0; --j)
        g.lineTo(X(eps[j][TA]), Y(base[j]));
      g.closePath();
      g.fill();
      for (var m = 0; m < eps.length; ++m)
        base[m] += valueAt(grp, m);
    });
    g.globalAlpha = 0.45;
    g.fillStyle = INK.rest;
    g.beginPath();
    for (var q = 0; q < eps.length; ++q)
      g.lineTo(X(eps[q][TA]),
               Y(denom ? 1 : Math.max(base[q], eps[q][TLIVE])));
    for (var r = eps.length - 1; r >= 0; --r)
      g.lineTo(X(eps[r][TA]), Y(base[r]));
    g.closePath();
    g.fill();
    g.globalAlpha = 1;
  } else if (shown.length) {
    // Sin apilar: una linea por grupo, que es como se comparan dos formas.
    shown.forEach(function (grp) {
      g.strokeStyle = timeColor(grp.key);
      g.lineWidth = 1.5;
      g.beginPath();
      eps.forEach(function (e, i) {
        var x = X(e[TA]), y = Y(valueAt(grp, i));
        if (i === 0) g.moveTo(x, y); else g.lineTo(x, y);
      });
      g.stroke();
    });
  }

  function line(col, key, dash) {
    g.strokeStyle = col;
    g.lineWidth = 2;
    g.setLineDash(dash || []);
    g.beginPath();
    eps.forEach(function (e, i) {
      var x = X(e[TA]), y = Y(e[key]);
      if (i === 0) g.moveTo(x, y); else g.lineTo(x, y);
    });
    g.stroke();
    g.setLineDash([]);
  }
  /* Con la escala ajustada a lo elegido, los dos totales se salen de la caja
   * por arriba: dibujarlos seria una raya pegada al borde que no dice nada y
   * ademas hace pensar que el eje llega hasta ellos. */
  /* En el modo 100% no se dibuja ninguno de los tres totales: la altura ahi ya
   * no es memoria sino la parte que le toca a cada banda, y una curva de MiB
   * encima de un eje de porcentajes es una cifra leida contra la regla que no
   * es. */
  if (scale !== 'picked' && !denom) {
    if (show !== 'live') line(INK.comm, TCOMM, [5, 4]);
    /* \~english THE ALLOCATOR'S OWN RANGES, between the other two: the distance
     * down to the live curve is the slack, and the distance up to the committed
     * one is everything that is not this allocator -- the image, the stacks,
     * whatever another library reserved.  Only drawn when the run measured it:
     * a flat zero would read as "the allocator holds nothing", which is the
     * opposite of "nobody asked".
     * \~spanish LOS RANGOS DEL PROPIO ASIGNADOR, entre las otras dos: lo que va
     * de aqui a la curva de vivos es la holgura, y lo que va de aqui a la de
     * comprometido es todo lo que no es este asignador -- la imagen, las pilas,
     * lo que reservara otra libreria.  Solo se dibuja si la corrida lo midio:
     * un cero plano se leeria como "el asignador no tiene nada", que es lo
     * contrario de "no se pregunto".  \~ */
    if (show !== 'live' && timeHasRegion()) line(INK.region, TREG, [2, 3]);
    /* \~english AND THE TWO HALVES OF THE GAP, as one curve drawn at live PLUS
     * the free spans.  Its distance DOWN to the live curve is what is sitting
     * in the free lists -- memory nobody is using, which could go back to the
     * system -- and its distance UP to the region is chunks a size class holds
     * with most of their blocks free, which no amount of giving pages back
     * would recover.  Drawn cumulative and not on its own precisely so those
     * two distances can be read off the chart instead of subtracted by hand:
     * measured on a compile of 144.000 lines the split was 197 MiB against 664,
     * and reading it the other way round is a week spent on the wrong cure.
     * \~spanish Y LAS DOS MITADES DEL HUECO, como una curva dibujada en lo vivo
     * MAS los tramos libres.  Lo que va de ella HACIA ABAJO, a la curva de
     * vivos, es lo que esta parado en las listas de libres -- memoria que no
     * usa nadie y que podria volver al sistema -- y lo que va HACIA ARRIBA,
     * hasta la region, son trozos que una clase de tamano retiene con casi
     * todos sus bloques libres, que no recupera ninguna devolucion de paginas.
     * Se dibuja acumulada y no suelta justo para que esas dos distancias se
     * lean del grafico en vez de restarse a mano: medido en una compilacion de
     * 144.000 lineas el reparto era 197 MiB contra 664, y leerlo al reves son
     * una semana en la cura equivocada.  \~ */
    if (show !== 'live' && timeHasFree())
      timeSplitCurve(g, eps, X, Y, INK.free, TFREE, [4, 2]);
    /* Y LA TERCERA: de aqui a la region queda solo lo que NINGUN asignador
     * puede recuperar -- trozos con unos pocos bloques vivos repartidos --,
     * porque lo de debajo ya son las dos formas que si vuelven. */
    if (show !== 'live' && timeHasEmpty())
      timeSplitCurve(g, eps, X, Y, INK.empty, TEMPTY, [6, 3]);
    if (show !== 'committed') line(INK.live, TLIVE);
  }

  /* \~english THE PHASES, as vertical rules with their name.  A curve without
   * them says "the peak is at cut 251 of 289", which is a fact nobody can act
   * on; with them it says the peak is in the emitter.
   * \~spanish LAS FASES, como lineas verticales con su nombre.  Una curva sin
   * ellas dice "el pico esta en el corte 251 de 289", que es un dato con el que
   * nadie puede hacer nada; con ellas dice que el pico esta en el emisor.  \~ */
  g.textAlign = 'left';
  var lastLabel = -1e9;
  eps.forEach(function (e) {
    if (!e[TMARK]) return;
    var x = X(e[TA]);
    /* \~english THE RULE ALWAYS, THE NAME ONLY IF IT FITS.  Two phases a few
     * pixels apart printed one name on top of the other and neither could be
     * read -- which is worse than one name, because it looks like a third
     * word nobody wrote.  The rule still marks where the phase changed.
     * \~spanish LA LINEA SIEMPRE, EL NOMBRE SOLO SI CABE.  Dos fases a unos
     * pixeles imprimian un nombre encima del otro y no se leia ninguno -- que
     * es peor que uno solo, porque parece una tercera palabra que no escribio
     * nadie.  La linea sigue marcando donde cambio la fase.  \~ */
    var room = x - lastLabel > 90;
    g.strokeStyle = INK.markline;
    g.lineWidth = 1;
    g.setLineDash([2, 3]);
    g.beginPath(); g.moveTo(x, Tp); g.lineTo(x, Tp + h); g.stroke();
    g.setLineDash([]);
    if (!room) return;
    lastLabel = x;
    g.save();
    g.translate(x + 4, Tp + 4);
    g.fillStyle = INK.mark;
    g.textBaseline = 'top';
    g.fillText(timeMark(e[TMARK]), 0, 0);
    g.restore();
  });

  // El punto del pico marca lo VIVO, asi que en el modo 100% no tiene donde
  // ponerse: alli lo vivo es el techo en todos los cortes.
  for (var p = 0; p < eps.length && !denom; ++p) {
    if (eps[p][TI] !== TIME.peakEpoch) continue;
    g.fillStyle = '#e55';
    g.beginPath(); g.arc(X(eps[p][TA]), Y(eps[p][TLIVE]), 4, 0, 6.284); g.fill();
    break;
  }

  if (THOVER >= 0 && THOVER < eps.length) {
    var hx = X(eps[THOVER][TA]);
    g.strokeStyle = INK.rule;
    g.lineWidth = 1;
    g.beginPath(); g.moveTo(hx, Tp); g.lineTo(hx, Tp + h); g.stroke();
  }
  // La banda que se esta arrastrando para ampliar.
  if (TDRAG && TDRAG.to !== null) {
    var a = Math.min(TDRAG.from, TDRAG.to), b = Math.max(TDRAG.from, TDRAG.to);
    g.fillStyle = INK.band;
    g.beginPath();
    g.moveTo(X(a), Tp); g.lineTo(X(b), Tp);
    g.lineTo(X(b), Tp + h); g.lineTo(X(a), Tp + h);
    g.closePath(); g.fill();
  }

  g.fillStyle = INK.axis;
  g.textBaseline = 'top';
  g.textAlign = 'left';
  g.fillText(num(x0), L, Tp + h + 8);
  g.textAlign = 'right';
  g.fillText(T('time.xaxis', { n: num(x1) }), L + w, Tp + h + 8);

  /* LO QUE EL EJE YA NO ES, dicho debajo de el.  Un eje logaritmico y uno de
   * porcentajes se parecen a uno de MiB lo bastante como para leerse mal, y el
   * unico sitio donde eso se puede avisar es donde estan las cifras. */
  if (how === 'log' || denom) {
    g.textAlign = 'center';
    g.fillText(denom ? T('time.pctaxis')
                     : T('time.nolog', { v: human(floor) }),
               L + w / 2, Tp + h + 20);
  }

  timeLegend();
}

/**
 * \~english One chart per band, each with its OWN vertical ruler.
 *
 * WHAT IT IS FOR.  Stacked or overlaid, a band worth two megabytes next to one
 * worth seventy is a hairline: the big one decides the scale and every question
 * about the small ones goes unanswered.  Giving each band the full height of
 * its own little box makes all of them equally readable -- which is the whole
 * point, and also the price: two boxes CANNOT be compared by eye any more,
 * because each has a different ruler.  That is why the peak is written on every
 * one of them, in figures, right where the shape is.
 *
 * The time axis is SHARED, so a rise at the same place in two boxes is a rise
 * at the same moment; the phase rules are drawn across all of them for the
 * same reason.
 *
 * \~spanish Un grafico por banda, cada uno con SU regla vertical.
 *
 * PARA QUE.  Apiladas o superpuestas, una banda de dos megas al lado de una de
 * setenta es un pelo: la grande decide la escala y toda pregunta sobre las
 * pequenas se queda sin contestar.  Darle a cada banda la altura entera de su
 * cajita las hace todas igual de legibles -- que es de lo que se trata, y
 * tambien lo que cuesta: dos cajas ya NO se pueden comparar a ojo, porque cada
 * una tiene otra regla.  Por eso el pico va escrito en todas, en cifras, justo
 * donde esta la forma.
 *
 * El eje del tiempo es COMUN, asi que una subida en el mismo sitio de dos cajas
 * es una subida en el mismo momento; las lineas de fase se dibujan cruzandolas
 * todas por la misma razon.  \~
 */
function timeDrawGrid(g, INK, eps, shown, L, R, Tp, B, W, H) {
  var w = W - L - R;
  var x0 = eps[0][TA], x1 = eps[eps.length - 1][TA];
  if (x1 <= x0) x1 = x0 + 1;
  function X(a) { return L + (a - x0) / (x1 - x0) * w; }

  g.font = '11px ui-monospace,Menlo,Consolas,monospace';
  if (!shown.length) {
    g.fillStyle = INK.axis;
    g.textAlign = 'center';
    g.textBaseline = 'middle';
    g.fillText(T('time.nopick'), L + w / 2, H / 2);
    timeLegend();
    return;
  }

  var rowH = 46, pad = 6;
  shown.forEach(function (grp, row) {
    var yTop = Tp + row * rowH;
    var hh = rowH - pad;
    var peak = grp.peak || 1;

    // La linea de base de cada caja, que es lo que separa una banda de la
    // siguiente sin tener que dibujar un marco alrededor de cada una.
    g.strokeStyle = INK.grid;
    g.lineWidth = 1;
    g.beginPath();
    g.moveTo(L, yTop + hh); g.lineTo(L + w, yTop + hh);
    g.stroke();

    g.fillStyle = timeColor(grp.key);
    g.globalAlpha = 0.75;
    g.beginPath();
    g.moveTo(L, yTop + hh);
    eps.forEach(function (e) {
      g.lineTo(X(e[TA]), yTop + hh - ((grp.at[e[TI]] || 0) / peak) * hh);
    });
    g.lineTo(X(eps[eps.length - 1][TA]), yTop + hh);
    g.closePath();
    g.fill();
    g.globalAlpha = 1;

    g.textBaseline = 'top';
    g.textAlign = 'right';
    g.fillStyle = INK.axis;
    // El nombre a la IZQUIERDA, en el hueco del eje, y el pico dentro de la
    // caja: asi la columna de nombres se lee de arriba abajo de una pasada.
    g.fillText(timeClip(grp.label, 11), L - 8, yTop + 2);
    g.textAlign = 'left';
    g.fillText(human(peak), L + 4, yTop + 2);
  });

  // Las fases, cruzando la rejilla entera: en una caja sola no dirian cuando.
  var bottom = Tp + shown.length * rowH - pad;
  g.strokeStyle = INK.markline;
  g.setLineDash([2, 3]);
  eps.forEach(function (e) {
    if (!e[TMARK]) return;
    var x = X(e[TA]);
    g.beginPath(); g.moveTo(x, Tp); g.lineTo(x, bottom); g.stroke();
  });
  g.setLineDash([]);

  if (THOVER >= 0 && THOVER < eps.length) {
    g.strokeStyle = INK.rule;
    g.beginPath();
    g.moveTo(X(eps[THOVER][TA]), Tp);
    g.lineTo(X(eps[THOVER][TA]), bottom);
    g.stroke();
  }

  g.fillStyle = INK.axis;
  g.textBaseline = 'top';
  g.textAlign = 'left';
  g.fillText(num(x0), L, bottom + 8);
  g.textAlign = 'right';
  g.fillText(T('time.xaxis', { n: num(x1) }), L + w, bottom + 8);
  timeLegend();
}

/**
 * \~english Who weighs what AT ONE MOMENT, as circles whose AREA is the memory.
 *
 * WHY A MOMENT AND NOT THE RUN.  Because the only figure that can be added up
 * is the one from a single cut: adding each band's own peak would give a total
 * no instant of the run ever reached, which is the mistake this whole panel
 * exists to prevent.  So the picture is of ONE cut -- the peak by default, the
 * hovered one while the pointer is over another view -- and the caption names
 * it.
 *
 * AREA AND NOT RADIUS.  A band twice as big gets twice the ink; using the
 * radius would make it look four times bigger, which is the classic way a
 * bubble chart lies.
 *
 * \~spanish Quien pesa que EN UN INSTANTE, como circulos cuya AREA es la
 * memoria.
 *
 * POR QUE UN INSTANTE Y NO LA CORRIDA.  Porque la unica cifra que se puede
 * sumar es la de un corte: sumar el pico de cada banda daria un total que
 * ningun instante de la corrida alcanzo, que es el error que este panel entero
 * existe para evitar.  Asi que el dibujo es de UN corte -- el del pico por
 * defecto, el señalado mientras el puntero anda por otra vista -- y el pie lo
 * nombra.
 *
 * AREA Y NO RADIO.  Una banda del doble se lleva el doble de tinta; usando el
 * radio pareceria cuatro veces mayor, que es la forma clasica en que un grafico
 * de burbujas miente.  \~
 */
function timeDrawBubbles(g, INK, eps, shown, L, R, Tp, B, W, H) {
  g.font = '11px ui-monospace,Menlo,Consolas,monospace';

  var items = [];
  shown.forEach(function (grp) {
    if (grp.peak > 0)
      items.push({ key: grp.key, label: grp.label, value: grp.peak });
  });
  items.sort(function (a, b) { return b.value - a.value; });

  var cx = L + (W - L - R) / 2, cy = Tp + (H - Tp - B) / 2;
  var box = Math.min(W - L - R, H - Tp - B);
  var total = 0;
  items.forEach(function (it) { total += it.value; });
  if (total <= 0) {
    g.fillStyle = INK.axis;
    g.textAlign = 'center';
    g.textBaseline = 'middle';
    g.fillText(T('time.nopick'), cx, cy);
    timeLegend();
    return;
  }
  /* El area de todos junta ocupa poco mas de la mitad de la caja: empaquetar
   * circulos deja huecos, y apuntar al 100% los saca por los bordes. */
  var kArea = (box * box * 0.30) / total;
  items.forEach(function (it) {
    it.r = Math.max(3, Math.sqrt(it.value * kArea / Math.PI));
  });

  timePack(items, cx, cy, box);
  TBUBBLES = items;

  /* AMPLIAR Y DESPLAZAR, como una transformacion del lienzo y no recolocando
   * los circulos: el empaquetado es el mismo, se mira mas de cerca.  Si se
   * recolocara, ampliar cambiaria el dibujo y dejaria de ser el mismo. */
  g.save();
  g.translate(TVIEW.x, TVIEW.y);
  g.scale(TVIEW.k, TVIEW.k);
  items.forEach(function (it) {
    g.fillStyle = it.key ? timeColor(it.key) : INK.rest;
    g.globalAlpha = 0.85;
    g.beginPath();
    g.arc(it.x, it.y, it.r, 0, 6.284);
    g.fill();
    g.globalAlpha = 1;
    // Lo que cabe depende de lo AMPLIADO: un circulo pequeno con el doble de
    // aumento ya admite su nombre, que es media razon de poder ampliar.
    var shown_r = it.r * TVIEW.k;
    if (shown_r < 13) return;
    g.fillStyle = '#000';
    g.textAlign = 'center';
    g.textBaseline = 'middle';
    g.font = (11 / TVIEW.k) + 'px ui-monospace,Menlo,Consolas,monospace';
    var room = Math.floor(shown_r / 3.2);
    g.fillText(timeClip(it.label, room), it.x,
               it.y - (shown_r > 24 ? 6 / TVIEW.k : 0));
    if (shown_r > 24) g.fillText(human(it.value), it.x, it.y + 7 / TVIEW.k);
  });
  g.restore();
  g.font = '11px ui-monospace,Menlo,Consolas,monospace';

  /* \~english AND IT SAYS THEY ARE NOT SIMULTANEOUS.  Each circle is that
   * band's own peak -- the same figure the legend and the picker show -- so
   * they cannot be added up: two bands that were never live at the same time
   * would make a total no instant of the run ever reached.  What the peak WAS
   * made of is a different question, and the table under the chart answers it.
   * \~spanish Y SE DICE QUE NO SON A LA VEZ.  Cada circulo es el pico de SU
   * banda -- la misma cifra que ensenan la leyenda y el selector --, asi que no
   * se pueden sumar: dos bandas que nunca estuvieron vivas a la vez darian un
   * total que ningun instante de la corrida alcanzo.  De que estaba hecho el
   * pico es otra pregunta, y la contesta la tabla de debajo del grafico.  \~ */
  g.fillStyle = INK.axis;
  g.textAlign = 'left';
  g.textBaseline = 'top';
  g.fillText(T('time.bubbles.peaks', { n: items.length }), L, H - B + 6);
  timeLegend();
}

/**
 * \~english Places circles around a centre without overlapping, biggest first.
 *
 * A SPIRAL AND NOT A LIBRARY.  A proper circle packer is a page of geometry;
 * this walks outwards from the middle and takes the first spot where the circle
 * fits.  With the forty bands the picker can show, the difference is a few
 * pixels of slack -- and the page has to work opened from a disc with no
 * network, where a library is not an option.
 *
 * \~spanish Coloca circulos alrededor de un centro sin solaparse, el mayor
 * primero.
 *
 * UNA ESPIRAL Y NO UNA LIBRERIA.  Un empaquetador de circulos de verdad es una
 * pagina de geometria; esto sale del centro hacia fuera y se queda en el primer
 * sitio donde el circulo cabe.  Con las cuarenta bandas que el selector puede
 * ensenar, la diferencia son unos pixeles de holgura -- y la pagina tiene que
 * funcionar abierta de un disco sin red, donde una libreria no es una opcion.
 * \~
 */
function timePack(items, cx, cy, box) {
  var placed = [];
  items.forEach(function (it) {
    if (!placed.length) {
      it.x = cx;
      it.y = cy;
      placed.push(it);
      return;
    }
    var step = Math.max(2, it.r / 2);
    for (var radius = 0; radius < box; radius += step) {
      var turns = Math.max(8, Math.floor((2 * Math.PI * radius) / step));
      for (var k = 0; k < turns; ++k) {
        var ang = (k / turns) * 2 * Math.PI + radius * 0.7;
        var x = cx + Math.cos(ang) * radius;
        var y = cy + Math.sin(ang) * radius;
        var fits = true;
        for (var i = 0; i < placed.length; ++i) {
          var dx = placed[i].x - x, dy = placed[i].y - y;
          if (dx * dx + dy * dy < (placed[i].r + it.r + 1) *
                                  (placed[i].r + it.r + 1)) {
            fits = false;
            break;
          }
        }
        if (fits) {
          it.x = x;
          it.y = y;
          placed.push(it);
          return;
        }
      }
    }
    // No cabia en ningun sitio: se pone en el centro antes que desaparecer.
    it.x = cx;
    it.y = cy;
    placed.push(it);
  });
}

/// Un texto que quepa en `n` caracteres, cortando por el final con puntos.
function timeClip(text, n) {
  text = String(text);
  if (n < 3) return '';
  return text.length <= n ? text : text.slice(0, n - 1) + '…';
}

/**
 * \~english A colour from the stylesheet, so the chart follows the theme.
 *
 * A canvas has no cascade: what is painted is painted, and a colour written
 * into the drawing code is one no theme can reach -- dark ink on dark paper,
 * and the curve is simply gone.  Read back from the document instead, with a
 * fallback for where there is no layout to ask (the tests run on a DOM that
 * computes nothing, and a chart that throws there is a chart nothing checks).
 *
 * \~spanish Un color de la hoja de estilos, para que el grafico siga al tema.
 *
 * Un lienzo no tiene cascada: lo pintado, pintado esta, y un color escrito en
 * el codigo de dibujo es uno al que ningun tema llega -- tinta oscura sobre
 * papel oscuro, y la curva simplemente no esta.  Se relee del documento, con un
 * valor de respaldo para donde no hay maquetacion a la que preguntar (las
 * pruebas corren sobre un DOM que no calcula nada, y un grafico que revienta
 * alli es un grafico que no comprueba nadie).  \~
 */
function timeInk(name, fallback) {
  if (typeof window.getComputedStyle !== 'function') return fallback;
  var v = window.getComputedStyle(document.documentElement)
                .getPropertyValue(name);
  return (v && v.trim()) || fallback;
}

/// A phase key in the reader's language, or the key itself when nobody has
/// translated it -- a label nobody wrote beats a blank.
function timeMark(key) {
  var text = T('mark.' + key);
  return text === '[mark.' + key + ']' ? key : text;
}

/**
 * \~english WHO CALLED WHO, for one series: the whole chain, not its innermost
 * frame.
 *
 * THE NAME ALONE ANSWERS THE WRONG QUESTION.  `operator new` inside a vector
 * inside a pass says WHAT allocated and never DE DONDE -- and the second one is
 * what a reader came for.  The chain is in the export already, one row per
 * (stack, frame, depth), and the page was showing one frame out of ten.
 *
 * A grouped series is several stacks, so they are all listed, biggest first:
 * "this function holds 50 MiB" is one answer, "from these four places" is the
 * one that can be acted on.
 *
 * \~spanish QUIEN LLAMO A QUIEN, de una serie: la cadena entera, no su marco
 * mas interior.
 *
 * EL NOMBRE SOLO CONTESTA LA PREGUNTA EQUIVOCADA.  `operator new` dentro de un
 * vector dentro de un pase dice QUE reservo y nunca DESDE DONDE -- y lo segundo
 * es a lo que se viene.  La cadena ya esta en la exportacion, una fila por
 * (pila, marco, profundidad), y la pagina ensenaba un marco de diez.
 *
 * Una serie agrupada son varias pilas, asi que se listan todas, la mayor
 * primero: "esta funcion tiene 50 MiB" es una respuesta, "desde estos cuatro
 * sitios" es la que se puede usar.  \~
 */
/**
 * \~english DE QUE FICHEROS es una banda, medido corte a corte.
 *
 * WHY IT IS NEEDED.  Grouping by shape or by lifetime answers "what KIND of
 * memory this is" -- the kind that dies in its phase against the kind that
 * outlives everything -- and that is a real question, but on its own it leads
 * nowhere: a band called "lived under 10 allocations, 145 MiB" says nothing
 * about which code to go and look at.  The sites are right there in the band;
 * what was missing was adding them up by where they come from.
 *
 * CUT BY CUT AND NOT BY TOTALS, which is the same rule the bands themselves
 * follow.  Two stacks of the same file that were never live at the same time
 * must not add up to a figure neither of them ever reached; the peak of a file
 * is the highest its OWN curve got, not the sum of its sites' peaks.
 *
 * \~spanish DE QUE FICHEROS es una banda, medido corte a corte.
 *
 * POR QUE HACE FALTA.  Agrupar por forma o por vida contesta que CLASE de
 * memoria es -- la que muere en su fase frente a la que sobrevive a todo --, y
 * esa es una pregunta de verdad, pero por si sola no lleva a ningun sitio: una
 * banda que dice "vivieron menos de 10 reservas, 145 MiB" no dice a que codigo
 * hay que ir a mirar.  Los sitios estan ahi dentro; lo que faltaba era sumarlos
 * por de donde vienen.
 *
 * CORTE A CORTE Y NO POR TOTALES, que es la regla que siguen las bandas mismas.
 * Dos pilas del mismo fichero que nunca estuvieron vivas a la vez no pueden
 * sumar una cifra que ninguna alcanzo; el pico de un fichero es lo mas alto que
 * llego SU curva, no la suma de los picos de sus sitios.  \~
 *
 * @return [{ path, name, peak, sids }], de mayor a menor.
 */
function timeBandFiles(grp) {
  var byPath = {};
  grp.sids.forEach(function (sid) {
    var frame = timeOwnFrame(sid);
    var path = (frame && frame[1]) || '';
    var entry = byPath[path];
    if (!entry) {
      entry = byPath[path] = { path: path, at: {}, sids: 0, peak: 0 };
    }
    entry.sids += 1;
    (TIME.series[String(sid)] || []).forEach(function (p) {
      entry.at[p[0]] = (entry.at[p[0]] || 0) + p[1];
    });
  });
  var out = [];
  Object.keys(byPath).forEach(function (path) {
    var entry = byPath[path];
    Object.keys(entry.at).forEach(function (cut) {
      if (entry.at[cut] > entry.peak) entry.peak = entry.at[cut];
    });
    entry.name = path ? path.split(/[\\/]/).pop() : T('time.nofile');
    out.push(entry);
  });
  out.sort(function (a, b) { return b.peak - a.peak; });
  return out;
}

function timeStackDetail(key) {
  var box = document.getElementById('tstack-detail');
  var grp = TBYKEY[key];
  if (!grp) { box.innerHTML = ''; return; }
  var frames = DATA.check.frames;
  var sids = grp.sids.slice().sort(function (a, b) {
    var pa = 0, pb = 0;
    (TIME.series[String(a)] || []).forEach(function (p) {
      if (p[1] > pa) pa = p[1];
    });
    (TIME.series[String(b)] || []).forEach(function (p) {
      if (p[1] > pb) pb = p[1];
    });
    return pb - pa;
  });
  var html = '<div class="h"><i style="background:' + timeColor(key) +
             '"></i><b>' + esc(grp.label) + '</b> · ' +
             esc(T('time.stacks', { n: sids.length })) +
             '<button class="x" id="tstack-close">×</button></div>';
  /* UNA CASILLA NO ES UN SITIO, y se dice en vez de dejar el panel vacio: la
   * curva por tamano sale de un recuento por corte, no de un reparto de las
   * pilas, asi que no hay cadena que abrir.  Un panel vacio se leeria como
   * "estas pilas no se pudieron leer", que es otra cosa. */
  if (!sids.length) {
    box.innerHTML = html + '<div class="sh">' + esc(T('time.nostacks')) +
                    '</div>';
    var closeIt = document.getElementById('tstack-close');
    if (closeIt) closeIt.onclick = function () { box.innerHTML = ''; };
    return;
  }
  /* DE QUE FICHEROS, ANTES QUE LAS PILAS.  Una banda de forma o de vida reune
   * sitios de todas partes, y ocho cadenas de llamadas sueltas no dicen de
   * donde sale la banda -- contestan por ocho sitios de los cientos que hay.
   * El reparto por fichero si contesta por la banda entera, y es lo que dice a
   * que codigo ir. */
  /* SIEMPRE, tambien cuando sale UNO.  Se penso en callarlo ahi por redundante
   * -- el fichero ya va en cada marco de la cadena de abajo --, y es peor: la
   * linea de arriba contesta sin leer ocho cadenas, y una vista que solo
   * aparece a veces no se aprende ni se prueba. */
  var files = timeBandFiles(grp);
  if (files.length) {
    html += '<div class="files"><div class="sh">' +
            esc(T('time.byfile', { n: files.length })) + '</div><ol>';
    files.slice(0, 8).forEach(function (f) {
      html += '<li><span class="n">' + human(f.peak) + '</span>' +
              '<span class="path" title="' + esc(f.path) + '">' +
              esc(f.name) + '</span>' +
              '<span class="sh">' + esc(T('time.stacks', { n: f.sids })) +
              '</span></li>';
    });
    if (files.length > 8)
      html += '<li class="sh">' +
              esc(T('time.more', { n: files.length - 8 })) + '</li>';
    html += '</ol></div>';
  }

  sids.slice(0, 8).forEach(function (sid) {
    var site = TIME.byId[sid];
    if (!site) return;
    var peak = 0;
    (TIME.series[String(sid)] || []).forEach(function (p) {
      if (p[1] > peak) peak = p[1];
    });
    html += '<div class="one"><div class="sh">' + human(peak) + ' · #' + sid +
            '</div><ol class="chain">';
    /* \~english OUTERMOST FIRST, which is the order a person reads a call in:
     * main called this, which called that.  The export writes it innermost
     * first because that is the order a stack walk produces, and printing it
     * that way makes the reader run it backwards in their head.
     * \~spanish DE FUERA HACIA DENTRO, que es el orden en el que una persona
     * lee una llamada: main llamo a esto, que llamo a aquello.  La exportacion
     * lo escribe de dentro hacia fuera porque es el orden en que sale un
     * recorrido de pila, y ensenarlo asi obliga a leerlo al reves.  \~ */
    for (var i = site.chain.length - 1; i >= 0; --i) {
      var f = frames[site.chain[i]];
      if (!f) continue;
      var cls = f[6] ? 'instr' : f[7] ? 'start' : f[5] ? 'lib' : 'mine';
      html += '<li class="' + cls + (f[4] ? ' inl' : '') + '">' +
              '<span class="sym">' + codeHtml(f[0], '', langOf(f[1], f[0])) +
              '</span>' +
              (f[1] ? '<span class="path">' + esc(f[1].split(/[\\/]/).pop()) +
                      ':' + f[2] + '</span>' : '') + '</li>';
    }
    html += '</ol></div>';
  });
  if (sids.length > 8)
    html += '<div class="sh">' + esc(T('time.more', { n: sids.length - 8 })) +
            '</div>';
  box.innerHTML = html;
  var close = document.getElementById('tstack-close');
  if (close) close.addEventListener('click', function () {
    box.innerHTML = '';
  });
}

/// The legend: what is on the chart.  A click opens its call chain, which is
/// the question a name on its own cannot answer.
function timeLegend() {
  var box = document.getElementById('tlegend');
  var html = '';
  timeShown().forEach(function (grp) {
    html += '<button class="lg" data-key="' + esc(grp.key) + '">' +
            '<i style="background:' + timeColor(grp.key) + '"></i>' +
            '<span class="sym">' + esc(grp.label) + '</span>' +
            '<span class="w">' + human(grp.peak) + '</span></button>';
  });
  if (TZOOM)
    html += '<button class="lg back" id="tunzoom">' +
            esc(T('time.unzoom')) + '</button>';
  box.innerHTML = html;
  box.querySelectorAll('.lg[data-key]').forEach(function (b) {
    b.addEventListener('click', function () {
      timeStackDetail(b.dataset.key);
    });
  });
  var un = document.getElementById('tunzoom');
  if (un) un.addEventListener('click', function () {
    TZOOM = null;
    timeDraw();
  });
}

/**
 * \~english The picker: everything that could go on the chart, and what is on
 * it.  This is the control the panel is really about -- a chart that picks its
 * own ten series answers one question and hides the rest.
 * \~spanish El selector: todo lo que podria entrar en el grafico, y lo que
 * esta.  Este es el control del que va de verdad el panel -- un grafico que
 * elige sus propias diez series contesta una pregunta y esconde las demas.  \~
 */
function timePicker() {
  var needle = (document.getElementById('tfilter').value || '').toLowerCase();
  var box = document.getElementById('tpick');
  var html = '';
  var hidden = 0;
  TGROUPS.forEach(function (grp) {
    var hay = (grp.label + ' ' + grp.where).toLowerCase();
    if (needle && hay.indexOf(needle) < 0) { hidden++; return; }
    html += '<label class="pk"><input type="checkbox" data-key="' +
            esc(grp.key) + '"' + (TPICK[grp.key] ? ' checked' : '') + '>' +
            '<i style="background:' + timeColor(grp.key) + '"></i>' +
            '<span class="sym">' + esc(grp.label) + '</span>' +
            '<span class="w">' + human(grp.peak) + '</span>' +
            '<span class="path">' + esc(grp.where) + '</span></label>';
  });
  if (hidden)
    html += '<span class="stat">' + esc(T('time.hidden', { n: hidden })) +
            '</span>';
  box.innerHTML = html;
  box.querySelectorAll('input[data-key]').forEach(function (input) {
    input.addEventListener('change', function () {
      if (input.checked) TPICK[input.dataset.key] = true;
      else delete TPICK[input.dataset.key];
      timeDraw();
    });
  });
}

/// What was live at one cut, as a box next to the pointer.
/**
 * \~english The tooltip of a bubble: its name WHOLE, and its share.
 *
 * The circle carries a clipped label when it is big enough and nothing at all
 * when it is not -- and the small ones are most of them.  This is where the
 * name lives, and where a symbol that does not fit in a circle can be read.
 *
 * \~spanish El globo de una burbuja: su nombre ENTERO, y su parte.
 *
 * El circulo lleva el rotulo recortado cuando cabe y nada cuando no -- y los
 * pequenos son casi todos.  Aqui es donde vive el nombre, y donde se puede leer
 * un simbolo que en un circulo no entra.  \~
 */
function timeBubbleTip(item) {
  var tip = document.getElementById('ttip');
  if (!item) { tip.style.display = 'none'; return; }
  var total = 0;
  TBUBBLES.forEach(function (it) { total += it.value; });
  tip.innerHTML =
    '<div class="h"><i style="background:' +
    (item.key ? timeColor(item.key) : 'rgba(128,128,128,.5)') + '"></i>' +
    esc(item.label) + '</div>' +
    '<table><tr><td>' + esc(T('time.live')) + '</td><td class="n">' +
    human(item.value) + '</td></tr>' +
    '<tr><td>' + esc(T('time.share')) + '</td><td class="n">' +
    (total ? (item.value * 100 / total).toFixed(1) : '0') + ' %</td></tr>' +
    '</table>';
  tip.style.display = 'block';
}

function timeTip(index) {
  var tip = document.getElementById('ttip');
  var eps = timeCuts();
  if (index < 0 || !eps[index]) { tip.style.display = 'none'; return; }
  var e = eps[index];
  /* \~english SIX LINES AND NOT FORTY.  A tooltip that lists every band covers
   * the chart it is explaining -- measured on a real export, it was taller than
   * the plot and hid the peak it was pointing at.  The biggest six answer "what
   * is this hill made of"; the rest are a number.
   * \~spanish SEIS LINEAS Y NO CUARENTA.  Un globo que lista todas las bandas
   * tapa el grafico que esta explicando -- medido en una exportacion de verdad,
   * era mas alto que el dibujo y escondia el pico al que senalaba.  Las seis
   * mayores contestan "de que esta hecha esta loma"; el resto es un numero.  \~ */
  var here = timeShown().map(function (grp) {
    return { label: grp.label, key: grp.key, v: grp.at[e[TI]] || 0 };
  }).filter(function (r) { return r.v > 0; });
  here.sort(function (a, b) { return b.v - a.v; });
  var rows = '';
  here.slice(0, 6).forEach(function (r) {
    rows += '<tr><td><i style="background:' + timeColor(r.key) + '"></i>' +
            '<span class="sym">' + esc(r.label) + '</span></td>' +
            '<td class="n">' + human(r.v) + '</td></tr>';
  });
  if (here.length > 6) {
    var rest = 0;
    here.slice(6).forEach(function (r) { rest += r.v; });
    rows += '<tr class="rest"><td>' +
            esc(T('time.more', { n: here.length - 6 })) +
            '</td><td class="n">' + human(rest) + '</td></tr>';
  }
  tip.innerHTML =
    '<div class="h">' + esc(T('time.cut', { i: e[TI], a: num(e[TA]) })) +
    (e[TMARK] ? ' · <b>' + esc(timeMark(e[TMARK])) + '</b>' : '') + '</div>' +
    '<table><tr><td>' + esc(T('time.live')) + '</td><td class="n">' +
    human(e[TLIVE]) + '</td></tr>' +
    '<tr><td>' + esc(T('time.committed')) + '</td><td class="n">' +
    human(e[TCOMM]) + '</td></tr>' +
    /* La holgura al lado de sus dos terminos, y solo cuando se midio: quien
       mira un corte viene justo a esta resta, y hacerla de cabeza entre dos
       filas separadas es lo que hace que no se haga. */
    (e[TREG] ? '<tr><td>' + esc(T('time.region')) + '</td><td class="n">' +
               human(e[TREG]) + '</td></tr>' +
               '<tr><td>' + esc(T('time.slack')) + '</td><td class="n">' +
               human(Math.max(0, e[TREG] - e[TLIVE])) + '</td></tr>'
              : '') +
    /* Y EL HUECO, REPARTIDO.  Las dos mitades tienen curas distintas -- una
       vuelve al sistema y la otra no --, asi que darlas juntas es lo que hace
       que se ataque la que no era. */
    (e[TFREE] ? '<tr><td>' + esc(T('time.freespans')) + '</td><td class="n">' +
                human(e[TFREE]) + '</td></tr>'
               : '') +
    (e[TEMPTY] ? '<tr><td>' + esc(T('time.emptychunks')) +
                 '</td><td class="n">' + human(e[TEMPTY]) + '</td></tr>'
                : '') +
    (e[TFREE] ? '<tr><td>' + esc(T('time.inclasses')) + '</td><td class="n">' +
                human(Math.max(0, e[TREG] - e[TLIVE] - e[TFREE] -
                                  (e[TEMPTY] || 0))) + '</td></tr>'
               : '') +
    '<tr><td>' + esc(T('time.churn')) + '</td><td class="n">+' +
    human(e[TBORN]) + ' / -' + human(e[TDIED]) + '</td></tr>' +
    rows + '</table>';
  tip.style.display = 'block';
}

/// The sites at the peak, as a table under the chart.
function timePeakTable() {
  var body = document.getElementById('tpeakrows');
  var total = 0;
  TIME.epochs.forEach(function (e) {
    if (e[TI] === TIME.peakEpoch) total = e[TLIVE];
  });
  var at = DATA.check ? DATA.check.siteCols.indexOf('growth') : -1;
  var html = '';
  TIME.peak.forEach(function (row) {
    var sid = row[0], site = TIME.byId[sid];
    var growth = (site && at >= 0) ? (site.row[at] || '') : '';
    html += '<tr data-sid="' + sid + '">' +
            '<td class="n">' + human(row[1]) + '</td>' +
            '<td class="n">' + (row[2] / 10).toFixed(1) + '%</td>' +
            '<td>' + esc(growth) + '</td>' +
            '<td class="name"><span class="sym">' + esc(timeName(sid)) +
            '</span></td>' +
            '<td><span class="path">' + esc(timeWhere(sid)) + '</span></td>' +
            '</tr>';
  });
  body.innerHTML = html;
  /* Una fila del pico tambien abre su cadena: es la tabla por la que se entra
   * al panel, y quedarse en el nombre es quedarse a medias. */
  body.querySelectorAll('tr[data-sid]').forEach(function (tr) {
    tr.addEventListener('click', function () {
      var key = 's:' + tr.dataset.sid;
      if (!TBYKEY[key]) {
        // Agrupado por otra cosa: se busca el grupo que contiene esa pila.
        for (var i = 0; i < TGROUPS.length; ++i)
          if (TGROUPS[i].sids.indexOf(tr.dataset.sid | 0) >= 0) {
            key = TGROUPS[i].key;
            break;
          }
      }
      timeStackDetail(key);
    });
  });
  document.getElementById('tpeakhead').textContent =
    T('time.peakhead', { bytes: human(total),
                         mark: TIME.peakMark ? timeMark(TIME.peakMark)
                                             : T('time.nomark') });
}

/// Where the pointer is, in allocations.
function timeAt(canvas, clientX) {
  var r = canvas.getBoundingClientRect();
  var eps = timeCuts();
  var L = 74, R = 14;
  var w = r.width - L - R;
  var x0 = eps[0][TA], x1 = eps[eps.length - 1][TA];
  if (x1 <= x0) x1 = x0 + 1;
  return x0 + (clientX - r.left - L) / w * (x1 - x0);
}

/// Rebuilds everything downstream of the grain, keeping what is still there
/// switched on: changing the grouping must not throw a selection away.
function timeRegroup(keepPick) {
  timeBuild();
  if (!keepPick || !TPICK) timePickTop();
  else {
    var kept = {};
    Object.keys(TPICK).forEach(function (k) {
      if (TBYKEY[k]) kept[k] = true;
    });
    TPICK = Object.keys(kept).length ? kept : (timePickTop(), TPICK);
  }
  timePicker();
  timeDraw();
}

/**
 * \~english Wires the panel up.  Does nothing when the run had no time axis,
 * and the tab is not offered either: a control that offers something the file
 * does not carry is a promise the page cannot keep.
 * \~spanish Engancha el panel.  No hace nada si la corrida no llevaba eje del
 * tiempo, y entonces la pestana tampoco se ofrece: un control que ofrece algo
 * que el fichero no trae es una promesa que la pagina no puede cumplir.  \~
 */
function timeInit() {
  TIME = DATA.time || null;
  var tab = document.querySelector('[data-panel="p-time"]');
  if (!TIME || !TIME.epochs || !TIME.epochs.length) {
    if (tab) tab.style.display = 'none';
    return;
  }
  TIME.byId = {};
  (DATA.check ? DATA.check.sites : []).forEach(function (s) {
    TIME.byId[s.id] = s;
  });

  document.getElementById('tgroup').addEventListener('change', function () {
    // Otro grano son otras claves: lo elegido no sobrevive, y volver al top
    // es mas honesto que dejar el grafico vacio sin decir por que.
    TPICK = null;
    timeRegroup(false);
  });
  document.getElementById('tmine').addEventListener('change', function () {
    timeRegroup(true);
  });
  document.getElementById('ttop').addEventListener('change', function () {
    timePickTop();
    timePicker();
    timeDraw();
  });
  ['tseries', 'tdraw', 'tscale'].forEach(function (id) {
    document.getElementById(id).addEventListener('change', timeDraw);
  });
  document.getElementById('tfilter').addEventListener('input', function () {
    timePicker();
  });
  document.getElementById('tall').addEventListener('click', function () {
    var needle = (document.getElementById('tfilter').value || '').toLowerCase();
    TGROUPS.forEach(function (grp) {
      var hay = (grp.label + ' ' + grp.where).toLowerCase();
      if (!needle || hay.indexOf(needle) >= 0) TPICK[grp.key] = true;
    });
    timePicker();
    timeDraw();
  });
  document.getElementById('tnone').addEventListener('click', function () {
    TPICK = {};
    timePicker();
    timeDraw();
  });
  window.addEventListener('resize', timeDraw);

  var canvas = document.getElementById('tchart');
  canvas.addEventListener('mousemove', function (ev) {
    var eps = timeCuts();
    if (TPAN) {
      var rp = canvas.getBoundingClientRect();
      var nx = ev.clientX - rp.left, ny = ev.clientY - rp.top;
      timePan(nx - TPAN.x, ny - TPAN.y);
      TPAN = { x: nx, y: ny };
      return;
    }
    /* EN BURBUJAS LA PREGUNTA ES OTRA: no "en que corte estoy" -- ahi solo hay
     * uno -- sino "sobre que circulo".  Sin esto, el puntero contestaba por el
     * eje del tiempo en una vista que no lo tiene, y los circulos pequenos, que
     * son los que no caben rotulados, se quedaban sin nombre. */
    if (timeHow() === 'bubbles') {
      var r0 = canvas.getBoundingClientRect();
      /* AL REVES DE LA TRANSFORMACION, porque los circulos estan guardados sin
       * ampliar: preguntar por el puntero tal cual acertaria en el sitio donde
       * el circulo estaria si no se hubiera ampliado, que es cualquier otro. */
      var mx = (ev.clientX - r0.left - TVIEW.x) / TVIEW.k;
      var my = (ev.clientY - r0.top - TVIEW.y) / TVIEW.k;
      var over = null;
      for (var b = 0; b < TBUBBLES.length; ++b) {
        var it = TBUBBLES[b];
        var ddx = it.x - mx, ddy = it.y - my;
        if (ddx * ddx + ddy * ddy <= it.r * it.r) { over = it; break; }
      }
      // El globo se coloca en PIXELES de pantalla, no en las coordenadas sin
      // ampliar de arriba: son dos sistemas distintos y mezclarlos pone el
      // globo en cualquier sitio menos donde esta el puntero.
      var tip0 = document.getElementById('ttip');
      tip0.style.left =
        Math.min(r0.width - 340, ev.clientX - r0.left + 14) + 'px';
      tip0.style.top = (ev.clientY - r0.top + 12) + 'px';
      timeBubbleTip(over);
      return;
    }
    var a = timeAt(canvas, ev.clientX);
    if (TDRAG) {
      TDRAG.to = a;
      timeDraw();
      return;
    }
    var best = -1, bestd = Infinity;
    for (var i = 0; i < eps.length; ++i) {
      var d = Math.abs(eps[i][TA] - a);
      if (d < bestd) { bestd = d; best = i; }
    }
    THOVER = best;
    var r = canvas.getBoundingClientRect();
    var tip = document.getElementById('ttip');
    tip.style.left = Math.min(r.width - 260, ev.clientX - r.left + 14) + 'px';
    tip.style.top = (ev.clientY - r.top + 12) + 'px';
    timeTip(best);
    timeDraw();
  });
  /* \~english DRAG TO ZOOM, because the interesting stretch of a run is
   * usually a tenth of it and a curve squeezed into a tenth of the width
   * cannot be read.  A drag that goes nowhere is not a zoom into nothing: it
   * is a click, and it is ignored.
   * \~spanish ARRASTRAR PARA AMPLIAR, porque el tramo interesante de una
   * corrida suele ser una decima parte y una curva metida en una decima parte
   * del ancho no se lee.  Un arrastre que no va a ningun sitio no es una
   * ampliacion a nada: es un clic, y se ignora.  \~ */
  /* \~english DRAGGING MOVES, and picking a stretch is the same drag with
   * shift held.  It used to be the other way round -- a plain drag picked a
   * stretch -- and that left no way to move once zoomed in: to see the next
   * stretch you had to go back to the whole run and pick again.  Moving is the
   * thing done constantly and picking the thing done once, so moving gets the
   * plain gesture.
   * \~spanish ARRASTRAR DESPLAZA, y elegir un tramo es el mismo arrastre con
   * mayusculas.  Era al reves -- un arrastre a secas elegia un tramo -- y eso
   * dejaba sin forma de MOVERSE una vez ampliado: para ver el tramo siguiente
   * habia que volver a la corrida entera y elegir otra vez.  Desplazarse es lo
   * que se hace a todas horas y elegir lo que se hace una vez, asi que el
   * gesto a secas es el de desplazarse.  \~ */
  canvas.addEventListener('mousedown', function (ev) {
    if (ev.shiftKey && timeHow() !== 'bubbles') {
      TDRAG = { from: timeAt(canvas, ev.clientX), to: null };
      return;
    }
    var r = canvas.getBoundingClientRect();
    TPAN = { x: ev.clientX - r.left, y: ev.clientY - r.top };
  });
  canvas.addEventListener('mouseup', function (ev) {
    TPAN = null;
    if (!TDRAG) return;
    var to = timeAt(canvas, ev.clientX);
    var from = TDRAG.from;
    TDRAG = null;
    var eps = timeCuts();
    var span = eps[eps.length - 1][TA] - eps[0][TA];
    if (Math.abs(to - from) > span / 50) {
      TZOOM = { from: Math.min(from, to), to: Math.max(from, to) };
      // Menos de dos cortes dentro no es una ampliacion, es un grafico vacio.
      if (timeCuts().length < 2) TZOOM = null;
    }
    timeDraw();
  });
  /* LA RUEDA AMPLIA DONDE APUNTA, que es lo que se espera de una rueda sobre un
   * grafico; y se le quita el desplazamiento de la pagina, porque si no la
   * pagina se mueve debajo mientras se amplia. */
  canvas.addEventListener('wheel', function (ev) {
    if (ev.preventDefault) ev.preventDefault();
    var r = canvas.getBoundingClientRect();
    timeZoom(ev.deltaY < 0 ? 1.25 : 1 / 1.25,
             ev.clientX - r.left, ev.clientY - r.top);
  });
  canvas.addEventListener('dblclick', timeZoomAll);
  document.getElementById('tzin').addEventListener('click', function () {
    timeZoom(1.5, null, null);
  });
  document.getElementById('tzout').addEventListener('click', function () {
    timeZoom(1 / 1.5, null, null);
  });
  document.getElementById('tzall').addEventListener('click', timeZoomAll);
  /* Y LA CAJA SE PUEDE ESTIRAR: el navegador la redimensiona solo, pero el
   * lienzo no se entera -- lo dibujado se queda del tamano que tenia y sale
   * estirado o cortado.  `ResizeObserver` es lo que cierra eso; donde no lo
   * haya, queda el redibujado por cambio de ventana. */
  var chartbox = canvas.parentNode;
  if (typeof ResizeObserver === 'function' && chartbox) {
    new ResizeObserver(function () { timeDraw(); }).observe(chartbox);
  }
  canvas.addEventListener('mouseleave', function () {
    THOVER = -1;
    TDRAG = null;
    // Y se suelta el desplazamiento: si no, sacar el puntero del grafico con el
    // boton apretado lo deja arrastrando para siempre.
    TPAN = null;
    timeTip(-1);
    timeDraw();
  });

  timePeakTable();
  timeRegroup(false);
}
