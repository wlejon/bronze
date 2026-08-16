// The imported half. Its `import.meta` is ITS OWN — a different object naming a
// different file — which is 16.2.1.10 caching the record on the Module Record
// rather than on the program.
export const depUrl = import.meta.url;
export const depSameTwice = import.meta === import.meta;

export function depMetaTag() {
  return import.meta.tag;
}

import.meta.tag = 'dep';
