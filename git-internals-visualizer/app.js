const $ = (selector, root = document) => root.querySelector(selector);
const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];

const encoder = new TextEncoder();
const icons = {
  folder: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M3 6h7l2 2h9v11H3z"/></svg>',
  file: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M6 3h8l4 4v14H6zM14 3v5h5"/></svg>',
  object: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="m4 7 8-4 8 4-8 4zM4 7v10l8 4 8-4V7M12 11v10"/></svg>',
};

const fixtureOids = {
  readme1: '8f2a9c4a11be7d76046ce946178756781ea53bd2',
  tree1: '31dfb34c1a1af70cd8553a0e1f7fa44260f4f0ee',
  commit1: 'a17e6b19d0dfd80fc4f57d733ae3af6ac4fa2712',
  readme2: 'd7fc73afc9da720bc462be6ebf4451dd0076d364',
  app1: '474c4669fd13708178141e52e0af22b76a2a0afc',
  tree2: '17bd83eb4a106769065ecddef50628814b7ee4bc',
  commit2: 'c42ad98c3c2e33e467f6583724c1ec82f2cd59bb',
  featureBlob: 'f19f1ed094f593360758f6ad7dd9b490b1a0a0b2',
  featureTree: 'e6415e35e41b282f8b839fd5cdafaf31dcb30d52',
  featureCommit: 'f53b219878913fbe30d5ca92f093431b849d90ea',
  mainBlob: '935ce32b583657671eee607475db43ea17c5d272f',
  mainTree: '248cad9fa6332278a59a782a51ca1ecb5fc07c96',
  mainCommit: '02c839d91dd9b2e110a93b536449493f1fe56195',
  mergeTree: '198a46765671d61ef64fa6748630bc29f1c8348c',
  mergeCommit: 'bd74ce6fe50792a2d432811d8c3c22afe7b5fd7c',
};

const initialState = () => ({
  step: 0,
  initialized: false,
  currentBranch: 'main',
  refs: {},
  headDetached: false,
  worktree: {},
  index: {},
  objects: {},
  commits: [],
  reflog: [],
  packed: false,
  selectedPath: null,
  lastCommand: 'Choose “git init” to begin',
  lastOutput: 'This simulator exposes the state Git normally keeps out of sight.',
});

let repo = initialState();

function short(oid) {
  return oid ? oid.slice(0, 7) : '0000000';
}

function addObject(oid, type, content, note) {
  repo.objects[oid] = { oid, type, content, note };
}

function addReflog(oldOid, newOid, action) {
  repo.reflog.unshift({ oldOid, newOid, action });
}

function commit({ oid, tree, parents, message, branch, snapshot }) {
  const old = repo.refs[branch] || null;
  addObject(tree, 'tree', Object.entries(snapshot).map(([path, value]) => `100644 ${path}\0${short(value.oid)}`).join('\n'), 'Root tree: a sorted mapping from names to blob OIDs.');
  addObject(oid, 'commit', `tree ${tree}\n${parents.map(parent => `parent ${parent}`).join('\n')}\nauthor Mira Holt <mira@example.test> 1786550400 +0100\ncommitter Mira Holt <mira@example.test> 1786550400 +0100\n\n${message}\n`, 'Commit metadata and graph edges. The file content lives behind the tree.');
  repo.commits.push({ oid, tree, parents, message, branch, snapshot: structuredClone(snapshot) });
  repo.refs[branch] = oid;
  addReflog(old, oid, `commit: ${message}`);
}

