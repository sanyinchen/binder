#!/usr/bin/env node
'use strict';
const fs = require('fs');

function main() {
  const inPath = process.argv[2];
  const outPath = process.argv[3];
  if (!inPath || !outPath) throw new Error('usage: script <input.json> <output.json>');
  const data = JSON.parse(fs.readFileSync(inPath, 'utf8'));
  const nodes = data.fileNodes || [];
  const importEdges = data.importEdges || [];
  const allEdges = data.allEdges || [];

  const byId = new Map(nodes.map(n => [n.id, n]));
  const paths = nodes.map(n => n.filePath || '');

  // ---- common prefix (directory-segment based) ----
  function commonPrefix(ps) {
    if (ps.length === 0) return '';
    const split = ps.map(p => p.split('/'));
    // only meaningful if every file has at least 2 segments
    if (split.some(s => s.length < 2)) return '';
    let pref = [];
    for (let i = 0; i < split[0].length - 1; i++) {
      const seg = split[0][i];
      if (split.every(s => s.length > i + 1 && s[i] === seg)) pref.push(seg);
      else break;
    }
    return pref.length ? pref.join('/') + '/' : '';
  }
  const prefix = commonPrefix(paths);

  function groupOf(p) {
    let rest = p.startsWith(prefix) ? p.slice(prefix.length) : p;
    const parts = rest.split('/');
    if (parts.length === 1) return '(root)';
    return parts[0];
  }

  const directoryGroups = {};
  const groupOfNode = new Map();
  for (const n of nodes) {
    const g = groupOf(n.filePath || '');
    (directoryGroups[g] = directoryGroups[g] || []).push(n.id);
    groupOfNode.set(n.id, g);
  }

  // flat-structure fallback
  if (Object.keys(directoryGroups).length === 1 && directoryGroups['(root)']) {
    const g2 = {};
    for (const n of nodes) {
      const f = n.name || '';
      let k = 'source';
      if (/\.(test|spec)\./.test(f) || /^test_/.test(f)) k = 'test';
      else if (/\.config\./.test(f) || /^(package|tsconfig)\./.test(f)) k = 'config';
      else if (/\.(md|rst)$/.test(f)) k = 'documentation';
      (g2[k] = g2[k] || []).push(n.id);
      groupOfNode.set(n.id, k);
    }
    for (const k of Object.keys(directoryGroups)) delete directoryGroups[k];
    Object.assign(directoryGroups, g2);
  }

  // ---- B. node type groups ----
  const nodeTypeGroups = {};
  for (const n of nodes) (nodeTypeGroups[n.type] = nodeTypeGroups[n.type] || []).push(n.id);

  // ---- C. adjacency / fan-in / fan-out ----
  const fileFanOut = {}, fileFanIn = {};
  for (const e of importEdges) {
    fileFanOut[e.source] = (fileFanOut[e.source] || 0) + 1;
    fileFanIn[e.target] = (fileFanIn[e.target] || 0) + 1;
  }

  // ---- D. cross-category edges ----
  const ccMap = new Map();
  for (const e of allEdges) {
    const s = byId.get(e.source), t = byId.get(e.target);
    if (!s || !t) continue;
    const key = s.type + '|' + t.type + '|' + e.type;
    ccMap.set(key, (ccMap.get(key) || 0) + 1);
  }
  const crossCategoryEdges = [...ccMap.entries()].map(([k, count]) => {
    const [fromType, toType, edgeType] = k.split('|');
    return { fromType, toType, edgeType, count };
  }).sort((a, b) => b.count - a.count);

  // ---- E. inter-group import frequency (all structural edges + imports) ----
  function pairCounts(edges) {
    const m = new Map();
    for (const e of edges) {
      const a = groupOfNode.get(e.source), b = groupOfNode.get(e.target);
      if (a === undefined || b === undefined || a === b) continue;
      const k = a + '|' + b;
      m.set(k, (m.get(k) || 0) + 1);
    }
    return [...m.entries()].map(([k, count]) => {
      const [from, to] = k.split('|');
      return { from, to, count };
    }).sort((x, y) => y.count - x.count);
  }
  const interGroupImports = pairCounts(importEdges);
  const interGroupAllEdges = pairCounts(allEdges);

  // ---- F. intra-group density (use allEdges for a sparse graph) ----
  const intraGroupDensity = {};
  for (const g of Object.keys(directoryGroups)) {
    let internal = 0, total = 0;
    for (const e of allEdges) {
      const a = groupOfNode.get(e.source), b = groupOfNode.get(e.target);
      if (a === g || b === g) {
        total++;
        if (a === g && b === g) internal++;
      }
    }
    intraGroupDensity[g] = { internalEdges: internal, totalEdges: total, density: total ? +(internal / total).toFixed(3) : 0 };
  }

  // ---- G. pattern matching ----
  const DIR_PATTERNS = [
    [['routes','api','controllers','endpoints','handlers','controller','routers','serializers','blueprints'], 'api'],
    [['services','core','lib','domain','logic','internal','signals','composables','mailers','jobs','channels'], 'service'],
    [['models','db','data','persistence','repository','entities','entity','migrations'], 'data'],
    [['components','views','pages','ui','layouts','screens'], 'ui'],
    [['middleware','plugins','interceptors','guards'], 'middleware'],
    [['utils','helpers','common','shared','tools','pkg','templatetags'], 'utility'],
    [['config','constants','env','settings','management','commands'], 'config'],
    [['__tests__','test','tests','spec','specs'], 'test'],
    [['types','interfaces','schemas','contracts','dtos','dto','request','response'], 'types'],
    [['hooks'], 'hooks'],
    [['store','state','reducers','actions','slices'], 'state'],
    [['assets','static','public'], 'assets'],
    [['cmd','bin'], 'entry'],
    [['docs','documentation','wiki'], 'documentation'],
    [['deploy','deployment','infra','infrastructure','k8s','kubernetes','helm','charts','terraform','tf','docker'], 'infrastructure'],
    [['.github','.gitlab','.circleci'], 'ci-cd'],
    [['sql','database','schema'], 'data'],
  ];
  const patternMatches = {};
  for (const g of Object.keys(directoryGroups)) {
    const key = g.toLowerCase();
    let label = null;
    for (const [names, l] of DIR_PATTERNS) if (names.includes(key)) { label = l; break; }
    if (!label) {
      // heuristics for names not in table
      if (/^(examples?|samples?|demos?)$/.test(key)) label = 'example';
      else if (/^(patch(es)?)$/.test(key)) label = 'patch';
      else if (/^(scripts?)$/.test(key)) label = 'script';
      else if (/^(compat|shim|stubs?|polyfill)$/.test(key)) label = 'compat';
      else if (key === '(root)') label = 'root';
      else label = 'unknown';
    }
    patternMatches[g] = label;
  }

  // file-level patterns
  const filePatterns = {};
  for (const n of nodes) {
    const p = n.filePath || '', f = n.name || '';
    let l = null;
    if (/\.(test|spec)\.[^.]+$/.test(f) || /^test_.*\.py$/.test(f) || /_test\.go$/.test(f) || /Test\.java$/.test(f) || /_spec\.rb$/.test(f) || /Tests?\.(php|cs)$/.test(f)) l = 'test';
    else if (/\.d\.ts$/.test(f)) l = 'types';
    else if (/^(Dockerfile|docker-compose\.)/.test(f)) l = 'infrastructure';
    else if (/\.(tf|tfvars)$/.test(f)) l = 'infrastructure';
    else if (/^(Makefile|Jenkinsfile)$/.test(f) || /^\.gitlab-ci\.yml$/.test(f) || p.startsWith('.github/workflows/')) l = /^Makefile$/.test(f) ? 'infrastructure' : 'ci-cd';
    else if (/\.sql$/.test(f)) l = 'data';
    else if (/\.(graphql|gql|proto|aidl)$/.test(f)) l = 'types';
    else if (/\.(md|rst)$/.test(f)) l = 'documentation';
    else if (/\.patch$/.test(f) || /\.diff$/.test(f)) l = 'patch';
    else if (/\.(sh|bash|zsh)$/.test(f)) l = 'script';
    else if (/^(CMakeLists\.txt|Cargo\.toml|go\.mod|Gemfile|pom\.xml|build\.gradle|composer\.json|package\.json|Makefile\.am|meson\.build)$/.test(f)) l = 'config';
    else if (/^(main|index)\.(cpp|cc|c|ts|js|go|rs)$/.test(f) || /^__init__\.py$/.test(f) || /^(Application\.java|Program\.cs|config\.ru|manage\.py)$/.test(f)) l = 'entry';
    else if (/\.(h|hpp|hh|hxx)$/.test(f)) l = 'header';
    if (l) filePatterns[n.id] = l;
  }

  // ---- H. deployment topology ----
  const infraFiles = [];
  let hasDockerfile = false, hasCompose = false, hasK8s = false, hasTerraform = false, hasCI = false;
  for (const n of nodes) {
    const p = n.filePath || '', f = n.name || '';
    if (/^Dockerfile/.test(f)) { hasDockerfile = true; infraFiles.push(p); }
    else if (/^docker-compose/.test(f)) { hasCompose = true; infraFiles.push(p); }
    else if (/(^|\/)(k8s|kubernetes|helm|charts)\//.test(p)) { hasK8s = true; infraFiles.push(p); }
    else if (/\.(tf|tfvars)$/.test(f)) { hasTerraform = true; infraFiles.push(p); }
    else if (p.startsWith('.github/workflows/') || /^(\.gitlab-ci\.yml|Jenkinsfile)$/.test(f)) { hasCI = true; infraFiles.push(p); }
    else if (/^(scripts?|deploy|infra)\//.test(p) && /\.(sh|bash)$/.test(f)) { infraFiles.push(p); }
  }
  const deploymentTopology = { hasDockerfile, hasCompose, hasK8s, hasTerraform, hasCI, infraFiles };

  // ---- I. data pipeline ----
  const dataPipeline = { schemaFiles: [], migrationFiles: [], dataModelFiles: [], apiHandlerFiles: [] };
  for (const n of nodes) {
    const p = n.filePath || '', tags = (n.tags || []).join(' ');
    if (/\.(sql|graphql|gql|proto|aidl|prisma)$/.test(p) || /schema/i.test(tags)) dataPipeline.schemaFiles.push(p);
    if (/migrations?\//.test(p)) dataPipeline.migrationFiles.push(p);
    if (/model|entity|data-model/i.test(tags)) dataPipeline.dataModelFiles.push(p);
    if (/api-handler|endpoint|controller|service\b/i.test(tags)) dataPipeline.apiHandlerFiles.push(p);
  }

  // ---- J. documentation coverage ----
  const docNodes = nodes.filter(n => n.type === 'document' || /\.(md|rst)$/.test(n.name || ''));
  const groupsWithDocs = new Set(docNodes.map(n => groupOfNode.get(n.id)));
  // also: docs that reference code via `documents` edges
  const docTargetGroups = new Set();
  for (const e of allEdges) {
    if (e.type === 'documents') {
      const g = groupOfNode.get(e.target);
      if (g) docTargetGroups.add(g);
    }
  }
  for (const g of docTargetGroups) groupsWithDocs.add(g);
  const totalGroups = Object.keys(directoryGroups).length;
  const docCoverage = {
    groupsWithDocs: groupsWithDocs.size,
    totalGroups,
    coverageRatio: totalGroups ? +(groupsWithDocs.size / totalGroups).toFixed(2) : 0,
    undocumentedGroups: Object.keys(directoryGroups).filter(g => !groupsWithDocs.has(g)),
  };

  // ---- K. dependency direction ----
  const dirMap = new Map();
  for (const src of [importEdges, allEdges.filter(e => ['imports', 'depends_on', 'calls', 'implements'].includes(e.type))]) {
    for (const e of src) {
      const a = groupOfNode.get(e.source), b = groupOfNode.get(e.target);
      if (!a || !b || a === b) continue;
      dirMap.set(a + '|' + b, (dirMap.get(a + '|' + b) || 0) + 1);
    }
  }
  const seen = new Set();
  const dependencyDirection = [];
  for (const [k, c] of dirMap.entries()) {
    const [a, b] = k.split('|');
    const pk = [a, b].sort().join('|');
    if (seen.has(pk)) continue;
    seen.add(pk);
    const rev = dirMap.get(b + '|' + a) || 0;
    if (c >= rev) dependencyDirection.push({ dependent: a, dependsOn: b, forward: c, reverse: rev });
    else dependencyDirection.push({ dependent: b, dependsOn: a, forward: rev, reverse: c });
  }

  const filesPerGroup = {};
  for (const [g, arr] of Object.entries(directoryGroups)) filesPerGroup[g] = arr.length;
  const nodeTypeCounts = {};
  for (const [t, arr] of Object.entries(nodeTypeGroups)) nodeTypeCounts[t] = arr.length;

  const out = {
    scriptCompleted: true,
    commonPrefix: prefix,
    directoryGroups,
    nodeTypeGroups,
    crossCategoryEdges,
    interGroupImports,
    interGroupAllEdges,
    intraGroupDensity,
    patternMatches,
    filePatterns,
    deploymentTopology,
    dataPipeline,
    docCoverage,
    dependencyDirection,
    fileStats: { totalFileNodes: nodes.length, filesPerGroup, nodeTypeCounts },
    fileFanIn,
    fileFanOut,
  };
  fs.writeFileSync(outPath, JSON.stringify(out, null, 2));
  console.log('OK: ' + nodes.length + ' nodes, ' + Object.keys(directoryGroups).length + ' groups');
}

try { main(); } catch (err) { console.error(err.stack || String(err)); process.exit(1); }
