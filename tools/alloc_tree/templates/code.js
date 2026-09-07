/*
 * VestaVM -- Distributed Virtual Machine
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * License: MIT (see LICENSE).  Part of the VestaVM family.
 *
 * How a symbol is PAINTED.  Nothing here removes a character: a demangled C++
 * name runs to four hundred of them and every one is needed to tell two
 * instantiations of the same template apart.  What it does is make the shape
 * visible -- the function's own name against its qualifiers, its template
 * arguments and its parameter list -- so the eye can find the part it came
 * for without the name being cut.
 *
 * And it says which LANGUAGE the code is, because "who allocated this" reads
 * very differently when the answer is a C library carried inside the project
 * and when it is the standard C++ library.
 */
'use strict';

var KEYWORDS = {
  'void': 1, 'bool': 1, 'char': 1, 'short': 1, 'int': 1, 'long': 1,
  'unsigned': 1, 'signed': 1, 'float': 1, 'double': 1, 'const': 1,
  'volatile': 1, 'static': 1, 'inline': 1, 'auto': 1, 'struct': 1,
  'class': 1, 'enum': 1, 'union': 1, 'operator': 1, 'noexcept': 1,
  'size_t': 1, 'wchar_t': 1, 'nullptr_t': 1
};

var C_EXT = { c: 1 };
var CPP_EXT = { cpp: 1, cc: 1, cxx: 1, 'c++': 1, hpp: 1, hh: 1, hxx: 1, tcc: 1, ipp: 1 };

/**
 * C or C++, and it SAYS when it does not know instead of guessing.
 *
 * The file extension is the reliable signal and it is asked first.  Where
 * there is none -- a stripped build has offsets and no paths -- the shape of
 * the name answers: a C identifier cannot contain `::`, `<` or a parameter
 * list, because C does not mangle and has no namespaces, templates or
 * overloads.  A bare `main` is genuinely ambiguous and comes back as unknown,
 * which is the truth and paints neutral.
 */
function langOf(file, fn) {
  var dot = file ? file.lastIndexOf('.') : -1;
  if (dot > 0) {
    var ext = file.slice(dot + 1).toLowerCase();
    if (C_EXT[ext]) return 'c';
    if (CPP_EXT[ext]) return 'cpp';
    // `.h` is shared by both languages, so it decides nothing on its own.
  }
  if (!fn) return '';
  if (fn.indexOf('::') >= 0 || fn.indexOf('<') >= 0 || fn.indexOf('(') >= 0)
    return 'cpp';
  return '';
}

var TOKEN = /([A-Za-z_~][A-Za-z0-9_]*)|(\s+)|(::|[<>(),*&\[\]{}]|.)/g;

/// Where a long name may be split across lines: at its seams.
var BREAK_AFTER = { ',': 1, '::': 1, '<': 1, '(': 1, '>': 1, ')': 1 };

function esc(s) {
  return String(s).replace(/[&<>"]/g, function (c) {
    return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c];
  });
}

/// A count with thousands separators.  Always the same locale, so two runs of
/// the tool can be compared without the numbers changing shape.
function num(n) { return n.toLocaleString('en-US'); }

/// Bytes as a person reads them.  One decimal from KiB up, none for plain
/// bytes: `1536.0 B` is not more precise than `1536 B`, it is just longer.
function human(n) {
  var u = ['B', 'KiB', 'MiB', 'GiB', 'TiB'], i = 0;
  while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
  return (i ? n.toFixed(1) : n) + ' ' + u[i];
}

/// The token text, with the search needle marked inside it if it is there.
function paint(text, cls, needle) {
  var body;
  var at = needle ? text.toLowerCase().indexOf(needle) : -1;
  if (at < 0) {
    body = esc(text);
  } else {
    body = esc(text.slice(0, at)) + '<span class="hit">' +
           esc(text.slice(at, at + needle.length)) + '</span>' +
           esc(text.slice(at + needle.length));
  }
  return cls ? '<span class="' + cls + '">' + body + '</span>' : body;
}

/**
 * A symbol as marked-up HTML.
 *
 * Two passes and not one: which identifier is the FUNCTION's own name can only
 * be known once the whole name has been read -- it is the last one at depth
 * zero before the parameter list opens -- and that is exactly the part a
 * reader is looking for.
 */
function codeHtml(name, needle, lang) {
  if (!name) return '';
  var toks = [], m, depth = 0, primary = -1, sawParen = false;
  TOKEN.lastIndex = 0;
  while ((m = TOKEN.exec(name)) !== null) {
    var text = m[0];
    var kind = m[1] ? 'word' : (m[2] ? 'space' : 'punct');
    if (kind === 'punct') {
      if (text === '<' || text === '(' || text === '[') {
        if (text === '(' && depth === 0) sawParen = true;
        depth++;
      } else if (text === '>' || text === ')' || text === ']') {
        depth--;
        if (depth < 0) depth = 0;   // an unbalanced name is still a name
      }
    }
    if (kind === 'word' && depth === 0 && !sawParen && !KEYWORDS[text])
      primary = toks.length;
    toks.push({ t: text, k: kind, d: depth });
  }

  var out = '';
  for (var i = 0; i < toks.length; i++) {
    var tk = toks[i];
    if (tk.k === 'space') { out += ' '; continue; }
    var cls = '';
    if (i === primary) {
      // The name it is really called, in the colour of its language.
      cls = 'id' + (lang ? ' lang-' + lang : '');
    } else if (tk.d > 0) {
      cls = 'arg';        // template arguments and parameters: present, quiet
    } else if (tk.k === 'word') {
      cls = KEYWORDS[tk.t] ? 'kw' : 'ns';
    } else {
      cls = 'pn';
    }
    out += paint(tk.t, cls, needle);
    /* A BREAK OPPORTUNITY, not a break.  A symbol four hundred characters
     * long makes its column four hundred characters wide, and then the whole
     * table resizes around one row -- so the cell is capped and the text
     * wraps.  Where it wraps is chosen here rather than left to the browser:
     * after a comma, a `::`, an opening angle or an opening paren the line
     * breaks where the name has a seam, instead of through the middle of an
     * identifier.  `<wbr>` adds nothing to the text; copying the name still
     * gives back exactly what the symbol table said. */
    if (BREAK_AFTER[tk.t]) out += '<wbr>';
  }
  return out;
}