const commandSteps = [
  {
    command: 'git init -b main', label: 'Create repository',
    explain: 'Creates the administrative directory, config, and symbolic HEAD. No commit or branch ref value exists yet.',
    run() {
      repo.initialized = true;
      repo.lastOutput = 'Initialized empty Git repository in ~/interview/demo/.git/\nHEAD → refs/heads/main (the ref is still unborn)';
    },
  },
  {
    command: 'edit README.md', label: 'Change worktree',
    explain: 'The working tree changes, but Git has not stored an object and the index still has no entry.',
    run() {
      repo.worktree['README.md'] = { oid: fixtureOids.readme1, content: '# Packet Garden\n\nA tiny protocol explorer.\n', status: 'untracked' };
      repo.lastOutput = 'README.md now exists only in the working tree.\nGit discovers it as untracked during status traversal.';
    },
  },
  {
    command: 'git add README.md', label: 'Write blob + index',
    explain: 'Hashes the file into a blob, writes the loose object if absent, then records its OID, mode, path, and stat cache in the index.',
    run() {
      addObject(fixtureOids.readme1, 'blob', repo.worktree['README.md'].content, 'The blob contains bytes only: no filename, path, or permissions.');
      repo.index['README.md'] = { oid: fixtureOids.readme1, mode: '100644', stage: 0 };
      repo.worktree['README.md'].status = 'staged';
      repo.lastOutput = `wrote blob ${fixtureOids.readme1}\nindex: 100644 ${short(fixtureOids.readme1)} 0\tREADME.md`;
    },
  },
  {
    command: 'git commit -m "root"', label: 'Snapshot index',
    explain: 'Writes a tree from the index, a parentless commit pointing to that tree, then atomically updates refs/heads/main.',
    run() {
      commit({ oid: fixtureOids.commit1, tree: fixtureOids.tree1, parents: [], message: 'root', branch: 'main', snapshot: repo.index });
      repo.worktree['README.md'].status = 'clean';
      repo.lastOutput = `[main (root-commit) ${short(fixtureOids.commit1)}] root\n1 file changed\nref main: 0000000 → ${short(fixtureOids.commit1)}`;
    },
  },
  {
    command: 'edit README.md + app.js', label: 'Make second change',
    explain: 'The worktree can diverge from both HEAD and the index. The index deliberately remains the previous proposed commit.',
    run() {
      repo.worktree['README.md'] = { oid: fixtureOids.readme2, content: '# Packet Garden\n\nExplore protocols one byte at a time.\n', status: 'modified' };
      repo.worktree['app.js'] = { oid: fixtureOids.app1, content: 'export function decode(packet) {\n  return [...packet];\n}\n', status: 'untracked' };
      repo.lastOutput = ' M README.md\n?? app.js\n\nHEAD and index still describe the root snapshot.';
    },
  },
  {
    command: 'git add .', label: 'Stage second snapshot',
    explain: 'Creates two blob entries in the index. Staging is a content operation, not merely a list of changed paths.',
    run() {
      addObject(fixtureOids.readme2, 'blob', repo.worktree['README.md'].content, 'Second README content; the first blob still exists.');
      addObject(fixtureOids.app1, 'blob', repo.worktree['app.js'].content, 'JavaScript file bytes, independent of its app.js name.');
      repo.index = {
        'README.md': { oid: fixtureOids.readme2, mode: '100644', stage: 0 },
        'app.js': { oid: fixtureOids.app1, mode: '100644', stage: 0 },
      };
      Object.values(repo.worktree).forEach(file => { file.status = 'staged'; });
      repo.lastOutput = `index now names two blobs:\n100644 ${short(fixtureOids.readme2)} 0\tREADME.md\n100644 ${short(fixtureOids.app1)} 0\tapp.js`;
    },
  },
  {
    command: 'git commit -m "decode"', label: 'Create child commit',
    explain: 'The commit records exactly one parent, producing a directed acyclic graph edge to the previous commit.',
    run() {
      commit({ oid: fixtureOids.commit2, tree: fixtureOids.tree2, parents: [fixtureOids.commit1], message: 'decode packets', branch: 'main', snapshot: repo.index });
      Object.values(repo.worktree).forEach(file => { file.status = 'clean'; });
      repo.lastOutput = `[main ${short(fixtureOids.commit2)}] decode packets\nparent ${short(fixtureOids.commit1)}\nmain moved; the parent commit did not change.`;
    },
  },
  {
    command: 'git switch -c feature', label: 'Create + attach ref',
    explain: 'Creates refs/heads/feature at the same commit as main, then changes symbolic HEAD to name feature.',
    run() {
      repo.refs.feature = repo.refs.main;
      repo.currentBranch = 'feature';
      addReflog(repo.refs.main, repo.refs.feature, 'checkout: moving from main to feature');
      repo.lastOutput = `Switched to a new branch 'feature'\nHEAD → refs/heads/feature → ${short(repo.refs.feature)}`;
    },
  },
  {
    command: 'edit app.js', label: 'Feature work',
    explain: 'Only the working copy changes. The current branch affects which commit is checked out, not how blobs are formed.',
    run() {
      repo.worktree['app.js'] = { oid: fixtureOids.featureBlob, content: 'export function decode(packet) {\n  return [...packet].map(byte => byte.toString(16));\n}\n', status: 'modified' };
      repo.lastOutput = ' M app.js\nfeature, main, HEAD, and the index still resolve to the decode snapshot.';
    },
  },
  {
    command: 'git add app.js && git commit', label: 'Advance feature',
    explain: 'Writes a new blob, tree, and commit, then advances feature only. main keeps pointing at the common ancestor.',
    run() {
      addObject(fixtureOids.featureBlob, 'blob', repo.worktree['app.js'].content, 'Feature-side version, a good candidate for delta compression.');
      repo.index['app.js'] = { oid: fixtureOids.featureBlob, mode: '100644', stage: 0 };
      commit({ oid: fixtureOids.featureCommit, tree: fixtureOids.featureTree, parents: [fixtureOids.commit2], message: 'format bytes', branch: 'feature', snapshot: repo.index });
      repo.worktree['app.js'].status = 'clean';
      repo.lastOutput = `[feature ${short(fixtureOids.featureCommit)}] format bytes\nfeature advanced; main remains at ${short(fixtureOids.commit2)}.`;
    },
  },
  {
    command: 'git switch main', label: 'Move HEAD to main',
    explain: 'HEAD now names main. Git updates the index and working tree to main’s snapshot while feature stays untouched.',
    run() {
      const old = repo.refs.feature;
      repo.currentBranch = 'main';
      const mainCommit = repo.commits.find(item => item.oid === repo.refs.main);
      repo.index = structuredClone(mainCommit.snapshot);
      repo.worktree['app.js'] = { oid: fixtureOids.app1, content: repo.objects[fixtureOids.app1].content, status: 'clean' };
      addReflog(old, repo.refs.main, 'checkout: moving from feature to main');
      repo.lastOutput = `Switched to branch 'main'\nworktree restored app.js from ${short(fixtureOids.app1)}.`;
    },
  },
  {
    command: 'edit app.js', label: 'Diverge main',
    explain: 'The same base file now changes differently on main. The graph will diverge after committing.',
    run() {
      repo.worktree['app.js'] = { oid: fixtureOids.mainBlob, content: 'export function decode(packet) {\n  if (!packet.length) return [];\n  return [...packet];\n}\n', status: 'modified' };
      repo.lastOutput = ' M app.js\nThe worktree differs from main’s index and from feature’s committed blob.';
    },
  },
  {
    command: 'git add app.js && git commit', label: 'Advance main',
    explain: 'main and feature now have different descendants of the same commit. Neither branch contains the other.',
    run() {
      addObject(fixtureOids.mainBlob, 'blob', repo.worktree['app.js'].content, 'Main-side version that diverges from the feature blob.');
      repo.index['app.js'] = { oid: fixtureOids.mainBlob, mode: '100644', stage: 0 };
      commit({ oid: fixtureOids.mainCommit, tree: fixtureOids.mainTree, parents: [fixtureOids.commit2], message: 'handle empty packets', branch: 'main', snapshot: repo.index });
      repo.worktree['app.js'].status = 'clean';
      repo.lastOutput = `[main ${short(fixtureOids.mainCommit)}] handle empty packets\nThe history now has two tips and one merge base.`;
    },
  },
  {
    command: 'git merge feature', label: 'Create merge commit',
    explain: 'A true merge commit records two parents. The first parent is the old main tip; the second is the feature tip.',
    run() {
      const mergedContent = 'export function decode(packet) {\n  if (!packet.length) return [];\n  return [...packet].map(byte => byte.toString(16));\n}\n';
      const mergedBlob = '7ec4310406b8a3eaeae44b938c683ba26dbcad02';
      addObject(mergedBlob, 'blob', mergedContent, 'Resolved merge result; neither parent blob is mutated.');
      repo.index['app.js'] = { oid: mergedBlob, mode: '100644', stage: 0 };
      repo.worktree['app.js'] = { oid: mergedBlob, content: mergedContent, status: 'clean' };
      commit({ oid: fixtureOids.mergeCommit, tree: fixtureOids.mergeTree, parents: [fixtureOids.mainCommit, fixtureOids.featureCommit], message: 'merge feature', branch: 'main', snapshot: repo.index });
      repo.lastOutput = `Merge made by the 'ort' strategy.\ncommit ${short(fixtureOids.mergeCommit)} has parents ${short(fixtureOids.mainCommit)} and ${short(fixtureOids.featureCommit)}.`;
    },
  },
  {
    command: 'git gc', label: 'Pack reachable objects',
    explain: 'Writes a pack and index, then prunes redundant loose copies when safe. Object IDs and graph semantics do not change.',
    run() {
      repo.packed = true;
      repo.lastOutput = `Enumerating objects: ${Object.keys(repo.objects).length}\nWriting objects: done\nCreated pack-a842….pack and pack-a842….idx; refs still name the same OIDs.`;
    },
  },
];

function runNextStep() {
  const action = commandSteps[repo.step];
  if (!action) return;
  repo.lastCommand = action.command;
  action.run();
  repo.step += 1;
  renderRepo();
  showToast(action.explain);
}

function renderCommands() {
  $('#command-list').innerHTML = commandSteps.map((step, index) => {
    const stateClass = index < repo.step ? 'done' : index === repo.step ? 'current' : '';
    const disabled = index === repo.step ? '' : 'disabled';
    return `<button class="command-item ${stateClass}" data-command-index="${index}" ${disabled}>
      <span class="command-state">${index < repo.step ? 'ok' : String(index + 1).padStart(2, '0')}</span>
      <span><code>${escapeHtml(step.command)}</code><small>${step.label}</small></span>
    </button>`;
  }).join('');
  $$('[data-command-index]').forEach(button => button.addEventListener('click', runNextStep));
  const next = commandSteps[repo.step];
  $('#command-help').innerHTML = next
    ? `<span>Next mutation</span><p>${next.explain}</p>`
    : '<span>Repository complete</span><p>You built divergent history, merged it, and packed the reachable object graph.</p>';
}

function treeRows() {
  if (!repo.initialized) return [];
  const rows = [
    { path: 'README.md', depth: 0, type: 'file', exists: Boolean(repo.worktree['README.md']), content: repo.worktree['README.md']?.content, note: 'Working-tree file; not stored under .git.' },
    { path: 'app.js', depth: 0, type: 'file', exists: Boolean(repo.worktree['app.js']), content: repo.worktree['app.js']?.content, note: 'Working-tree file; its current bytes may differ from the index.' },
    { path: '.git', depth: 0, type: 'folder', exists: true, git: true, note: 'Administrative directory: object database, refs, index, config, and logs.' },
    { path: '.git/HEAD', depth: 1, type: 'file', exists: true, content: `ref: refs/heads/${repo.currentBranch}\n`, note: 'A symbolic ref in normal attached-HEAD operation.' },
    { path: '.git/config', depth: 1, type: 'file', exists: true, content: '[core]\n\trepositoryformatversion = 0\n\tfilemode = true\n\tbare = false\n', note: 'Repository-local configuration.' },
    { path: '.git/index', depth: 1, type: 'file', exists: Object.keys(repo.index).length > 0, content: indexContent(), note: 'Binary DIRC cache. It stores the proposed next snapshot plus stat data.' },
    { path: '.git/objects', depth: 1, type: 'folder', exists: true, note: 'Content-addressed object database.' },
  ];

  if (repo.packed) {
    rows.push(
      { path: '.git/objects/pack', depth: 2, type: 'folder', exists: true, note: 'Packed object storage.' },
      { path: '.git/objects/pack/pack-a842.pack', depth: 3, type: 'object', exists: true, content: 'PACK  00 00 00 02  00 00 00 …\n[compressed object entries]\n[pack checksum]', note: 'Self-contained pack: header, entries, and hash trailer.' },
      { path: '.git/objects/pack/pack-a842.idx', depth: 3, type: 'object', exists: true, content: '[fanout table: 256 × uint32]\n[sorted OIDs]\n[CRC32 table]\n[offset table]\n[checksums]', note: 'OID-to-offset index for fast lookup inside the pack.' },
    );
  } else {
    Object.values(repo.objects).slice(-7).forEach(object => rows.push({
      path: `.git/objects/${object.oid.slice(0, 2)}/${object.oid.slice(2)}`,
      depth: 2,
      type: 'object',
      exists: true,
      oid: object.oid,
      content: object.content,
      note: object.note,
      objectType: object.type,
    }));
  }

  rows.push(
    { path: '.git/refs', depth: 1, type: 'folder', exists: true, note: 'Namespace for movable references.' },
    { path: '.git/refs/heads', depth: 2, type: 'folder', exists: true, note: 'Local branch refs.' },
  );
  Object.entries(repo.refs).forEach(([name, oid]) => rows.push({ path: `.git/refs/heads/${name}`, depth: 3, type: 'file', exists: true, content: `${oid}\n`, note: `The branch is a file naming commit ${short(oid)}.` }));
  if (repo.reflog.length) {
    rows.push({ path: '.git/logs', depth: 1, type: 'folder', exists: true, note: 'Local journals of ref movements.' });
    rows.push({ path: '.git/logs/HEAD', depth: 2, type: 'file', exists: true, content: reflogContent(), note: 'Recovery journal for where HEAD has pointed locally.' });
  }
  return rows.filter(row => row.exists);
}

