import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';

/** Single source of truth for the version: the repository's VERSION file. */
export const version = readFileSync(resolve(process.cwd(), '../VERSION'), 'utf8').trim();

export const REPO = 'https://github.com/razobill/razobill';
export const SITE = 'https://razobill.github.io/razobill/';
export const PYPI = 'https://pypi.org/project/pyrazobill/';
export const PAPER = 'https://doi.org/10.5281/zenodo.23287286';
export const DUCKDB_EXT = 'https://github.com/duckdb/community-extensions/tree/main/extensions/razobill';
export const AUTHOR = { name: 'Moustafa Mahmoud Atta', url: 'https://github.com/4t1a', orcid: 'https://orcid.org/0009-0008-6724-4698' };

/** Prefix an internal path with the site base ("/razobill"). */
export const url = (p = '') => {
  const base = import.meta.env.BASE_URL.replace(/\/$/, '');
  return `${base}/${p.replace(/^\//, '')}`;
};

export const buildDate = new Date();
export const isoDate = buildDate.toISOString().slice(0, 10);
