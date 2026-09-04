#!/usr/bin/env node
import fs from 'node:fs/promises';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

/* Temporary safety launcher for the material-form importer. The source texture
 * can have >100k vertices; spreading that array into Math.max exceeds V8's
 * argument limit. Patch the one diagnostic-only expression before evaluating
 * convert.mjs. The actual form bake is otherwise byte-for-byte the committed
 * importer. This file can disappear once convert.mjs is edited in-place. */
const here=path.dirname(new URL(import.meta.url).pathname);
const srcPath=path.join(here,'convert.mjs');
const tmpPath=path.join(here,'.convert-material-v2-runtime.mjs');
let src=await fs.readFile(srcPath,'utf8');
const needle='meta.rawMax=Math.max(...form);';
const replacement='meta.rawMax=form.reduce((m,v)=>v>m?v:m,0);';
if(!src.includes(needle)) {
  throw new Error('convert.mjs safety patch anchor not found');
}
src=src.replace(needle,replacement);
await fs.writeFile(tmpPath,src,'utf8');
try {
  await import(pathToFileURL(tmpPath).href+'?run='+Date.now());
} finally {
  await fs.rm(tmpPath,{force:true});
}