function indexContent() {
  const entries = Object.entries(repo.index).map(([path, item]) => `${item.mode} ${short(item.oid)} stage=${item.stage} ${path}`).join('\n');
  return `DIRC\nversion 2\n${Object.keys(repo.index).length} entries\n\n${entries}\n\n[trailing checksum]`;
}

function reflogContent() {
  return [...repo.reflog].reverse().map(entry => `${short(entry.oldOid)} ${short(entry.newOid)} Mira Holt <mira@example.test>\t${entry.action}`).join('\n');
}

function renderFileTree() {
  const rows = treeRows();
  if (!rows.length) {
    $('#file-tree').innerHTML = '<div class="state-empty">The working directory has no .git directory.</div>';
    renderInspector(null);
    return;
  }
  $('#file-tree').innerHTML = rows.map((row, index) => {
    const selected = row.path === repo.selectedPath ? 'selected' : '';
    const icon = row.type === 'folder' ? icons.folder : row.type === 'object' ? icons.object : icons.file;
    const label = row.path.split('/').pop();
    return `<button class="tree-row ${selected} ${row.git ? 'git-dir' : ''}" style="--depth:${row.depth}" data-tree-row="${index}">${icon}<span>${escapeHtml(label)}</span>${row.oid ? `<span class="oid-fragment">${short(row.oid)}</span>` : ''}</button>`;
  }).join('');
  $$('[data-tree-row]').forEach(button => button.addEventListener('click', () => {
    const row = rows[Number(button.dataset.treeRow)];
    repo.selectedPath = row.path;
    renderFileTree();
    renderInspector(row);
  }));

  const selected = rows.find(row => row.path === repo.selectedPath);
  if (selected) renderInspector(selected);
  else if (repo.step > 0) {
    const preferred = rows.find(row => row.path === '.git/HEAD');
    if (preferred) {
      repo.selectedPath = preferred.path;
      renderInspector(preferred);
    }
  }
}

function renderInspector(row) {
  if (!row) {
    $('#file-inspector').innerHTML = '<div class="empty-state"><svg viewBox="0 0 48 48" aria-hidden="true"><path d="M9 10h13l4 5h13v23H9z"/><path d="M15 23h18M15 29h12"/></svg><h3>No repository yet</h3><p>Run <code>git init</code> to create the administrative directory.</p></div>';
    return;
  }
  const kind = row.objectType || row.type;
  const content = row.type === 'folder' ? directoryDescription(row.path) : row.content || '(empty file)';
  $('#file-inspector').innerHTML = `<div class="inspector-head"><div><span>Selected path</span><h3>${escapeHtml(row.path)}</h3></div><span class="type-pill">${escapeHtml(kind)}</span></div>
    <div class="inspector-body"><pre>${escapeHtml(content)}</pre><div class="inspector-facts">
      <div><span>Role</span><code>${escapeHtml(row.note || 'Repository data')}</code></div>
      <div><span>Identity</span><code>${row.oid || (row.type === 'folder' ? 'path-based directory' : 'not an object ID')}</code></div>
    </div></div>`;
}

function directoryDescription(path) {
  const descriptions = {
    '.git': 'Git directory\n\nEverything needed to reconstruct refs and reachable history lives here. The working tree is a checkout, not the repository itself.',
    '.git/objects': 'Object database\n\nLoose objects are zlib-compressed canonical bytes addressed by OID. Packed objects preserve those logical identities using different physical representations.',
    '.git/objects/pack': 'Pack directory\n\n.pack stores compressed representations. .idx maps OIDs to offsets. Optional .rev reverses the offset order mapping.',
    '.git/refs': 'Ref namespace\n\nheads/ for local branches, tags/ for tags, remotes/ for remote-tracking refs. Refs may also be consolidated into packed-refs.',
    '.git/refs/heads': `Local heads\n\n${Object.entries(repo.refs).map(([name, oid]) => `${name} → ${short(oid)}`).join('\n') || 'No branch has a value yet.'}`,
    '.git/logs': 'Reflogs\n\nLocal, expiring journals of ref updates. They are not shared by fetch or clone and are not part of the object graph.',
  };
  return descriptions[path] || 'Directory entry';
}

function headCommit() {
  const oid = repo.refs[repo.currentBranch];
  return repo.commits.find(item => item.oid === oid);
}

function stateFiles(items, type) {
  const entries = Object.entries(items || {});
  if (!entries.length) return '<div class="state-empty">No entries</div>';
  return entries.map(([path, item]) => `<div class="state-file"><span>${escapeHtml(path)}</span><span>${type === 'worktree' ? escapeHtml(item.status) : `${short(item.oid)} · ${item.mode || 'bytes'}`}</span></div>`).join('');
}

function renderThreeTrees() {
  const head = headCommit();
  $('#three-trees').innerHTML = `
    <section class="tree-state"><code class="label">HEAD^{tree}</code><h3>Committed snapshot</h3><p>Read from the tree object named by the current commit. Immutable.</p>${stateFiles(head?.snapshot)}</section>
    <section class="tree-state"><code class="label">.git/index</code><h3>Proposed snapshot</h3><p>Binary staging cache. This is what the next ordinary commit will snapshot.</p>${stateFiles(repo.index)}</section>
    <section class="tree-state"><code class="label">working directory</code><h3>Editable checkout</h3><p>Normal filesystem bytes. Commands compare this against the index.</p>${stateFiles(repo.worktree, 'worktree')}</section>`;
}

function renderReflog() {
  $('#reflog-list').innerHTML = repo.reflog.length
    ? repo.reflog.map((entry, index) => `<div class="reflog-entry"><code>HEAD@{${index}}</code><span>${short(entry.newOid)}</span><strong>${escapeHtml(entry.action)}</strong></div>`).join('')
    : '<div class="state-empty">No ref has moved yet. Reflogs record updates, not file edits.</div>';
}

function renderGraph() {
  const container = $('#commit-graph');
  if (!repo.commits.length) {
    container.innerHTML = '<div class="state-empty">No reachable commit graph yet.</div>';
    $('#graph-note').textContent = 'No commits exist. An unborn branch is a ref name that has no value yet.';
    return;
  }
  const positions = {
    [fixtureOids.commit1]: { x: 28, y: 292 },
    [fixtureOids.commit2]: { x: 92, y: 220 },
    [fixtureOids.featureCommit]: { x: 16, y: 133 },
    [fixtureOids.mainCommit]: { x: 125, y: 133 },
    [fixtureOids.mergeCommit]: { x: 74, y: 42 },
  };
  let html = '';
  repo.commits.forEach(item => {
    const to = positions[item.oid] || { x: 70, y: 40 };
    item.parents.forEach(parentOid => {
      const from = positions[parentOid];
      if (!from) return;
      const x1 = from.x + 54, y1 = from.y, x2 = to.x + 54, y2 = to.y + 65;
      const length = Math.hypot(x2 - x1, y2 - y1);
      const angle = Math.atan2(y2 - y1, x2 - x1) * 180 / Math.PI - 90;
      html += `<i class="graph-edge" style="left:${x1}px;top:${y1}px;height:${length}px;transform:rotate(${angle}deg)"></i>`;
    });
  });
  repo.commits.forEach(item => {
    const pos = positions[item.oid] || { x: 70, y: 40 };
    const labels = Object.entries(repo.refs).filter(([, oid]) => oid === item.oid).map(([name]) => `<span>${name}${name === repo.currentBranch ? ' · HEAD' : ''}</span>`).join('');
    html += `<div class="commit-node" style="--x:${pos.x}px;--y:${pos.y}px"><code>${short(item.oid)}</code><strong>${escapeHtml(item.message)}</strong>${labels ? `<div class="ref-labels">${labels}</div>` : ''}</div>`;
  });
  container.innerHTML = html;
  $('#graph-note').textContent = repo.commits.at(-1).parents.length === 2
    ? 'The merge has two ordered parents. First-parent traversal follows the old main line; full traversal includes both histories.'
    : 'Commit edges point to parents, so history is traversed backward from refs. Branches are labels, not containers.';
}

