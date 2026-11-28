# rummage website

A custom Hugo site for `https://rummage-fm.com`, hosted with GitHub Pages.
No npm, external theme, web fonts, analytics, or application server is required.
Use **Hugo 0.170.1** (the same version and checksum pinned in the workflow).
Python 3.11+ is needed only for the generated-site checks.

## Preview and build

From the repository root:

```sh
hugo server --source website --bind 127.0.0.1
```

Open the local address printed by Hugo, normally `http://localhost:1313/`.
Changes to the mounted `docs/` directory are watched too.

```sh
hugo --source website --minify --panicOnWarning --printPathWarnings
python3 website/check.py website/public
```

Generated output goes into the ignored `website/public/` directory. Do not commit it.
The check verifies internal links, fragments, images, script/style assets, all
search entries, distribution downloads, and the domain configuration. It does
not check remote release availability or replace desktop/mobile browser review.

## Content and design

- `hugo.toml`: domain, metadata, file mounts, and optional video.
- `layouts/` and `assets/`: the custom templates, CSS, and small progressive JS.
- `data/navigation.toml`: the published guides, their groupings, and descriptions.
- `content/docs/_content.gotmpl`: reads the original Markdown from `../docs/`,
  derives titles, and removes the GitHub-only navigation row during rendering.
- `layouts/_markup/`: maps repository-relative links to website pages, keeps
  source/contributor references on GitHub, and makes wide tables scrollable.
- `layouts/downloads.html`: derives all five package links and installation
  commands from the development download table in `docs/install.md`.

Edit existing guides in `docs/`, not in `website/`. To publish a new guide, add
its slug and description to `data/navigation.toml`. The build creates its page
and adds it to browser-side search. The docs follow current development sources;
they are not a versioned copy of the last successful binary release.

Screenshots, the logo, and the example configuration are mounted directly from
their canonical locations; no second asset copy is required. Search downloads
its index only when used and sends no queries to a service. Reading, navigation,
downloads, and native video playback work without JavaScript; all screenshot
views and the complete docs navigation remain accessible in that case.

The screenshot collection was refreshed from the supplied 24 November 2026
captures. `data/screenshots.json` records each source PNG filename and SHA-256,
the selected light/dark/custom appearance, dimensions, and descriptive alt text.
The published WebP files are lossless, at their original dimensions; decoded
pixels were compared with the source PNGs. The original PNGs stay in the supplied
Screenshots directory. The four initial custom-palette takes were superseded by
the later neutral takes. The homepage uses the neutral dark artwork grid as its hero
until a current dual-pane capture is available, and its view gallery compares
both light and dark appearances. Images can be opened at full resolution.

Standalone screenshots use dark appearances throughout the homepage, README,
and guides. Light images are reserved for explicit appearance comparisons, with
dark first and the homepage switcher defaulting to dark.
Folder Insights has a homepage showcase and its own guide at `/docs/insights/`,
listed in the docs navigation and search.

A second homepage gallery shows niri, Hyprland, Umbriel, GNOME, and KDE Plasma.
These five additional captures retain their original dimensions and demonstrate
customised desktop setups, not default themes. They also appear in the Appearance
guide. Their manifest entries include a `desktop` label; the gallery uses separate
controls from the light/dark view comparison and shows all five without JavaScript.

Update the manifest when replacing an image. The website and guides share these
assets; the original `guide-*`, `shot-*`, and `readme-*` screenshots have been
retired rather than retained in the deployed image directory.

The Photo Library screenshot collection contains six supplied 27 November 2026 captures:
the library grid, Quick Look, the album cover grid, photo details, the source folder picker,
and the compact Add to album context menu. Five captures were refreshed from the 22:51–22:52
session; the source folder picker retains its earlier capture. Their `photo-*` manifest entries
record the capture date and custom dark appearance. They use the same lossless, original-size
WebP conversion and decoded-pixel verification as the earlier collection. Superseded source
filenames are listed as unused. All six captures appear in the guide; the homepage uses the
updated library grid and links to the guide and optional add-on installation steps.

The `/pricing/` page presents three humorous free tiers, all linking to the same
downloads. Keep their feature lists identical and the MIT licence and development
build context accurate. The page credits Hesiod's pricing page as its inspiration.

## Support links

`/support/` links to troubleshooting and GitHub Issues, using `params.repository`
from `hugo.toml`. It appears in the main navigation, footer, and pricing FAQ.
To add Discord, set the existing `params.discordInvite` to your public invite URL
in `hugo.toml`. The Discord card and footer link appear automatically; leaving
the value empty hides both. Use a permanent invite so published links stay valid.

`params.koFi` points to the donation page linked from the footer, support page,
and pricing FAQ. Leave it empty to hide those donation links. Donations are
optional and do not unlock paid features.

## Add a screen recording

Put a short, compressed H.264 MP4 in `website/static/media/`, then update:

```toml
[params]
demoVideo = '/media/rummage-demo.mp4'
demoPoster = '/images/rummage-dark-grid.webp'
demoCaption = 'A short tour of rummage.'
```

These keys already exist in `hugo.toml`; edit them in place. The homepage switches
from its screenshot to an inline player with native controls, a poster, and no
autoplay. Keep `demoVideo` empty until the recording exists. The site check fails
if a configured video or poster is missing. For narrated footage, add a caption
track to the video template. Keep clips small; longer videos can be hosted
elsewhere and referenced with an HTTPS URL.

## Publish to GitHub Pages

The existing **Build, test and package** workflow (`.github/workflows/ci.yml`)
now builds and checks the website before application tests and packaging. It runs
automatically on `main` pushes and can be dispatched manually on a branch. A
feature-branch push does not start a second build. There is no separate Website
workflow or generated `gh-pages` branch.

Pages deployment runs only on `main` in `barrulus/rummage`, after the website
checks succeed. It is independent of application tests and packaging within the
same workflow, so an application failure cannot block a valid website update.
Application releases still require their full test and packaging gates.
Branch validation builds the site without publishing it. The Tumbleweed-only
manual mode does not publish downloads or deploy Pages.

One-time repository and domain setup:

1. In `barrulus/rummage` → **Settings → Pages**, choose **GitHub Actions** as the
   build source.
2. Configure `rummage-fm.com` as the custom domain in Pages settings. The tracked
   `static/CNAME` documents the same domain; with Actions publishing, set the
   domain in repository settings as well.
3. At the domain's DNS provider, follow GitHub's current
   [apex-domain instructions](https://docs.github.com/en/pages/configuring-a-custom-domain-for-your-github-pages-site/managing-a-custom-domain-for-your-github-pages-site).
   If using `www`, point its CNAME to `barrulus.github.io`.
   Verify ownership using GitHub's
   [domain verification instructions](https://docs.github.com/en/pages/configuring-a-custom-domain-for-your-github-pages-site/verifying-your-custom-domain-for-github-pages).
4. Merge the site changes to `main`, or run **Build, test and package** on `main`.
5. Once DNS and the certificate are ready, enable **Enforce HTTPS** in Pages.

The website links straight to the existing `development` GitHub Release assets.
Binaries remain there, not in the repository or the Pages artifact.

When upgrading Hugo, update the workflow's version and SHA-256 together, verify
the digest against the official release, and run the build/check commands above.
