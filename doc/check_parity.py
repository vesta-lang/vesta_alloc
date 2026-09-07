"""Comprueba que la documentacion bilingue del codigo lo es de verdad.

POR QUE ESTO EXISTE.  Doxygen escribe la documentacion de un idioma quedandose
con los trozos marcados para el y descartando el resto, y cuando un trozo no
tiene version en un idioma **no avisa de nada**: simplemente no lo escribe.  El
resultado es una referencia que se publica igual de bien estando coja, y la
unica forma de enterarse seria leer las dos y compararlas a mano.

Eso es lo que hace esto.  Genera las dos, compara sus arboles elemento a
elemento, y falla diciendo QUE se quedo sin traducir.  Un parrafo olvidado pasa
de ser un silencio a ser un rojo.

Y hay una segunda trampa que tambien caza: los marcadores `\\~` mantienen el
idioma activo hasta el siguiente, asi que uno mal cerrado no se lleva solo su
frase -- se lleva todo lo que venga detras, incluidos los `@param` y el
`@return`.  Eso sale aqui como una diferencia de cuenta, que es justo lo que
un ojo humano no ve al revisar un fichero de trescientas lineas.

    python doc/check_parity.py            desde la raiz de la libreria
"""

import os
import subprocess
import sys
import xml.etree.ElementTree as ET

# Los elementos que llevan prosa.  Si uno tiene texto en un idioma y no en el
# otro, alguien se dejo media traduccion.
TEXT_TAGS = ("briefdescription", "detaileddescription", "parameterdescription")


def run_doxygen(config):
    """Genera una de las dos versiones.  Devuelve la carpeta del XML."""
    lang = "en" if config.endswith(".en") else "es"
    # Doxygen crea SU carpeta de salida, pero no la cadena de padres que le
    # falte, y ahi se planta con un error que no dice cual falta.
    out = os.path.join("doc", "out", lang)
    if not os.path.isdir(out):
        os.makedirs(out)
    proc = subprocess.run(["doxygen", config], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE)
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr.decode("utf-8", "replace"))
        raise SystemExit("doxygen fallo con %s" % config)
    return os.path.join(out, "xml")


def text_of(node):
    """Todo el texto de un elemento, sin marcado y sin espacios de sobra."""
    return " ".join("".join(node.itertext()).split())


def collect(xml_dir):
    """Lo documentado, por identificador estable.

    La clave es el `id` que Doxygen le da a cada elemento, no su posicion:
    comparando por posicion, un elemento que solo existe en un idioma
    desplazaria todos los siguientes y el informe acusaria a los inocentes.
    """
    found = {}
    for name in sorted(os.listdir(xml_dir)):
        if not name.endswith(".xml") or name in ("index.xml", "Doxyfile.xml"):
            continue
        root = ET.parse(os.path.join(xml_dir, name)).getroot()
        for parent in root.iter():
            ident = parent.get("id")
            if ident is None:
                continue
            for tag in TEXT_TAGS:
                for i, node in enumerate(parent.findall(tag)):
                    body = text_of(node)
                    if body:
                        found["%s|%s|%d" % (ident, tag, i)] = body
    return found


def main():
    if not os.path.isdir("include"):
        raise SystemExit("ejecutalo desde la raiz de vesta_alloc")

    en = collect(run_doxygen(os.path.join("doc", "Doxyfile.en")))
    es = collect(run_doxygen(os.path.join("doc", "Doxyfile.es")))

    only_en = sorted(set(en) - set(es))
    only_es = sorted(set(es) - set(en))
    # El MISMO texto en los dos idiomas casi siempre significa que no se
    # tradujo: se escribio una vez sin marcadores y Doxygen la puso en ambos.
    # Casi, y no siempre -- un nombre propio o una firma son iguales en los dos
    # --, asi que esto se cuenta y se ensena, pero no falla por si solo.
    same = sorted(k for k in set(en) & set(es) if en[k] == es[k])

    print("elementos documentados: %d en ingles, %d en espanol" % (len(en), len(es)))
    for key in only_en:
        print("  SIN ESPANOL  %s\n      %s" % (key, en[key][:100]))
    for key in only_es:
        print("  SIN INGLES   %s\n      %s" % (key, es[key][:100]))
    if same:
        print("  %d elementos con el MISMO texto en los dos idiomas "
              "(sin marcadores, o iguales a proposito)" % len(same))

    if only_en or only_es:
        print("\n%d elementos sin las dos versiones" % (len(only_en) + len(only_es)))
        return 1
    print("\nTODO OK: cada elemento documentado existe en los dos idiomas")
    return 0


if __name__ == "__main__":
    sys.exit(main())
