// SPDX-License-Identifier: LGPL-3.0-only
//
// Copyright (C) 2026 Corey Pennycuff
//
// This file is part of Ghoti.io Vector.
//
// Ghoti.io Vector is free software: you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License version 3 as
// published by the Free Software Foundation.
//
// Ghoti.io Vector is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
// for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// The browser driver of the animation oracle (tools/doc-oracle/anim_oracle.py): the style oracle's driver (style_driver.mjs, beside the
// browser image's Containerfile), with one change. A document carries a `time` in milliseconds, and before the computed styles are read
// the document is paused there: `pauseAnimations()` and `setCurrentTime()` of the root for SMIL, and every animation `getAnimations()`
// lists that is playing paused at that current time for CSS (one `animation-play-state` paused from the start keeps the time it holds,
// as it would on a running timeline). That is a seek, so no event fires (design.md A2).
//
// The driver is not copied: this file reads style_driver.mjs as it is in this repository, replaces its reading function and the call that
// hands it the queries, and runs the result. Each replacement must find its text exactly once, or the driver stops (exit 2), so a change of
// the style driver that this one does not follow is an error and never a silent difference. Run it inside the pinned browser image
// (tools/oracle/oracle-exec, with this directory and the style driver's mounted read-only), from /opt/browser.

import { readFileSync } from 'node:fs';

const here = new URL('.', import.meta.url);
const source = readFileSync(new URL('../oracle/containers/browser/style_driver.mjs', here), 'utf8');

function replaceOnce(text, from, to, what) {
  const at = text.indexOf(from);
  if (at < 0 || text.indexOf(from, at + 1) >= 0) {
    process.stderr.write(`anim-driver: the style driver's ${what} is not where this driver expects it\n`);
    process.exit(2);
  }
  return text.slice(0, at) + to + text.slice(at + from.length);
}

let patched = replaceOnce(source,
  "const READ = `(json) => {\n  const queries = JSON.parse(json);\n",
  "const READ = `(json) => {\n  const asked = JSON.parse(json);\n  const queries = asked.queries;\n" +
  "  const root = document.documentElement;\n" +
  "  if (root.pauseAnimations) { root.pauseAnimations(); root.setCurrentTime(asked.time / 1000); }\n" +
  "  for (const a of document.getAnimations()) { if (a.playState !== 'paused') { a.pause(); a.currentTime = asked.time; } }\n",
  'reading function');
patched = replaceOnce(patched,
  "const text = await this.call(READ, [{ type: 'string', value: JSON.stringify(doc.queries) }]);",
  "const text = await this.call(READ, [{ type: 'string', value: JSON.stringify({ time: doc.time, queries: doc.queries }) }]);",
  'call of the reading function');

await import('data:text/javascript;base64,' + Buffer.from(patched, 'utf8').toString('base64'));
