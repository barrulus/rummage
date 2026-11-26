# Interface language and translations

rummage speaks the language of your session when it has a translation for it and
English otherwise. This page explains how the language is chosen and how to add or
complete a translation.

## Choosing the language

Choose **Settings → General → Interface language** to override the session language
for Rummage. Choices use each language's own name. The equivalent configuration is
`[interface] language = "de"` (on separate lines in TOML); `"system"` is the default
and `"en"` explicitly selects English. Restart all Rummage browser windows after a
change: opening another window in the existing process retains that process's language.
After saving a different language, the Settings status prominently reminds you to restart.
New file-picker processes read the setting independently, using their selected config.
This setting takes precedence over all message-language environment variables and
does not change the system locale, environment or external applications. Rummage's
translated shortcut labels and decimal formatting follow the chosen catalog; shortcut
bindings remain unchanged. An unavailable catalog is reported as a configuration
problem and falls back to `system`.

With `system`, the language comes from the environment once, at start, the way other programs
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
| `es` | Español | [`data/i18n/es.toml`](../data/i18n/es.toml) |
| `fr` | Français | [`data/i18n/fr.toml`](../data/i18n/fr.toml) |
| `ja` | 日本語 | [`data/i18n/ja.toml`](../data/i18n/ja.toml) |
| `pt_BR` | Português (Brasil) | [`data/i18n/pt_BR.toml`](../data/i18n/pt_BR.toml) |
| `ru` | Русский | [`data/i18n/ru.toml`](../data/i18n/ru.toml) |
| `zh_CN` | 简体中文 | [`data/i18n/zh_CN.toml`](../data/i18n/zh_CN.toml) |

Use `LANGUAGE=pt_BR` for Brazilian Portuguese or `LANGUAGE=zh_CN` for Simplified
Chinese. These are region-specific catalogs: `pt` and `zh` alone do not select
them, nor do other regions such as `pt_PT` or `zh_TW`. Spanish, French, Russian
and Japanese also accept regional locales such as `es_MX.UTF-8` and `fr_CA.UTF-8`.
Chinese and Japanese need fonts covering their characters; rummage uses the
system's font fallback to find them.

## Adding a language

Each language is one file in `data/i18n`, named after its code: `fr.toml`, `pt_BR.toml`.
The build compiles every file found there into rummage, so a new language needs no
code change. Re-run CMake (or build again) after adding a file.

Start from a copy of an existing file. Plain messages and key names remain simple
strings; plural messages and contexts use the tables described below:

```toml
[language]
name = "Français"          # the language's own name
decimal_separator = ","     # how numbers like 1.5 MB are written; "." when omitted

[language.plural_rules]
one = "n = 0..1"
many = "n != 0 and n % 1000000 = 0"

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
"{} files" = { one = "{} fichier", many = "{} fichiers", other = "{} fichiers" }
```

- **Keys are the English texts.** Copy them exactly, including punctuation, spaces at
  either end, `…` and quotation marks. A key that does not match stays English.
- **Fields.** `{}` marks where a value goes, in the English order. To reorder, number
  them: `"{} of {} selected" = "{1} éléments, {0} sélectionnés"`. Format details stay inside the
  braces (`{:.1f}`). The catalog checker rejects changed or missing fields. Invalid
  formatting at runtime falls back to the English message.
- **Composed messages.** Some texts are parts of a longer message: an operation name
  that fills a sentence (`"Could not {} “{}”."` with `"extract"`), or a label shown
  before a value (`"Archive: {}"`). Translate each part so the sentence still reads
  well, numbering fields where the order differs.
- **Shortcut keys.** Shortcut texts are written with English key names (`Ctrl+Shift+Z`).
  The `[keys]` table renames each word between `+`, spaces and commas.

## Plural messages

Use `trn("{} file", "{} files", count)` for a count, rather than choosing singular
and plural in C++. The catalog key is the **English plural**. Its value is a table
of category names and translated messages. This also applies to complete sentences,
such as `"Getting ready to copy {} items…"`.

