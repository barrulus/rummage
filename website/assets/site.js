// Progressive enhancement only: downloads, docs, and video work without JS.
const viewButtons = [...document.querySelectorAll('[data-view]')];
const appearanceButtons = [...document.querySelectorAll('[data-appearance]')];
const panels = [...document.querySelectorAll('[data-panel]')];
if (viewButtons.length) {
  document.querySelector('.gallery-controls').hidden = false;
  let view = 'columns';
  let appearance = 'dark';
  function updateGallery() {
    for (const panel of panels) panel.hidden = panel.dataset.panel !== view || panel.dataset.theme !== appearance;
    for (const button of viewButtons) button.setAttribute('aria-pressed', String(button.dataset.view === view));
    for (const button of appearanceButtons) button.setAttribute('aria-pressed', String(button.dataset.appearance === appearance));
  }
  for (const button of viewButtons) button.addEventListener('click', () => { view = button.dataset.view; updateGallery(); });
  for (const button of appearanceButtons) button.addEventListener('click', () => { appearance = button.dataset.appearance; updateGallery(); });
  updateGallery();
}

const desktopButtons = [...document.querySelectorAll('[data-desktop]')];
const desktopPanels = [...document.querySelectorAll('[data-desktop-panel]')];
if (desktopButtons.length) {
  const selectDesktop = (desktop) => {
    for (const panel of desktopPanels) panel.hidden = panel.dataset.desktopPanel !== desktop;
    for (const button of desktopButtons) button.setAttribute('aria-pressed', String(button.dataset.desktop === desktop));
  };
  for (const button of desktopButtons) button.addEventListener('click', () => selectDesktop(button.dataset.desktop));
  selectDesktop(desktopButtons[0].dataset.desktop);
  document.querySelector('.desktop-switcher').hidden = false;
}

const docsMenu = document.querySelector('.docs-menu');
if (docsMenu) {
  const mobile = matchMedia('(max-width: 760px)');
  const updateMenu = () => { docsMenu.open = !mobile.matches; };
  updateMenu();
  mobile.addEventListener('change', updateMenu);
}

const search = document.querySelector('[data-search-index]');
if (search) {
  search.hidden = false;
  const input = search.querySelector('input');
  const status = search.querySelector('[role="status"]');
  const results = search.querySelector('ul');
  let indexPromise;
  let request = 0;
  async function loadIndex() {
    if (!indexPromise) {
      indexPromise = fetch(search.dataset.searchIndex).then(response => {
        if (!response.ok) throw new Error('Search index unavailable');
        return response.json();
      }).catch(error => { indexPromise = undefined; throw error; });
    }
    return indexPromise;
  }
  input.addEventListener('input', async () => {
    const current = ++request;
    const query = input.value.trim().toLowerCase();
    results.replaceChildren();
    results.hidden = true;
    if (query.length < 2) {
      status.textContent = query ? 'Type at least two characters.' : '';
      return;
    }
    status.textContent = 'Searching…';
    try {
      const pages = await loadIndex();
      if (current !== request) return;
      const words = query.split(/\s+/);
      const matches = pages.map(page => {
        const title = page.title.toLowerCase();
        const description = page.description.toLowerCase();
        const text = page.text.toLowerCase();
        const score = words.every(word => `${title} ${description} ${text}`.includes(word))
          ? words.reduce((sum, word) => sum + (title.includes(word) ? 10 : 0) + (description.includes(word) ? 5 : 0) + 1, 0) : 0;
        return { page, score };
      }).filter(result => result.score).sort((a, b) => b.score - a.score);
      status.textContent = matches.length ? `${matches.length} matching ${matches.length === 1 ? 'guide' : 'guides'}.` : 'No matching guides. Try a broader term, or browse the topics below.';
      for (const { page } of matches) {
        const item = document.createElement('li');
        const link = document.createElement('a');
        link.href = page.url;
        const title = document.createElement('strong');
        title.textContent = page.title;
        const excerpt = document.createElement('p');
        const position = page.text.toLowerCase().indexOf(words[0]);
        const start = Math.max(0, position - 65);
        excerpt.textContent = position < 0 ? page.description : `${start ? '…' : ''}${page.text.slice(start, start + 190).replace(/\s+/g, ' ')}…`;
        link.append(title, excerpt);
        item.append(link);
        results.append(item);
      }
      results.hidden = !matches.length;
    } catch {
      if (current === request) status.textContent = 'Search could not load. Browse the topics below, or type again to retry.';
    }
  });
}
