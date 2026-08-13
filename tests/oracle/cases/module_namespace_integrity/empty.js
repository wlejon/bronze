// A module that exports nothing. Its namespace is the one namespace that can
// be FROZEN: 7.3.15's extra requirement is about every own data property, and
// this object has none for `writable: true` to be true of.
export {};