The categories are `zero`, `one`, `two`, `few`, `many` and `other`. Rules are stored
in `[language.plural_rules]`; no C++ change is needed for another language's rules.
For example, integer counts in Polish use:

```toml
[language.plural_rules]
one = "n = 1"
few = "n % 10 = 2..4 and n % 100 != 12..14"
many = "n != 1 and n % 10 = 0..1 or n % 10 = 5..9 or n % 100 = 12..14"

[texts]
"{} files" = { one = "{} plik", few = "{} pliki", many = "{} plików", other = "{} pliku" }
```

Rules accept `n`, optional modulo `%`, `=` or `!=`, integer values or inclusive
ranges (`2..4`), comma-separated alternatives, and `and` / `or`. `and` binds more
tightly than `or`. Rules are checked in category order above; `other` is the
unconditional fallback and has no rule. Zero divisors, overflow, reversed ranges
and unrecognized syntax are rejected. Each rule is limited to 1,024 bytes,
32 conditions and 32 ranges per condition.

Every plural message needs `other` and a translation for each category declared
by the language. A missing category at runtime uses that message's `other` form;
the strict checker catches the omission before release. A missing message uses
the English singular for 1 and the English plural otherwise. A catalog with invalid
rules is ignored. Omitting the rules table keeps the original `n = 1` / `other`
behavior; an explicitly empty table selects `other` for every count.

Count is format argument `{0}`; extra arguments start at `{1}`. For example:
`trn("{} file in {}", "{} files in {}", count, folder)`. Number fields when a
translation changes their order. If a count needs custom display formatting, use
`trn("{1} item", "{1} items", count, formatted_count)` so category selection still
uses the numeric count.

This API handles whole, nonnegative 64-bit cardinal counts. It does not handle
fractional counts, ordinal numbers or plural ranges. Consult the
[Unicode CLDR language rules](https://www.unicode.org/cldr/charts/latest/supplemental/language_plural_rules.html)
and adapt their integer cases to this grammar. A translation catalog alone also
does not establish right-to-left layout support; inspect the interface in each
new language.

## Context keys

Use a stable context when the same English text has different meanings:

```toml
[contexts.file-action]
"Open" = "Öffnen"

[contexts.connection-state]
"Open" = "Offen"
```

Code calls `trc("file-action", "Open")`, or `trcf(context, english, args...)` for a
formatted message. `trnc(context, singular, plural, count, args...)` supports plural
tables inside a context. Contexts are independent: a missing contextual entry
falls back to English, never to an unrelated context-free translation. Existing
`[texts]` entries and `tr()` / `trf()` calls continue to work.

Translate application-owned labels where they are constructed. Menu renderers
display final labels literally, preserving filenames, template names, application
names and user-configured labels even when they match a catalog key.

## Checking a translation

`scripts/i18n-check.py` (Python 3.11 or later) compares each file with the texts the
code translates:

```sh
scripts/i18n-check.py            # every language: missing and unused texts
scripts/i18n-check.py fr         # one language
scripts/i18n-check.py --strict   # exit status 1 when something is missing or unused
```

`missing` texts are shown in English until they are added; `unused` entries belong to
texts the code no longer has and can be removed. The checker also validates contexts,
plural-rule syntax, required forms and placeholders. It runs in the shared lint
check. Runtime tests cover multiple language rules and English fallbacks.

## Making new texts translatable

Code keeps its English text and passes it through `i18n::tr()`, or `i18n::trf()` when
it has fields (`src/core/include/rummage/core/i18n.hpp`); use `trn()` for counts and
the context variants when meanings differ. After adding interface text, run
`scripts/i18n-check.py --unwrapped`: it also lists string literals in interface code
that look visible but are not translated. It is a heuristic and lists identifiers,
D-Bus names and log text too, so read the list before acting on it.

For English labels stored in `constexpr` tables (such as shortcut descriptions and
Details columns), use `i18n::mark("Label")`. It leaves the string unchanged until
`tr()` translates it for display, and makes it a required message in the strict
catalog check. Do not mark filenames, configured paths, custom action names or
keybinding identifiers. The `--unwrapped` audit scans interface headers as well as
implementation files.
