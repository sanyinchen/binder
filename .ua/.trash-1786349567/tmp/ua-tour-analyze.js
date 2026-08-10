#!/usr/bin/env node
'use strict';
const fs = require('fs');

function main() {
  const inPath = process.argv[2];
  const outPath = process.argv[3];
  if (!inPath || !outPath) throw new Error('usage: analyze.js <input.json> <output.json>');

  const data = JSON.parse(fs.readFileSync(inPath, 'utf8'));
  const nodes = data.nodes || [];
  const edges = data.edges || [];
  const layers = data.layers || [];

  const byId = new Map(nodes.map(n => [n.id, n]));
  const nameOf = id => (byId.get(id) ? byId.get(id).name : id);

  // ---- A/B. fan-in / fan-out
  const fanIn = new Map(), fanOut = new Map();
  nodes.forEach(n => { fanIn.set(n.id, 0); fanOut.set(n.id, 0); });
  for (const e of edges) {
    if (fanOut.has(e.source)) fanOut.set(e.source, fanOut.get(e.source) + 1);
    if (fanIn.has(e.target)) fanIn.set(e.target, fanIn.get(e.target) + 1);
  }
  const rank = (m, key) => [...m.entries()]
    .map(([id, v]) => ({ id, [key]: v, name: nameOf(id) }))
    .sort((a, b) => b[key] - a[key] || a.id.localeCompare(b.id))
    .slice(0, 20);
  const fanInRanking = rank(fanIn, 'fanIn');
  const fanOutRanking = rank(fanOut, 'fanOut');

  // ---- C. entry point candidates
  const ENTRY_NAMES = new Set(['index.ts','index.js','main.ts','main.js','app.ts','app.js','server.ts','server.js','mod.rs','main.go','main.py','main.rs','manage.py','app.py','wsgi.py','asgi.py','run.py','__main__.py','Application.java','Main.java','Program.cs','config.ru','index.php','App.swift','Application.kt','main.cpp','main.c']);
  const foSorted = [...fanOut.values()].sort((a, b) => b - a);
  const fiSorted = [...fanIn.values()].sort((a, b) => a - b);
  const foTop10 = foSorted.length ? foSorted[Math.max(0, Math.floor(foSorted.length * 0.1) - 1)] : 0;
  const fiBot25 = fiSorted.length ? fiSorted[Math.max(0, Math.ceil(fiSorted.length * 0.25) - 1)] : 0;

  const candidates = [];
  for (const n of nodes) {
    const fp = n.filePath || '';
    const depth = fp ? fp.split('/').length : 1;
    let score = 0;
    if (n.type === 'document') {
      if (/^README\.md$/i.test(fp)) score += 5;
      else if (depth === 1 && /\.md$/i.test(fp)) score += 2;
    } else {
      if (ENTRY_NAMES.has(n.name)) score += 3;
      if (depth <= 2) score += 1;
      if ((fanOut.get(n.id) || 0) >= foTop10 && foTop10 > 0) score += 1;
      if ((fanIn.get(n.id) || 0) <= fiBot25) score += 1;
    }
    if (score > 0) candidates.push({ id: n.id, score, name: n.name, type: n.type, summary: n.summary || '' });
  }
  candidates.sort((a, b) => b.score - a.score || a.id.localeCompare(b.id));
  const entryPointCandidates = candidates.slice(0, 5);

  // ---- D. BFS from top code entry point
  const TRAVERSE = new Set(['imports', 'calls']);
  const adj = new Map();
  nodes.forEach(n => adj.set(n.id, []));
  for (const e of edges) {
    if (TRAVERSE.has(e.type) && adj.has(e.source) && byId.has(e.target)) adj.get(e.source).push(e.target);
  }
  const codeCandidates = candidates.filter(c => c.type !== 'document');
  const start = (codeCandidates[0] || candidates[0] || nodes[0] || {}).id;
  const order = [], depthMap = {};
  if (start) {
    const q = [start];
    depthMap[start] = 0;
    while (q.length) {
      const cur = q.shift();
      order.push(cur);
      for (const nx of (adj.get(cur) || [])) {
        if (!(nx in depthMap)) { depthMap[nx] = depthMap[cur] + 1; q.push(nx); }
      }
    }
  }
  const byDepth = {};
  for (const [id, d] of Object.entries(depthMap)) {
    (byDepth[d] = byDepth[d] || []).push(id);
  }

  // ---- E. non-code inventory
  const bucket = { documentation: [], infrastructure: [], data: [], config: [] };
  const MAP = { document: 'documentation', service: 'infrastructure', pipeline: 'infrastructure', resource: 'infrastructure', table: 'data', schema: 'data', endpoint: 'data', config: 'config' };
  for (const n of nodes) {
    const b = MAP[n.type];
    if (b) bucket[b].push({ id: n.id, name: n.name, type: n.type, summary: n.summary || '' });
  }

  // ---- F. tightly coupled clusters
  const pairKey = (a, b) => a < b ? a + '||' + b : b + '||' + a;
  const undirected = new Map(); // pairKey -> count
  const dirSet = new Set();
  for (const e of edges) {
    if (!byId.has(e.source) || !byId.has(e.target) || e.source === e.target) continue;
    dirSet.add(e.source + '>' + e.target + '#' + e.type);
    const k = pairKey(e.source, e.target);
    undirected.set(k, (undirected.get(k) || 0) + 1);
  }
  // seed clusters from mutual relationships
  const seeds = [];
  for (const e of edges) {
    if (!byId.has(e.source) || !byId.has(e.target) || e.source === e.target) continue;
    if (dirSet.has(e.target + '>' + e.source + '#' + e.type)) {
      const k = pairKey(e.source, e.target);
      if (!seeds.some(s => s.key === k)) seeds.push({ key: k, nodes: [e.source, e.target] });
    }
  }
  // fallback seeds: strongest undirected pairs
  if (seeds.length < 5) {
    [...undirected.entries()].sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0])).forEach(([k, c]) => {
      if (c >= 2 && seeds.length < 10 && !seeds.some(s => s.key === k)) seeds.push({ key: k, nodes: k.split('||') });
    });
  }
  const neighbors = new Map();
  nodes.forEach(n => neighbors.set(n.id, new Set()));
  for (const k of undirected.keys()) {
    const [a, b] = k.split('||');
    neighbors.get(a).add(b); neighbors.get(b).add(a);
  }
  const clusters = [];
  const seen = new Set();
  for (const s of seeds) {
    const set = new Set(s.nodes);
    let grew = true;
    while (grew && set.size < 5) {
      grew = false;
      let best = null, bestLinks = 0;
      for (const cand of neighbors.keys()) {
        if (set.has(cand)) continue;
        let links = 0;
        for (const m of set) if (neighbors.get(cand).has(m)) links++;
        if (links >= 2 && links > bestLinks) { best = cand; bestLinks = links; }
      }
      if (best) { set.add(best); grew = true; }
    }
    const list = [...set].sort();
    const sig = list.join('|');
    if (seen.has(sig)) continue;
    seen.add(sig);
    let edgeCount = 0;
    for (const a of list) for (const b of list) if (a < b) edgeCount += (undirected.get(pairKey(a, b)) || 0);
    clusters.push({ nodes: list, edgeCount });
  }
  clusters.sort((a, b) => b.edgeCount - a.edgeCount || b.nodes.length - a.nodes.length);
  const topClusters = clusters.slice(0, 10);

  // ---- H. node summary index
  const nodeSummaryIndex = {};
  for (const n of nodes) nodeSummaryIndex[n.id] = { name: n.name, type: n.type, summary: n.summary || '' };

  const results = {
    scriptCompleted: true,
    entryPointCandidates,
    fanInRanking,
    fanOutRanking,
    bfsTraversal: { startNode: start, order, depthMap, byDepth },
    nonCodeFiles: bucket,
    clusters: topClusters,
    layers: { count: layers.length, list: layers },
    nodeSummaryIndex,
    totalNodes: nodes.length,
    totalEdges: edges.length
  };
  fs.writeFileSync(outPath, JSON.stringify(results, null, 2));
  console.log('OK ->', outPath);
}

try { main(); } catch (err) { console.error(err && err.stack || String(err)); process.exit(1); }
