# Interface language and translations

rummage speaks the language of your session when it has a translation for it and
English otherwise. This page explains how the language is chosen and how to add or
complete a translation.

## Choosing the language

The language comes from the environment once, at start, the way other programs
choose their message language: `LANGUAGE` (a list such as `de:en`), then `LC_ALL`,
`LC_MESSAGES` and `LANG`. A locale with a region uses that region's translation when
there is one (`pt_BR`) and otherwise the language's (`pt`). The encoding and modifier
(`.UTF-8`, `@euro`) do not matter. `C`, `POSIX` and any English locale give English.

To try another language for one run:

```sh
LANGUAGE=de rummage
```

Texts a translation does not have yet stay English. Command-line output, log messages
and problems reported about `config.toml` stay English, because they name the
configuration keys and options literally.

## Available translations

| Code | Language | File |
| --- | --- | --- |
| `de` | Deutsch | [`data/i18n/de.toml`](../data/i18n/de.toml) |

## Adding a language

Each language is one file in `data/i18n`, named after its code: `fr.toml`, `pt_BR.toml`.
The build compiles every file found there into rummage, so a new language needs no
code change. Re-run CMake (or build again) after adding a file.

Start from a copy of an existing file and keep its three tables:

```toml
[language]
name = "Français"          # the language's own name
decimal_separator = ","     # how numbers like 1.5 MB are written; "." when omitted

# Key names in shortcuts, as keyboards in the language label them. Leave out keys
# that keep their English name.
[keys]
Ctrl = "Ctrl"
Space = "Espace"

# The English text, exactly as the code writes it, and its translation.
[texts]
"Empty folder" = "Dossier vide"
"{} of {} selected" = "{} sur {} sélectionnés"
"Delete “{}” permanently?" = "Supprimer « {} » définitivement ?"
```

- **Keys are the English texts.** Copy them exactly, including punctuation, spaces at
  either end, `…` and quotation marks. A key that does not match stays English.
- **Fields.** `{}` marks where a value goes, in the English order. To reorder, number
  them: `"{} of {} selected" = "{1} éléments, {0} sélectionnés"`. Format details stay inside the
  braces (`{:.1f}`). A translation with other fields than its English text is not used:
  that message then shows in English.
- **Composed messages.** Some texts are parts of a longer message: an operation name
  that fills a sentence (`"Could not {} “{}”."` with `"extract"`), or a label shown
  before a value (`"Archive: {}"`). Translate each part so the sentence still reads
  well, numbering fields where the order differs.
- **Shortcut keys.** Shortcut texts are written with English key names (`Ctrl+Shift+Z`).
  The `[keys]` table renames each word between `+`, spaces and commas.

## Checking a translation

`scripts/i18n-check.py` (Python 3.11 or later) compares each file with the texts the
code translates:

```sh
scripts/i18n-check.py            # every language: missing and unused texts
scripts/i18n-check.py fr         # one language
scripts/i18n-check.py --strict   # exit status 1 when something is missing or unused
```

`missing` texts are shown in English until they are added; `unused` entries belong to
texts the code no longer has and can be removed. The unit tests check every file in
`data/i18n`: it must parse, have a `name`, and keep the fields of each English text.

## Making new texts translatable

Code keeps its English text and passes it through `i18n::tr()`, or `i18n::trf()` when
it has fields (`src/core/include/rummage/core/i18n.hpp`). Menus, the settings dialog
and the shortcut overview translate their labels when they draw them, so a label there
needs only a catalog entry. After adding interface text, run
`scripts/i18n-check.py --unwrapped`: it also lists string literals in interface code
that look visible but are not translated. It is a heuristic and lists identifiers,
D-Bus names and log text too, so read the list before acting on it.