function renderRepo() {
  renderCommands();
  renderFileTree();
  renderThreeTrees();
  renderReflog();
  renderGraph();
  const hasCommit = Boolean(repo.refs[repo.currentBranch]);
  $('#branch-indicator').textContent = repo.initialized ? (hasCommit ? repo.currentBranch : 'main · unborn') : 'unborn branch';
  $('#terminal-branch').textContent = hasCommit ? `(${repo.currentBranch})` : '(no commits)';
  $('#terminal-command').textContent = repo.lastCommand;
  $('#terminal-output').textContent = repo.lastOutput;
}

function resetRepo() {
  repo = initialState();
  renderRepo();
  showToast('Repository state reset. The explanatory content and object workbench are unchanged.');
}

const objectSamples = {
  blob: {
    payload: 'hello, internals\n',
    title: 'Blobs do not know filenames',
    explanation: 'A blob is an uninterpreted byte string. The filename and mode live in a tree entry, so one blob can appear at many paths.',
  },
  tree: {
    payload: '100644 README.md 8f2a9c4a11be7d76046ce946178756781ea53bd2\n100644 app.js 474c4669fd13708178141e52e0af22b76a2a0afc',
    title: 'Trees are binary directory maps',
    explanation: 'Each teaching line is encoded as “mode SP name NUL raw-OID-bytes”. Entries are ordered by Git’s tree-name comparison, not by locale.',
  },
  commit: {
    payload: `tree 17bd83eb4a106769065ecddef50628814b7ee4bc\nparent a17e6b19d0dfd80fc4f57d733ae3af6ac4fa2712\nauthor Mira Holt <mira@example.test> 1786550400 +0100\ncommitter Mira Holt <mira@example.test> 1786550400 +0100\n\ndecode packets\n`,
    title: 'A commit is metadata plus edges',
    explanation: 'The tree is the snapshot. Parent lines form the DAG. Author and committer are distinct identities and timestamps; neither changes the tree.',
  },
  tag: {
    payload: `object c42ad98c3c2e33e467f6583724c1ec82f2cd59bb\ntype commit\ntag v1.0.0\ntagger Mira Holt <mira@example.test> 1786550400 +0100\n\nRelease the decoder.\n`,
    title: 'Annotated tags are objects',
    explanation: 'An annotated tag can carry a message, tagger, and signature. A lightweight tag is only a ref and creates no tag object.',
  },
};

let selectedObjectType = 'blob';
let currentObjectOid = '';

function hexToBytes(hex) {
  if (!/^[0-9a-f]{40}([0-9a-f]{24})?$/i.test(hex)) throw new Error('Tree OIDs must contain 40 SHA-1 or 64 SHA-256 hex digits.');
  return new Uint8Array(hex.match(/../g).map(byte => Number.parseInt(byte, 16)));
}

function concatBytes(chunks) {
  const length = chunks.reduce((sum, chunk) => sum + chunk.length, 0);
  const result = new Uint8Array(length);
  let offset = 0;
  chunks.forEach(chunk => { result.set(chunk, offset); offset += chunk.length; });
  return result;
}

function payloadBytes(type, value) {
  if (type !== 'tree') return encoder.encode(value);
  const chunks = [];
  const lines = value.split('\n').filter(Boolean);
  for (const line of lines) {
    const match = line.match(/^(\d{5,6}) ([^\0]+) ([0-9a-f]{40}|[0-9a-f]{64})$/i);
    if (!match) throw new Error('Use one tree entry per line: MODE NAME OID.');
    chunks.push(encoder.encode(`${match[1]} ${match[2]}\0`), hexToBytes(match[3]));
  }
  return concatBytes(chunks);
}

function toHex(bytes) {
  return [...bytes].map(byte => byte.toString(16).padStart(2, '0')).join('');
}

async function sha1(bytes) {
  const digest = await crypto.subtle.digest('SHA-1', bytes);
  return toHex(new Uint8Array(digest));
}

async function updateObjectWorkbench() {
  const value = $('#object-payload').value;
  const sample = objectSamples[selectedObjectType];
  try {
    const payload = payloadBytes(selectedObjectType, value);
    const header = encoder.encode(`${selectedObjectType} ${payload.length}\0`);
    const canonical = concatBytes([header, payload]);
    currentObjectOid = await sha1(canonical);
    $('#payload-count').textContent = `${payload.length} bytes`;
    $('#object-header').textContent = `${selectedObjectType} ${payload.length}\\0`;
    $('#payload-preview').textContent = selectedObjectType === 'tree' ? `${toHex(payload).slice(0, 38)}…` : JSON.stringify(value.slice(0, 32));
    $('#object-hash').textContent = currentObjectOid;
    $('#loose-path').textContent = `.git/objects/${currentObjectOid.slice(0, 2)}/${currentObjectOid.slice(2)}`;
    $('#object-explanation').innerHTML = `<h3>${sample.title}</h3><p>${sample.explanation}</p><p><code>OID = SHA-1(canonical bytes)</code></p>`;
  } catch (error) {
    currentObjectOid = '';
    $('#payload-count').textContent = 'invalid payload';
    $('#object-header').textContent = 'cannot encode';
    $('#payload-preview').textContent = error.message;
    $('#object-hash').textContent = 'Fix the payload format';
    $('#loose-path').textContent = '.git/objects/??/…';
    $('#object-explanation').innerHTML = `<h3>Encoding error</h3><p>${escapeHtml(error.message)}</p><p>The workbench refuses to invent an invalid tree encoding.</p>`;
  }
}

function selectObjectType(type) {
  selectedObjectType = type;
  $$('[data-object-type]').forEach(button => button.setAttribute('aria-selected', String(button.dataset.objectType === type)));
  $('#object-type-label').textContent = `${type} payload`;
  $('#object-payload').value = objectSamples[type].payload;
  updateObjectWorkbench();
}

