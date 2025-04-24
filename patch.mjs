/* patch.mjs – streaming incremental updater (works in Firefox/Edge/Chrome)
   Node 20+ (built-in fetch).  For Node ≤18: npm i node-fetch and uncomment line.  */

   import {
    createWriteStream, openSync, readSync, closeSync, statSync,
  } from 'fs';
  import { appendFile } from 'fs/promises';
  import { pipeline } from 'stream/promises';
  import { TextEncoder } from 'util';
  import path from 'path';
  // import fetch from 'node-fetch';   // ← uncomment if you’re on Node ≤18
  
  /* ───────── Config ───────── */
  const URL      = 'http://host.docker.internal:8000/save-pdf.pdf';
  const NEW_OBJ  = 7126;   // any free object number > current /Size
  const CHUNK    = 65536;  // max bytes kept in RAM
  const OUT      = path.resolve(process.cwd(), 'patched.pdf');
  
  /* ───────── 1. stream original PDF → disk ───────── */
  console.log(`→ streaming ${URL}\n  to ${OUT}`);
  await pipeline((await fetch(URL)).body, createWriteStream(OUT));
  const origSize = statSync(OUT).size;
  console.log(`  done (${origSize} bytes)`);
  
  /* ───────── 2. find startxref of previous revision ───────── */
  const fd   = openSync(OUT, 'r');
  const tail = Buffer.alloc(Math.min(CHUNK, origSize));
  readSync(fd, tail, 0, tail.length, origSize - tail.length);
  
  const sxMatch = tail.toString('latin1').match(/startxref\s+(\d+)\s+%%EOF/);
  if (!sxMatch) throw new Error('startxref not found – PDF may be corrupt');
  const prevXref = Number(sxMatch[1]);
  console.log(`  previous xref at byte ${prevXref}`);
  
  /* ───────── 3. extract entire previous trailer dict ───────── */
  const sniffBuf = Buffer.alloc(32);
  readSync(fd, sniffBuf, 0, sniffBuf.length, prevXref);
  let trailerDict = '';
  
  if (sniffBuf.toString('latin1').startsWith('xref')) {
    /* 3a. previous section is a *text* table */
    const buf = Buffer.alloc(CHUNK);
    let pos = prevXref, depth = 0, found = false;
    while (!found) {
      const n = readSync(fd, buf, 0, CHUNK, pos);
      const slice = buf.toString('latin1', 0, n);
      const t = slice.indexOf('trailer');
      if (t !== -1) {
        let i = slice.indexOf('<<', t);
        for (; i < slice.length; ++i) {
          const ch = slice[i];
          if (ch === '<') depth++;
          if (ch === '>') depth--;
          trailerDict += ch;
          if (depth === 0) { found = true; break; }
        }
      }
      pos += n;
    }
    trailerDict = trailerDict.slice(2, -2);       // drop outer << >>
  } else {
    /* 3b. previous section is an *xref stream* */
    const buf = Buffer.alloc(CHUNK);
    readSync(fd, buf, 0, CHUNK, prevXref);
    const m = buf.toString('latin1').match(/<<[\s\S]+?>>\s*stream/);
    if (!m) throw new Error('xref stream dictionary not found');
    trailerDict = m[0].replace(/^<<|>>\s*stream$/gs, '');
  }
  closeSync(fd);
  
  /* clean up trailer: bump /Size, remove any /Prev */
  trailerDict = trailerDict
    .replace(/\/Size\s+\d+/, `/Size ${NEW_OBJ + 1}`)
    .replace(/\/Prev\s+\d+/, '')
    .trim();
  
  /* ───────── 4. build the incremental patch in memory ───────── */
  const objOffset = origSize;     // first byte of “7126 0 obj”
  let patch = `${NEW_OBJ} 0 obj
  << /Length 34 >>
  stream
  BT
    /F1 12 Tf
    (downloaded by asem) Tj
  ET
  endstream
  endobj
  xref
  0 2
  0000000000 65535 f 
  ${String(objOffset).padStart(10, '0')} 00000 n 
  trailer
  << ${trailerDict} /Prev ${prevXref} >>
  startxref
  ${objOffset}
  %%EOF
  `;
  
  await appendFile(OUT, new TextEncoder().encode(patch));
  console.log(`✓ patch appended.  final size ${statSync(OUT).size} bytes`);
  console.log('✓ run `qpdf --check patched.pdf` – should say “no syntax errors”');
  
