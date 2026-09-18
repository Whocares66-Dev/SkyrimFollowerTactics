(() => {
  const root = document.documentElement;
  const toggle = document.getElementById('guide-toggle');
  const navigation = document.getElementById('guide-navigation');
  if (!toggle || !navigation) return;
  function update() {
    const collapsed = root.classList.contains('toc-collapsed');
    toggle.setAttribute('aria-expanded', String(!collapsed));
    toggle.setAttribute('aria-label', `${collapsed ? 'Expand' : 'Collapse'} table of contents`);
    toggle.title = toggle.getAttribute('aria-label');
    navigation.hidden = collapsed;
  }
  toggle.addEventListener('click', () => {
    root.classList.toggle('toc-collapsed');
    update();
    try { localStorage.setItem('ft-toc-collapsed', String(root.classList.contains('toc-collapsed'))); } catch (_) {}
  });
  update();
  const sections = Array.from(navigation.querySelectorAll('details'));
  try {
    const closed = JSON.parse(sessionStorage.getItem('ft-toc-closed') || '[]');
    sections.forEach((section, index) => { section.open = !closed.includes(index); });
    navigation.scrollTop = Number(sessionStorage.getItem('ft-toc-scroll') || 0);
  } catch (_) {}
  window.addEventListener('pagehide', () => {
    try {
      sessionStorage.setItem('ft-toc-scroll', navigation.scrollTop);
      sessionStorage.setItem('ft-toc-closed', JSON.stringify(sections.flatMap((section, index) => section.open ? [] : [index])));
    } catch (_) {}
  });
})();

(() => {
  const screenshots = document.querySelectorAll('.main-content img.screenshot');
  if (!screenshots.length) return;

  const dialog = document.createElement('dialog');
  dialog.className = 'image-lightbox';
  dialog.setAttribute('aria-label', 'Screenshot at original size');
  const close = document.createElement('button');
  close.type = 'button';
  close.className = 'lightbox-close';
  close.setAttribute('aria-label', 'Close screenshot');
  close.title = 'Close screenshot';
  const viewport = document.createElement('div');
  viewport.className = 'lightbox-viewport';
  viewport.tabIndex = 0;
  viewport.setAttribute('role', 'region');
  viewport.setAttribute('aria-label', 'Original-size screenshot; scroll to view larger images');
  const image = document.createElement('img');
  image.className = 'lightbox-image';
  viewport.append(image);
  dialog.append(close, viewport);
  document.body.append(dialog);
  let opener;

  close.addEventListener('click', () => dialog.close());
  viewport.addEventListener('click', event => {
    if (event.target === viewport) dialog.close();
  });
  dialog.addEventListener('close', () => {
    document.documentElement.classList.remove('lightbox-open');
    image.removeAttribute('src');
    opener?.focus({ preventScroll: true });
  });

  screenshots.forEach(screenshot => {
    const link = document.createElement('a');
    link.href = screenshot.currentSrc || screenshot.src;
    link.className = 'screenshot-link';
    link.setAttribute('aria-label', `Open ${screenshot.alt || 'screenshot'} at original size`);
    link.setAttribute('aria-haspopup', 'dialog');
    screenshot.replaceWith(link);
    link.append(screenshot);
    link.addEventListener('click', event => {
      if (event.ctrlKey || event.metaKey || event.shiftKey || event.altKey) return;
      if (typeof dialog.showModal !== 'function') return;
      event.preventDefault();
      opener = link;
      image.src = link.href;
      image.alt = screenshot.alt;
      dialog.showModal();
      document.documentElement.classList.add('lightbox-open');
      viewport.scrollTo(0, 0);
      close.focus({ preventScroll: true });
    });
  });
})();