const cloneModes = [
  {
    id: 'full', label: 'Full', command: 'git clone https://host/packet-garden.git', kicker: 'Complete local object closure', title: 'Full network clone',
    description: '<p>The client discovers refs, negotiates commits it already has, receives a pack containing the reachable commits, trees, and blobs, configures <code>origin</code>, then checks out the selected branch.</p>',
    warning: '<strong>Key property</strong><p>After transfer, reachable history can be traversed and checked out offline. “Full” still usually means packed and delta-compressed, not one loose file per object.</p>',
    sourceNote: 'The sample remote advertises refs and capabilities, then upload-pack constructs the requested reachable closure.',
    phases: [['Discover refs', 'Protocol v2 refs and capabilities', 'refs'], ['Negotiate', 'Send wants and known haves', 'DAG'], ['Receive pack', 'Commits + trees + blobs', 'PACK'], ['Configure refs', 'origin and tracking refs', 'refs'], ['Checkout', 'Populate index and worktree', 'files']],
    metrics: [['Commit range', 'complete', 'selected ref history'], ['Object coverage', 'complete', 'reachable closure'], ['Worktree', 'all paths', 'HEAD snapshot'], ['Offline', 'independent', 'history and checkout']],
    disk: [['repo/',0,'folder',''], ['.git/',1,'folder',''], ['objects/pack/pack-full.pack',2,'object','all reachable types'], ['refs/remotes/origin/main',2,'file','tip OID'], ['index',2,'file','full index'], ['README.md',1,'file','materialized'], ['src/',1,'folder','materialized']], action: 'Already materialized', disabled: true,
  },
  {
    id: 'shallow', label: 'Shallow', command: 'git clone --depth=1 --single-branch https://host/packet-garden.git', kicker: 'Commit-graph truncation', title: 'Shallow clone',
    description: '<p>The server sends the tip commit and its required tree/blob closure, but ancestry stops at a declared shallow boundary. Git records boundary commit OIDs in <code>.git/shallow</code> and treats their parents as unavailable for traversal.</p>',
    warning: '<strong>Not a partial clone</strong><p>Objects inside the requested shallow history are complete. Old commits are not “promised missing objects”; cross the boundary with <code>git fetch --deepen</code> or <code>--unshallow</code>.</p>',
    sourceNote: 'Only history near the selected tip is requested. Depth implies single-branch unless explicitly overridden.',
    phases: [['Discover refs', 'Choose one branch tip', 'refs'], ['Negotiate depth', 'Request shallow boundary', 'depth=1'], ['Receive pack', 'Tip + complete tip snapshot', 'PACK'], ['Write boundary', '.git/shallow names cutoff', 'shallow'], ['Checkout', 'All paths at the tip', 'files']],
    metrics: [['Commit range', 'depth 1', 'truncated ancestry'], ['Object coverage', 'tip-complete', 'tree and blobs for tip'], ['Worktree', 'all tip paths', 'ordinary checkout'], ['Offline', 'tip only', 'deepen needs remote']],
    disk: [['repo/',0,'folder',''], ['.git/',1,'folder',''], ['objects/pack/pack-shallow.pack',2,'object','tip closure'], ['shallow',2,'file','boundary OID'], ['refs/remotes/origin/main',2,'file','tip OID'], ['README.md',1,'file','materialized']], action: 'Deepen history', actionResult: 'Fetched older commits and their reachable objects; .git/shallow moved to an earlier boundary.', actionRow: ['objects/pack/pack-deepen.pack',2,'object','new history'],
  },
  {
    id: 'single', label: 'Single branch', command: 'git clone --single-branch --branch main https://host/packet-garden.git', kicker: 'Ref and history selection', title: 'Single-branch clone',
    description: '<p>Only history leading to the chosen branch tip is cloned, and future fetches update only that remote-tracking branch by default. This limits selected refs, not object types.</p>',
    warning: '<strong>Reachability can overlap</strong><p>Objects shared with other branches still arrive when reachable from <code>main</code>. Tags may also introduce reachability unless tag fetching is disabled.</p>',
    sourceNote: 'The selected refspec narrows which branch tip drives history negotiation.',
    phases: [['Discover refs', 'Find requested main tip', 'main'], ['Select refspec', 'Track one remote branch', 'refspec'], ['Negotiate', 'Reachability from main', 'DAG'], ['Receive pack', 'All required object types', 'PACK'], ['Checkout', 'Populate main snapshot', 'files']],
    metrics: [['Commit range', 'main history', 'no depth boundary'], ['Object coverage', 'complete', 'selected reachability'], ['Remote refs', 'one branch', 'origin/main'], ['Offline', 'selected history', 'other refs need fetch']],
    disk: [['repo/',0,'folder',''], ['.git/',1,'folder',''], ['objects/pack/pack-main.pack',2,'object','main reachability'], ['refs/remotes/origin/main',2,'file','only tracked branch'], ['README.md',1,'file','materialized']], action: 'Selection complete', disabled: true,
  },
  {
    id: 'blobless', label: 'Blobless', command: 'git clone --filter=blob:none https://host/packet-garden.git', kicker: 'Partial clone filter', title: 'Blobless partial clone',
    description: '<p>The initial filtered pack contains commits and trees but can omit blobs. It is marked by a sibling <code>.promisor</code> file. Checkout then bulk-fetches blobs needed for the current worktree; historical and unneeded blobs remain absent.</p>',
    warning: '<strong>Critical nuance</strong><p>Blobless does not mean “no blobs on disk.” Current checkout blobs materialize immediately. A later <code>git show</code>, diff, blame, or checkout can fault in more blobs.</p>',
    sourceNote: 'The remote promises omitted blobs and must remain available for demand fetches.',
    phases: [['Discover filter', 'Server advertises filter', 'filter'], ['Negotiate', 'Request blob:none', 'DAG'], ['Promisor pack', 'Commits + trees; blobs omitted', 'PACK'], ['Bulk prefetch', 'Current checkout blobs', 'blobs'], ['Checkout', 'Populate worktree', 'files']],
    metrics: [['Commit range', 'complete', 'all selected history'], ['Object coverage', 'blobs deferred', 'commits and trees local'], ['Worktree', 'all HEAD paths', 'needed blobs fetched'], ['Offline', 'current tree', 'old content may block']],
    disk: [['repo/',0,'folder',''], ['.git/',1,'folder',''], ['objects/pack/pack-promisor.pack',2,'object','commits + trees'], ['objects/pack/pack-promisor.promisor',2,'file','promise marker'], ['objects/pack/pack-checkout.pack',2,'object','HEAD blobs'], ['missing historical blob OIDs',2,'object','inferred; no manifest','promised'], ['README.md',1,'file','materialized']], action: 'Read an old blob', actionResult: 'Object lookup missed locally, fetched the promised blob by OID, then retried successfully.', actionRow: ['objects/pack/pack-lazy-blob.pack',2,'object','demand-fetched blob'],
  },
  {
    id: 'treeless', label: 'Treeless', command: 'git clone --filter=tree:0 https://host/packet-garden.git', kicker: 'Aggressive partial clone', title: 'Treeless partial clone',
    description: '<p>The filtered history initially prioritizes commits while omitting trees and blobs. To check out HEAD, Git must fetch the current tree closure and its blobs. Historical path walks can trigger substantial later tree fetches.</p>',
    warning: '<strong>Latency tradeoff</strong><p>This minimizes initial graph payload for some workloads, but commands inspecting old paths need trees. Git-aware operations try to batch prefetch instead of fetching one object at a time.</p>',
    sourceNote: 'The remote promises omitted trees and their descendant blobs under the partial-clone contract.',
    phases: [['Discover filter', 'Server accepts tree:0', 'filter'], ['Receive commits', 'Commit graph first', 'commits'], ['Promisor mark', 'Missing closure is allowed', 'promise'], ['Fetch HEAD tree', 'Checkout closure in batch', 'tree'], ['Checkout', 'Materialize selected snapshot', 'files']],
    metrics: [['Commit range', 'complete', 'commit objects local'], ['Object coverage', 'trees deferred', 'plus descendant blobs'], ['Worktree', 'HEAD paths', 'closure fetched'], ['Offline', 'fragile', 'history walks may fetch']],
    disk: [['repo/',0,'folder',''], ['.git/',1,'folder',''], ['objects/pack/pack-commits.pack',2,'object','commit graph'], ['objects/pack/pack-commits.promisor',2,'file','promise marker'], ['objects/pack/pack-head-tree.pack',2,'object','checkout closure'], ['missing historical tree OIDs',2,'object','inferred; no manifest','promised'], ['README.md',1,'file','materialized']], action: 'Inspect old tree', actionResult: 'The path walk discovered missing promised trees and fetched them before continuing.', actionRow: ['objects/pack/pack-lazy-trees.pack',2,'object','historical trees'],
  },
  {
    id: 'sparse', label: 'Sparse + blobless', command: 'git clone --filter=blob:none --sparse https://host/packet-garden.git', kicker: 'Object filter plus worktree filter', title: 'Sparse, blobless clone',
    description: '<p>Partial clone reduces the object database; sparse checkout separately limits which tracked paths are populated. <code>--sparse</code> initially checks out top-level files, and <code>git sparse-checkout set</code> grows the path cone.</p>',
    warning: '<strong>Two independent controls</strong><p>Sparse checkout alone saves worktree and index cost, not network history. Combining it with <code>blob:none</code> avoids fetching blobs outside the selected cone until needed.</p>',
    sourceNote: 'Commits and trees arrive through the filter; only blobs required by the sparse cone are bulk-fetched.',
    phases: [['Negotiate', 'Request blob:none', 'filter'], ['Promisor pack', 'Commits + trees', 'PACK'], ['Set sparsity', 'Top-level cone patterns', 'sparse'], ['Prefetch cone', 'Only selected blobs', 'blobs'], ['Sparse checkout', 'Populate included paths', 'files']],
    metrics: [['Commit range', 'complete', 'all selected history'], ['Object coverage', 'blobs deferred', 'outside cone omitted'], ['Worktree', 'top level', 'sparse paths only'], ['Offline', 'inside cone', 'expansion may fetch']],
    disk: [['repo/',0,'folder',''], ['.git/',1,'folder',''], ['objects/pack/pack-promisor.pack',2,'object','commits + trees'], ['info/sparse-checkout',2,'file','cone patterns'], ['README.md',1,'file','materialized'], ['packages/api/',1,'folder','not present','promised'], ['packages/web/',1,'folder','not present','promised']], action: 'Expand packages/api', actionResult: 'Sparse patterns changed; unpack-trees bulk-prefetched required blobs and populated the new cone.', actionRow: ['packages/api/',1,'folder','newly materialized'],
  },
  {
    id: 'local', label: 'Local', command: 'git clone /srv/repos/packet-garden.git', kicker: 'Filesystem optimization', title: 'Local hardlink clone',
    description: '<p>A local-path clone bypasses normal Git transport and copies refs and object directories, hardlinking object files when possible. <code>--no-hardlinks</code> forces physical copies for backup-style isolation.</p>',
    warning: '<strong>Different from --shared</strong><p>Each clone has directory entries for its objects. Removing a source pathname does not remove the clone’s hardlink. Git objects are immutable, so content is not edited through the shared inode.</p>',
    sourceNote: 'No pack negotiation is required; the filesystem supplies copies or hardlinks of existing object files.',
    phases: [['Validate source', 'Reject unsafe object symlinks', 'safety'], ['Copy refs', 'Snapshot HEAD and refs', 'refs'], ['Link objects', 'Hardlink where possible', 'inode'], ['Write config', 'Configure origin path', 'config'], ['Checkout', 'Populate working tree', 'files']],
    metrics: [['Commit range', 'complete', 'copied refs'], ['Object coverage', 'complete', 'shared inodes'], ['Network', 'none', 'filesystem operation'], ['Source deletion', 'safe links', 'unlike alternates']],
    disk: [['repo/',0,'folder',''], ['.git/',1,'folder',''], ['objects/pack/pack-local.pack',2,'object','hardlinked inode'], ['refs/remotes/origin/main',2,'file','copied ref'], ['README.md',1,'file','materialized']], action: 'Hardlinks are safe', disabled: true,
  },
  {
    id: 'shared', label: 'Shared', command: 'git clone --shared /srv/repos/packet-garden.git', kicker: 'Object alternates', title: 'Shared local clone',
    description: '<p>The new repository starts with no objects of its own. <code>.git/objects/info/alternates</code> points object lookup at the source object database.</p>',
    warning: '<strong>Corruption risk</strong><p>If the source drops refs and prunes objects still needed here, this clone breaks. <code>git repack -a</code> copies borrowed objects locally; ordinary <code>git gc</code> intentionally stays local.</p>',
    sourceNote: 'Object reads fall through to the source object database named by the alternates file.',
    phases: [['Copy refs', 'Independent movable names', 'refs'], ['Write alternate', 'Point to source objects', 'path'], ['Skip transfer', 'No initial local objects', '0 bytes'], ['Write index', 'Resolve via alternate', 'index'], ['Checkout', 'Read borrowed blobs', 'files']],
    metrics: [['Commit range', 'complete view', 'through alternate'], ['Own objects', 'none initially', 'borrowed lookup'], ['Disk saving', 'maximum', 'source dependency'], ['Source prune', 'dangerous', 'can corrupt clone']],
    disk: [['repo/',0,'folder',''], ['.git/',1,'folder',''], ['objects/info/alternates',2,'file','source object path'], ['objects/ (own)',2,'folder','initially empty'], ['refs/heads/main',2,'file','independent ref'], ['README.md',1,'file','materialized']], action: 'Repack -a locally', actionResult: 'Copied every reachable borrowed object into a local pack; the clone no longer depends on source retention.', actionRow: ['objects/pack/pack-owned.pack',2,'object','owned closure'],
  },
  {
    id: 'reference', label: 'Reference', command: 'git clone --reference /cache/repo --dissociate https://host/packet-garden.git', kicker: 'Borrow for transfer, then detach', title: 'Reference clone with dissociation',
    description: '<p><code>--reference</code> installs an alternate so known objects can be borrowed during negotiation. <code>--dissociate</code> then copies required borrowed objects locally, retaining transfer savings without permanent dependency.</p>',
    warning: '<strong>Without --dissociate</strong><p>The alternate remains and inherits the same source-pruning risk as <code>--shared</code>. A reference is a local optimization, not a server-side promisor contract.</p>',
    sourceNote: 'The local cache supplies existing objects while the remote transfers only objects absent from that reference.',
    phases: [['Read reference', 'Advertise cached haves', 'cache'], ['Negotiate', 'Remote computes missing set', 'DAG'], ['Receive delta', 'Only cache misses', 'PACK'], ['Dissociate', 'Copy borrowed closure locally', 'repack'], ['Checkout', 'Independent repository', 'files']],
    metrics: [['Commit range', 'complete', 'selected refs'], ['Network', 'cache misses', 'reference supplies haves'], ['Final storage', 'independent', 'after dissociate'], ['Offline', 'complete', 'no alternate needed']],
    disk: [['repo/',0,'folder',''], ['.git/',1,'folder',''], ['objects/pack/pack-missing.pack',2,'object','remote cache misses'], ['objects/pack/pack-dissociated.pack',2,'object','borrowed closure'], ['refs/remotes/origin/main',2,'file','tip OID'], ['README.md',1,'file','materialized']], action: 'Already dissociated', disabled: true,
  },
  {
    id: 'bare', label: 'Bare', command: 'git clone --bare https://host/packet-garden.git packet-garden.git', kicker: 'Repository without checkout', title: 'Bare clone',
    description: '<p>The destination itself is the Git directory: objects, refs, and config live at its root. There is no working tree and no index for a normal checkout. Source branch heads are copied directly to local branch heads.</p>',
    warning: '<strong>Typical use</strong><p>Bare repositories suit servers and object/ref operations. Bare does not mean mirror: it does not automatically copy every ref namespace or install a force-updating mirror refspec.</p>',
    sourceNote: 'Normal object transfer occurs, but the checkout phase is deliberately absent.',
    phases: [['Discover refs', 'Advertise source heads', 'refs'], ['Negotiate', 'Reachability transfer', 'DAG'], ['Receive pack', 'Complete object closure', 'PACK'], ['Copy heads', 'Direct refs/heads mapping', 'refs'], ['No checkout', 'No worktree or index', 'omitted']],
    metrics: [['Commit range', 'complete', 'source heads'], ['Object coverage', 'complete', 'reachable closure'], ['Worktree', 'none', 'bare repository'], ['Index', 'none', 'until explicitly created']],
    disk: [['packet-garden.git/',0,'folder','GIT_DIR'], ['objects/pack/pack-bare.pack',1,'object','reachable closure'], ['refs/heads/main',1,'file','source head'], ['HEAD',1,'file','symbolic ref'], ['config',1,'file','bare = true']], action: 'No checkout phase', disabled: true,
  },
  {
    id: 'mirror', label: 'Mirror', command: 'git clone --mirror https://host/packet-garden.git packet-garden.git', kicker: 'Exact ref projection', title: 'Mirror clone',
    description: '<p>Mirror implies bare, copies all refs—including remote-tracking refs, notes, and other namespaces—and configures a refspec so <code>git remote update</code> force-updates the destination to match the source.</p>',
    warning: '<strong>Destructive by design</strong><p>A mirror is not a backup with independent ref history. Remote update can overwrite or delete mirrored refs to reproduce source state exactly.</p>',
    sourceNote: 'All advertised ref namespaces become projection targets rather than only ordinary branch tracking refs.',
    phases: [['Discover all refs', 'Heads, tags, notes, remotes', 'refs/*'], ['Negotiate', 'Union of mirrored reachability', 'DAG'], ['Receive pack', 'Complete required objects', 'PACK'], ['Map all refs', '+refs/*:refs/*', 'mirror'], ['No checkout', 'Bare destination', 'omitted']],
    metrics: [['Ref scope', 'all refs', 'exact namespaces'], ['Object coverage', 'complete', 'mirrored reachability'], ['Worktree', 'none', 'bare implied'], ['Remote update', 'force maps', 'source projection']],
    disk: [['packet-garden.git/',0,'folder','GIT_DIR'], ['objects/pack/pack-mirror.pack',1,'object','mirrored closure'], ['refs/heads/main',1,'file','mirrored'], ['refs/tags/v2.4',1,'file','mirrored'], ['refs/notes/commits',1,'file','mirrored'], ['config',1,'file','mirror refspec']], action: 'Projection configured', disabled: true,
  },
];

