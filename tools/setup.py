# -*- coding: utf-8 -*-
#
# VestaVM -- Distributed Virtual Machine
#
# Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
# License: MIT (see LICENSE).  Part of the VestaVM family.
"""Install `alloc_tree`, so it runs where the problem is.

\\~english
WHY THIS EXISTS.  The package already had `__init__.py` and `__main__.py`, so
`python -m alloc_tree` worked -- from inside this directory, and nowhere else.
That is the wrong place to be standing: an allocation report is read where the
PROBLEM happened, which is somebody else's machine, somebody else's checkout, or
a build server with no source tree at all.

    python setup.py install          # and then, from anywhere:
    python -m alloc_tree <dir>
    alloc-tree <dir>

\\~spanish
POR QUE EXISTE.  El paquete ya tenia `__init__.py` y `__main__.py`, asi que
`python -m alloc_tree` funcionaba -- desde dentro de este directorio, y en
ningun otro sitio.  Y ese es el sitio equivocado para estar: un informe de
reservas se lee donde ocurrio el PROBLEMA, que es la maquina de otro, la copia
de otro, o un servidor de construccion sin arbol de fuentes.
\\~
"""

from setuptools import setup

setup(
    name="vesta-alloc-tree",
    version="1.0.0",
    description="Where a program's allocations are born, as a tree you can walk",
    author="David Lopez.T (DesmonHak)",
    license="MIT",
    python_requires=">=3.8",
    packages=["alloc_tree"],
    # \~english THE TEMPLATES TRAVEL WITH THE MODULE, and they have to: `page.py`
    #          finds them with `os.path.dirname(__file__)/templates`, so an
    #          install that left them behind would not fail at install time --
    #          it would fail the first time somebody asked for a page, on the
    #          machine where the problem is.
    # \~spanish LAS PLANTILLAS VIAJAN CON EL MODULO, y tienen que hacerlo:
    #          `page.py` las busca en `os.path.dirname(__file__)/templates`, asi
    #          que una instalacion que las dejara atras no fallaria al instalar
    #          -- fallaria la primera vez que alguien pidiera una pagina, en la
    #          maquina donde esta el problema.
    # \~
    package_data={"alloc_tree": ["templates/*"]},
    include_package_data=True,
    # \~english NOTHING REQUIRED, and this is the one decision here worth
    #          arguing.  Folding the tree and printing it -- the whole `--text`
    #          path, tree and five tables -- uses the standard library alone, so
    #          the tool runs where NOTHING can be installed.  That is not a
    #          nicety: it is the usual situation on the machine a memory problem
    #          actually shows up on.  Making Jinja2 required would trade that
    #          away for a rendering step most readings never reach.
    #
    # \~spanish NADA OBLIGATORIO, y es la unica decision de aqui que merece
    #          discutirse.  Plegar el arbol e imprimirlo -- todo el camino
    #          `--text`, el arbol y las cinco tablas -- usa la biblioteca
    #          estandar y nada mas, asi que la herramienta corre donde NO se
    #          puede instalar nada.  Y eso no es un detalle: es la situacion
    #          habitual en la maquina donde un problema de memoria aparece de
    #          verdad.  Hacer Jinja2 obligatorio cambiaria eso por un paso de
    #          dibujado al que la mayoria de las lecturas no llegan.
    # \~
    install_requires=[],
    # \~english The page, and only the page.  See `requirements.txt` for why it
    #          is Jinja2 and not concatenation: it escapes by default, and this
    #          page is fed function names straight out of a symbol table -- one
    #          carrying a `<` would swallow the rest of the row, which does not
    #          look like an error, it looks like a shorter report.
    # \~spanish La pagina, y solo la pagina.  Ver `requirements.txt` para por
    #          que es Jinja2 y no concatenar: escapa por defecto, y a esta
    #          pagina le llegan nombres de funcion sacados de una tabla de
    #          simbolos -- uno con un `<` se comeria el resto de la fila, que no
    #          parece un error: parece un informe mas corto.
    # \~
    extras_require={"page": ["jinja2>=3.0"]},
    # \~english The same entry point as `python -m alloc_tree`, under a name
    #          that does not need the module path.  Both stay: one for a machine
    #          where this is installed, the other for a checkout where it is not.
    # \~spanish El mismo punto de entrada que `python -m alloc_tree`, con un
    #          nombre que no obliga a saber la ruta del modulo.  Se quedan los
    #          dos: uno para una maquina donde esto esta instalado, el otro para
    #          una copia donde no.
    # \~
    entry_points={"console_scripts": ["alloc-tree = alloc_tree.__main__:main"]},
)