let selectedCloneMode = 0;
let cloneActionComplete = false;

function cloneDiskRows(mode) {
  const rows = [...mode.disk];
  if (cloneActionComplete && mode.actionRow) rows.push([...mode.actionRow, 'materialized']);
  return rows.map(([name, depth, type, note, state]) => {
    const icon = type === 'folder' ? icons.folder : type === 'object' ? icons.object : icons.file;
    const stateClass = state === 'promised' ? 'promised' : state === 'materialized' ? 'newly-materialized' : '';
    return `<div class="disk-row ${stateClass}" style="--disk-depth:${depth}">${icon}<span>${escapeHtml(name)}</span><small>${escapeHtml(note)}</small></div>`;
  }).join('');
}

function renderCloneLab() {
  const mode = cloneModes[selectedCloneMode];
  $('#clone-mode-rail').innerHTML = cloneModes.map((item, index) => `<button role="tab" aria-selected="${index === selectedCloneMode}" data-clone-mode="${index}">${item.label}</button>`).join('');
  $$('[data-clone-mode]').forEach(button => button.addEventListener('click', () => { selectedCloneMode = Number(button.dataset.cloneMode); cloneActionComplete = false; renderCloneLab(); }));
  $('#clone-command').textContent = mode.command;
  const action = $('#clone-action');
  action.textContent = cloneActionComplete ? 'Materialized' : mode.action;
  action.disabled = mode.disabled || cloneActionComplete;
  $('#clone-source-note').textContent = mode.sourceNote;
  $('#clone-pipeline').innerHTML = mode.phases.map(([title, detail, code], index) => {
    const omitted = title.startsWith('No ') || title.startsWith('Skip ') ? 'omitted' : '';
    const active = cloneActionComplete && index === 2 ? 'active' : '';
    return `<div class="clone-phase ${omitted} ${active}" style="--phase:${index}"><span>${String(index + 1).padStart(2, '0')}</span><div><strong>${title}</strong><p>${detail}</p></div><code>${code}</code></div>`;
  }).join('');
  $('#clone-disk-tree').innerHTML = cloneDiskRows(mode);
  $('#clone-metrics').innerHTML = mode.metrics.map(([label, value, note]) => `<div class="clone-metric"><span>${label}</span><strong>${value}</strong><small>${note}</small></div>`).join('');
  $('#clone-kicker').textContent = mode.kicker;
  $('#clone-title').textContent = mode.title;
  $('#clone-description').innerHTML = mode.description;
  $('#clone-warning').innerHTML = mode.warning;
}

function runCloneAction() {
  const mode = cloneModes[selectedCloneMode];
  if (mode.disabled || cloneActionComplete) return;
  cloneActionComplete = true;
  renderCloneLab();
  showToast(mode.actionResult);
}

const topics = [
  {
    id: 'index', label: 'Index stages', number: '01', kicker: 'Three-way merge state',
    title: 'During a conflict, the index becomes a miniature merge database.',
    body: '<p>Most index entries are stage 0. An unresolved path can instead hold up to three entries with the same pathname:</p><ul><li><code>stage 1</code>: merge-base version</li><li><code>stage 2</code>: “ours”, the current branch side</li><li><code>stage 3</code>: “theirs”, the other side</li></ul><p>The conflict markers in the working file are a presentation. The structured three-way evidence lives in the index. <code>git add</code> resolves the path by replacing stages 1–3 with a single stage-0 blob.</p>',
    diagram: ['base :1', '→', 'ours :2', '+', 'theirs :3', '→', 'resolved :0'],
    question: 'Where does Git actually store the three versions of a conflicted file?',
    answer: 'As same-path entries at stages 1, 2, and 3 in the index. Stage 1 is the merge base, 2 is ours, and 3 is theirs. The working-tree conflict markers are not the authoritative structure.',
  },
  {
    id: 'merge', label: 'Merge vs rebase', number: '02', kicker: 'Graph transformation',
    title: 'Merge preserves identities; rebase manufactures new commits.',
    body: '<p>A merge commit joins histories with multiple parent OIDs. Existing commits remain byte-for-byte identical. Rebase selects commits, computes patches relative to their parents, and applies those changes onto a new base.</p><ul><li>New parent OID means new commit bytes.</li><li>New commit bytes mean a new OID.</li><li>Descendant commits also change because their parent line changes.</li></ul><p>Patch equivalence can survive even when commit identity does not; that is why Git has patch IDs and why rebased commits are not “the same commits moved”.</p>',
    diagram: ['A—B—C', 'merge', 'A—B—C—M', 'rebase', 'A—D—B′—C′'],
    question: 'Why does rebasing one old commit rewrite every descendant?',
    answer: 'Each descendant commit stores its parent OID in its content. Changing one parent changes that commit’s OID, which changes the next child’s parent field, cascading through the chain.',
  },
  {
    id: 'reachability', label: 'Reachability + GC', number: '03', kicker: 'Liveness and recovery',
    title: '“Deleted commit” usually means “temporarily unreachable object”.',
    body: '<p>Garbage collection starts from roots: refs, reflogs, the index, and other protected references. It traverses commit, tree, and tag edges to mark reachable objects.</p><p>Resetting a branch does not immediately erase old commits. The reflog usually retains the previous tip, keeping it recoverable and often reachable for expiry purposes. After refs and reflogs expire—and prune grace periods pass—unreachable objects may be removed.</p><ul><li><code>git fsck --unreachable</code> finds dangling graph components.</li><li><code>git reflog</code> gives human-friendly prior ref positions.</li><li>Cruft packs retain unreachable objects with mtimes for safer expiry.</li></ul>',
    diagram: ['refs', '→', 'commits', '→', 'trees', '→', 'blobs'],
    question: 'Does git reset --hard delete commits?',
    answer: 'It moves a ref and resets index/worktree state. Old commits normally remain in the object database and are often named by reflogs. Only later reachability-based pruning can physically remove them.',
  },
  {
    id: 'transport', label: 'Fetch negotiation', number: '04', kicker: 'Distributed graph set difference',
    title: 'Fetch negotiates reachability, then transfers a pack—not a folder diff.',
    body: '<p>The client advertises what it wants and reports commits it already has. The server finds a boundary between known and missing reachable objects, then sends the required graph closure as a pack.</p><p>A thin pack may use <code>REF_DELTA</code> bases the receiver already owns, making the transfer smaller. Before normal on-disk storage, <code>index-pack --fix-thin</code> makes the pack self-contained by appending the missing bases and correcting the pack metadata.</p><ul><li>Protocol v2 separates capability advertisement and commands.</li><li>Bitmaps can accelerate reachability set computation.</li><li>Partial clone filters can omit promised objects for later fetching.</li></ul>',
    diagram: ['want tip', '+', 'have bases', '→', 'missing closure', '→', 'thin pack'],
    question: 'Why can a received thin pack refer to an object it does not contain?',
    answer: 'Because negotiation established that the receiver already has the base. REF_DELTA can name that existing OID. Git fixes the thin pack into a self-contained on-disk pack before ordinary storage.',
  },
  {
    id: 'refs', label: 'Refs + transactions', number: '05', kicker: 'Mutable names over immutable data',
    title: 'Updating history safely is a compare-and-swap problem.',
    body: '<p>A branch is logically a mutable mapping from refname to OID. File-based refs use lockfiles and atomic rename; ref transactions can validate expected old values before committing several updates.</p><p>Push uses the same safety idea across a network. A normal update must be a fast-forward unless policy permits otherwise. <code>--force-with-lease</code> supplies an expected remote value: update only if nobody moved the ref since your observation.</p><ul><li>Symbolic <code>HEAD</code> normally contains <code>ref: refs/heads/main</code>.</li><li>Detached HEAD contains an OID directly.</li><li><code>packed-refs</code> consolidates many refs; loose refs override packed entries.</li></ul>',
    diagram: ['expected old', '+', 'proposed new', '→', 'atomic ref update'],
    question: 'What extra safety does --force-with-lease provide over --force?',
    answer: 'It requires the remote ref to still equal an expected OID. If another writer advanced it, the compare fails instead of blindly overwriting their work.',
  },
  {
    id: 'commitgraph', label: 'Commit-graph', number: '06', kicker: 'Acceleration structures',
    title: 'Git can optimize graph questions without changing the object graph.',
    body: '<p>The commit-graph file caches commit OIDs, parent positions, root tree OIDs, timestamps, and generation numbers. Generation numbers let walks rule out impossible ancestry earlier.</p><p>Changed-path Bloom filters probabilistically answer whether a commit might have changed a path. A negative answer is definitive and skips expensive tree diffs; a positive answer may be a false positive and must be checked.</p><ul><li>The commit objects remain authoritative.</li><li>Commit-graphs can be chained for incremental writes.</li><li>Bitmap indexes answer reachability sets for selected commits using compressed bitsets.</li></ul>',
    diagram: ['objects', '→', 'commit-graph', '+', 'Bloom filters', '→', 'faster walks'],
    question: 'Can a Bloom filter incorrectly hide a commit that changed a path?',
    answer: 'No. Bloom filters may return false positives, not false negatives. “Definitely absent” is safe to skip; “possibly present” requires the ordinary check.',
  },
  {
    id: 'hashes', label: 'SHA-1 + SHA-256', number: '07', kicker: 'Identity and integrity',
    title: 'The hash names canonical content; it is not encryption.',
    body: '<p>Traditional repositories use SHA-1 OIDs; repositories initialized with SHA-256 use 64-hex-digit OIDs and a repository format extension. Git does not mix object formats casually because OIDs are embedded inside trees, commits, and tags.</p><p>Git’s hardened SHA-1 implementation detects known collision techniques, but SHA-256 is the forward object format. Collision resistance matters because a signed commit transitively commits to its root tree and parent history through embedded OIDs.</p><ul><li>Object lookup and corruption detection rely on recomputing identity.</li><li>Second-preimage resistance matters when replacing known content.</li><li>Abbreviated OIDs must be unique in the repository’s current object set.</li></ul>',
    diagram: ['type + size + NUL', '+', 'payload', '→', 'SHA-1 or SHA-256'],
    question: 'Why does changing a file invalidate the containing commit’s OID?',
    answer: 'The file creates a new blob OID; its parent tree embeds that new OID and therefore changes; ancestors up to the root tree change; the commit embeds the new root tree OID and therefore changes.',
  },
];

let selectedTopic = 0;

function renderTopics() {
  $('#topic-rail').innerHTML = topics.map((topic, index) => `<button role="tab" aria-selected="${index === selectedTopic}" data-topic="${index}">${topic.label}<span>${topic.number}</span></button>`).join('');
  $$('[data-topic]').forEach(button => button.addEventListener('click', () => {
    selectedTopic = Number(button.dataset.topic);
    renderTopics();
  }));
  const topic = topics[selectedTopic];
  $('#topic-content').innerHTML = `<span>${topic.kicker}</span><h3>${topic.title}</h3>${topic.body}<div class="topic-diagram">${topic.diagram.map(item => item === '→' || item === '+' ? `<i>${item}</i>` : `<span>${item}</span>`).join('')}</div>`;
  $('#interview-question').textContent = topic.question;
  $('#interview-answer').textContent = topic.answer;
  $('#interview-answer').hidden = true;
  $('#reveal-answer').textContent = 'Reveal answer';
}

function renderPackObjects() {
  const samples = [
    ['blob', '8f/2a9c…', '53 bytes'], ['blob', 'd7/fc73…', '61 bytes'], ['blob', '47/4c46…', '74 bytes'],
    ['blob', 'f1/9f1e…', '96 bytes'], ['tree', '17/bd83…', '73 bytes'], ['commit', 'c4/2ad9…', '228 bytes'],
  ];
  $('#loose-object-field').innerHTML = samples.map(([type, oid, size]) => `<div class="loose-object"><span>OID path · ${oid}</span><strong>${type} object</strong><small>one compressed file · ${size}</small></div>`).join('');
}

function setPackMode(mode) {
  const packed = mode === 'packed';
  $$('[data-pack-mode]').forEach(button => button.classList.toggle('active', button.dataset.packMode === mode));
  $('#loose-object-field').hidden = packed;
  $('#packed-object-view').hidden = !packed;
  $('#run-pack').textContent = packed ? 'Show before' : 'Run git gc';
  $('#pack-state-step').textContent = packed ? 'After packing' : 'Before packing';
  $('#pack-state-title').textContent = packed ? 'One pack file plus a lookup index' : 'Six independent loose-object files';
  $('#pack-state-description').textContent = packed
    ? 'Git may delta-compress similar objects inside the pack. A separate .idx maps every original object ID to its pack offset.'
    : 'Each object is stored in its own path. The path comes from the object ID; the file contains the zlib-compressed canonical object.';
  $('#pack-details').innerHTML = packed
    ? `<article><span class="detail-index">A</span><div><h3>The pack is storage, not identity</h3><p>The same blob, tree, commit, and tag OIDs still address the same canonical content.</p></div></article>
       <article><span class="detail-index">B</span><div><h3>Deltas save repeated bytes</h3><p>A packed entry can store instructions such as “copy this base range, then insert these bytes.” Git reconstructs the target before hashing it.</p></div></article>
       <article><span class="detail-index">C</span><div><h3>The <code>.idx</code> finds entries</h3><p>Git searches the sorted OID table, reads the corresponding offset, then jumps into the <code>.pack</code> file.</p></div></article>`
    : `<article><span class="detail-index">A</span><div><h3>One object, one file</h3><p>The first two OID hex digits name a directory; the remaining digits name the file.</p></div></article>
       <article><span class="detail-index">B</span><div><h3>Each file compresses alone</h3><p>The loose file contains zlib-compressed <code>type + size + NUL + content</code>. Similar versions cannot share bytes.</p></div></article>
       <article><span class="detail-index">C</span><div><h3>Why Git packs later</h3><p>Thousands of tiny files cost filesystem metadata and repeat similar content. Packing consolidates them without changing any OID.</p></div></article>`;
}

function showToast(message) {
  const toast = $('#toast');
  toast.textContent = message;
  toast.classList.add('show');
  clearTimeout(showToast.timer);
  showToast.timer = setTimeout(() => toast.classList.remove('show'), 3900);
}

function escapeHtml(value = '') {
  return String(value).replace(/[&<>'"]/g, character => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', "'": '&#39;', '"': '&quot;' })[character]);
}

function setupTabs() {
  $$('[data-tab]').forEach(button => button.addEventListener('click', () => {
    $$('[data-tab]').forEach(tab => tab.setAttribute('aria-selected', String(tab === button)));
    $$('.tab-panel').forEach(panel => {
      const active = panel.id === button.dataset.tab;
      panel.classList.toggle('active', active);
      panel.hidden = !active;
    });
  }));
}

function setupInteractions() {
  $('#reset-lab').addEventListener('click', resetRepo);
  $$('[data-object-type]').forEach(button => button.addEventListener('click', () => selectObjectType(button.dataset.objectType)));
  $('#object-payload').addEventListener('input', updateObjectWorkbench);
  $('#copy-oid').addEventListener('click', async () => {
    if (!currentObjectOid) return;
    await navigator.clipboard.writeText(currentObjectOid);
    showToast('Object ID copied to the clipboard.');
  });
  $$('[data-pack-mode]').forEach(button => button.addEventListener('click', () => setPackMode(button.dataset.packMode)));
  $('#run-pack').addEventListener('click', () => setPackMode($('#packed-object-view').hidden ? 'packed' : 'loose'));
  $('#clone-action').addEventListener('click', runCloneAction);
  $('#reveal-answer').addEventListener('click', event => {
    const answer = $('#interview-answer');
    answer.hidden = !answer.hidden;
    event.currentTarget.textContent = answer.hidden ? 'Reveal answer' : 'Hide answer';
  });
  setupTabs();
}

function setupReveals() {
  if (!('IntersectionObserver' in window)) {
    $$('.reveal').forEach(element => element.classList.add('visible'));
    return;
  }
  const observer = new IntersectionObserver(entries => entries.forEach(entry => {
    if (entry.isIntersecting) {
      entry.target.classList.add('visible');
      observer.unobserve(entry.target);
    }
  }), { threshold: .08, rootMargin: '0px 0px -30px' });
  $$('.reveal').forEach(element => observer.observe(element));
}

renderRepo();
renderPackObjects();
setPackMode('loose');
renderCloneLab();
renderTopics();
selectObjectType('blob');
setupInteractions();
setupReveals();
