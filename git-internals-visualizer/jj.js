const $ = (selector, root = document) => root.querySelector(selector);
const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];

const icons = {
  folder: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M3 6h7l2 2h9v11H3z"/></svg>',
  file: '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="M6 3h8l4 4v14H6zM14 3v5h5"/></svg>',
};

const baseCommits = () => [
  { change: 'zzzzzzzzzzzz', commit: '0000000', description: 'root()', parents: [], hidden: false, root: true },
];

const initialState = () => ({
  step: 0,
  initialized: false,
  colocated: false,
  commits: baseCommits(),
  operations: [],
  bookmarks: {},
  workingChange: null,
  view: 'commit',
  files: {},
  conflict: false,
  terminalCommand: 'Select “jj git init --colocate” to begin',
  terminalOutput: 'Jujutsu will create a repository view and a working-copy commit.',
});

let jjState = initialState();

const jjCommands = [
  {
    command: 'jj git init --colocate .', label: 'Create both stores',
    explanation: 'Creates sibling .jj and .git directories. The Git backend stores commits; JJ stores operations, views, and workspace state.',
    run() {
      jjState.initialized = true; jjState.colocated = true;
      addCommit({ change: 'qpvuntsmwlqt', commit: '3d45e11', description: '(no description set)', parents: ['0000000'], working: true });
      jjState.workingChange = 'qpvuntsmwlqt';
      addOperation('initialize repo', 'empty view', '@ = qpvuntsm');
      output('Initialized repo in "."\nWorking copy  (@) qpvuntsm 3d45e11 (empty) (no description set)\nParent commit (@-) zzzzzzzz 0000000 (empty) (no description set)');
    },
  },
  {
    command: 'edit README.md', label: 'Automatic snapshot',
    explanation: 'Before the next JJ command, file changes are snapshotted by rewriting the working-copy commit. No add or commit command is required.',
    run() {
      rewrite('qpvuntsmwlqt', '52bb91a', '(no description set)');
      jjState.files['README.md'] = '# Packet Garden\n\nA protocol explorer.\n';
      addOperation('snapshot working copy', 'qpvuntsm 3d45e11', 'qpvuntsm 52bb91a');
      output('Working copy changes:\nA README.md\n\nWorking copy commit was rewritten automatically.');
    },
  },
  {
    command: 'jj describe -m "root docs"', label: 'Describe current change',
    explanation: 'Changing metadata rewrites the commit ID while retaining the same change ID. The previous commit becomes hidden.',
    run() {
      rewrite('qpvuntsmwlqt', '87d2f0c', 'root docs');
      addOperation('describe commit qpvuntsm', '52bb91a no description', '87d2f0c root docs');
      output('Working copy now at: qpvuntsm 87d2f0c root docs\nThe change ID stayed qpvuntsm; the commit ID changed.');
    },
  },
  {
    command: 'jj new', label: 'Start child change',
    explanation: 'Finalizes nothing: the old working-copy commit simply becomes the parent of a new empty working-copy commit.',
    run() {
      setWorking('qpvuntsmwlqt', false);
      addCommit({ change: 'rnvyrznzttwr', commit: 'b412fb8', description: '(no description set)', parents: ['87d2f0c'], working: true });
      jjState.workingChange = 'rnvyrznzttwr';
      addOperation('new empty commit', '@ = qpvuntsm', '@ = rnvyrznz');
      output('Working copy now at: rnvyrznz b412fb8 (empty) (no description set)\nParent commit      : qpvuntsm 87d2f0c root docs');
    },
  },
  {
    command: 'edit app.js && jj describe', label: 'Build second change',
    explanation: 'The working-copy commit evolves as files and description change. It is always a normal commit in the graph.',
    run() {
      jjState.files['app.js'] = 'export const decode = bytes => [...bytes];\n';
      rewrite('rnvyrznzttwr', 'ca792d4', 'decode packets');
      addOperation('snapshot and describe', 'rnvyrznz b412fb8', 'rnvyrznz ca792d4 decode packets');
      output('Working copy changes:\nA app.js\n\nWorking copy now at: rnvyrznz ca792d4 decode packets');
    },
  },
  {
    command: 'jj bookmark set main -r @', label: 'Name the revision',
    explanation: 'A bookmark points to a commit, but it is not “checked out.” Creating new commits does not automatically advance it.',
    run() {
      jjState.bookmarks.main = 'ca792d4';
      addOperation('set bookmark main', 'main absent', 'main → ca792d4');
      output('Created 1 bookmarks pointing to rnvyrznz ca792d4 main | decode packets');
    },
  },
  {
    command: 'jj new @- && edit parser.js', label: 'Create anonymous sibling',
    explanation: 'JJ keeps visible anonymous heads without requiring a branch name. This creates a sibling change from the parent of @.',
    run() {
      setWorking('rnvyrznzttwr', false);
      addCommit({ change: 'sokukzrxkwpq', commit: 'd13a5c2', description: 'parse headers', parents: ['87d2f0c'], working: true });
      jjState.workingChange = 'sokukzrxkwpq';
      jjState.files = { 'README.md': jjState.files['README.md'], 'parser.js': 'export const parse = line => line.split(":");\n' };
      addOperation('new sibling and snapshot', '@ = rnvyrznz', '@ = sokukzrx');
      output('Working copy now at: sokukzrx d13a5c2 parse headers\nParent commit      : qpvuntsm 87d2f0c root docs\n\nThe previous change remains visible at bookmark main.');
    },
  },
  {
    command: 'jj rebase -s main -d @', label: 'Rebase stable change',
    explanation: 'The rebased commit gets a new commit ID but retains its change ID. Descendants would be automatically rebased too.',
    run() {
      const old = visibleByChange('rnvyrznzttwr');
      if (old) old.hidden = true;
      addCommit({ change: 'rnvyrznzttwr', commit: 'f65d021', description: 'decode packets', parents: ['d13a5c2'] });
      jjState.bookmarks.main = 'f65d021';
      addOperation('rebase main onto @', 'main → ca792d4', 'main → f65d021');
      output('Rebased 1 commits onto destination\nRebased rnvyrznz ca792d4 as f65d021\nchange ID rnvyrznz remained stable');
    },
  },
  {
    command: 'jj new @ main', label: 'Record merge conflict',
    explanation: 'A merge working-copy commit can contain a logical conflict. The command succeeds; resolution can happen later.',
    run() {
      setWorking('sokukzrxkwpq', false);
      addCommit({ change: 'tqwwqpnzpmrv', commit: 'ab831e9', description: 'merge parser and decoder', parents: ['d13a5c2', 'f65d021'], working: true, conflict: true });
      jjState.workingChange = 'tqwwqpnzpmrv'; jjState.conflict = true;
      jjState.files['app.js'] = '<<<<<<< Conflict 1 of 1\n...logical conflict materialized...\n>>>>>>> Conflict 1 of 1 ends\n';
      addOperation('new merge commit', '@ + main', 'conflicted merge @');
      output('Working copy now at: tqwwqpnz ab831e9 (conflict) merge parser and decoder\nAdded 1 files, modified 1 files\nThere are unresolved conflicts at these paths:\napp.js');
    },
  },
  {
    command: 'jj undo', label: 'Restore prior operation',
    explanation: 'Undo restores the previous repository view: working-copy assignment, visible heads, and conflict state—not just one pointer.',
    run() {
      const merge = visibleByChange('tqwwqpnzpmrv'); if (merge) merge.hidden = true;
      setWorking('sokukzrxkwpq', true); jjState.workingChange = 'sokukzrxkwpq'; jjState.conflict = false;
      delete jjState.files['app.js'];
      addOperation('undo new merge commit', 'conflicted merge @', '@ = sokukzrx; conflict absent');
      output('Restored to operation: rebase main onto @\nWorking copy now at: sokukzrx d13a5c2 parse headers');
    },
  },
];

function addCommit(commit) { jjState.commits.push({ hidden: false, conflict: false, working: false, ...commit }); }
function addOperation(description, before, after) { jjState.operations.push({ id: opId(jjState.operations.length), description, before, after }); }
function opId(index) { return ['6fca0b2','33ad914','91ee2c7','bc731d0','ee49a13','02d8ff4','4a0317e','7c941aa','add94c0','f0b12e8','c71da04'][index] || `op${index}`; }
function visibleByChange(change) { return [...jjState.commits].reverse().find(commit => commit.change === change && !commit.hidden); }
function setWorking(change, value) { const commit = visibleByChange(change); if (commit) commit.working = value; }
function rewrite(change, newCommit, description) {
  const old = visibleByChange(change); if (!old) return;
  old.hidden = true; old.working = false;
  addCommit({ ...old, commit: newCommit, description, hidden: false, working: true });
}
function output(text) { jjState.terminalCommand = jjCommands[jjState.step].command; jjState.terminalOutput = text; }

function runNext() {
  const command = jjCommands[jjState.step]; if (!command) return;
  command.run(); jjState.step += 1; renderJJ(); showToast(command.explanation);
}

function renderCommands() {
  $('#jj-command-list').innerHTML = jjCommands.map((item, index) => `<button class="jj-command ${index < jjState.step ? 'done' : index === jjState.step ? 'current' : ''}" ${index === jjState.step ? '' : 'disabled'}><span>${index < jjState.step ? 'ok' : String(index + 1).padStart(2,'0')}</span><div><code>${escapeHtml(item.command)}</code><small>${item.label}</small></div></button>`).join('');
  $$('.jj-command').forEach((button, index) => { if (index === jjState.step) button.addEventListener('click', runNext); });
  $('#jj-next-explanation').textContent = jjCommands[jjState.step]?.explanation || 'The workflow is complete. Reset to replay both histories.';
}

const graphPositions = {
  '0000000':[50,440], '3d45e11':[210,370], '52bb91a':[210,370], '87d2f0c':[210,370],
  'b412fb8':[370,285], 'ca792d4':[370,285], 'd13a5c2':[190,190], 'f65d021':[440,190], 'ab831e9':[310,75],
};

function edge(from, to) {
  const a = graphPositions[from], b = graphPositions[to]; if (!a || !b) return '';
  const x1 = a[0] + 75, y1 = a[1], x2 = b[0] + 75, y2 = b[1] + 62;
  const length = Math.hypot(x2-x1,y2-y1), angle = Math.atan2(y2-y1,x2-x1)*180/Math.PI;
  return `<i class="sim-edge" style="left:${x1}px;top:${y1}px;height:${length}px;transform:rotate(${angle-90}deg)"></i>`;
}

function renderCommitGraph() {
  const visibleCommits = jjState.commits.filter(commit => !commit.root && (!commit.hidden || jjState.step >= 3));
  let html = '<div class="sim-graph">';
  visibleCommits.forEach(commit => commit.parents.forEach(parent => { html += edge(parent, commit.commit); }));
  visibleCommits.forEach(commit => {
    const [x,y] = graphPositions[commit.commit] || [300,100];
    const bookmarks = Object.entries(jjState.bookmarks).filter(([,oid]) => oid === commit.commit).map(([name])=>name).join(', ');
    html += `<div class="sim-node ${commit.hidden?'hidden-node':''} ${commit.working?'wc-sim':''} ${commit.conflict?'conflict-node':''}" style="--gx:${x}px;--gy:${y}px"><span>${commit.working?'@ · ':''}${commit.change.slice(0,8)}</span><code>${commit.commit}${commit.hidden?' · hidden':''}</code><strong>${escapeHtml(commit.description)}</strong><small>${bookmarks ? `bookmark: ${bookmarks}` : commit.conflict ? 'first-class conflict' : 'anonymous revision'}</small></div>`;
  });
  html += '</div>'; return html;
}

function renderOperationView() {
  if (!jjState.operations.length) return '<div class="state-empty">No operation has been recorded yet.</div>';
  return `<div class="op-log-list">${[...jjState.operations].reverse().map((op,index)=>`<div class="op-log-row"><code>${op.id}</code><strong>${escapeHtml(op.description)}</strong><span>${index===0?'@':''}</span></div>`).join('')}</div>`;
}

function renderSnapshot() {
  const files = Object.entries(jjState.files);
  return files.length ? `<div class="snapshot-view">${files.map(([path,content])=>`<div class="snapshot-file"><span>${escapeHtml(path)}</span><pre>${escapeHtml(content)}</pre></div>`).join('')}</div>` : '<div class="state-empty">The working-copy commit has an empty tree.</div>';
}

function renderMainView() {
  $('#jj-main-view').innerHTML = jjState.view === 'commit' ? renderCommitGraph() : jjState.view === 'operation' ? renderOperationView() : renderSnapshot();
  $$('[data-jj-view]').forEach(button => button.setAttribute('aria-selected', String(button.dataset.jjView === jjState.view)));
}

function renderCurrentState() {
  const working = visibleByChange(jjState.workingChange);
  const visibleHeads = jjState.commits.filter(commit => !commit.hidden && !commit.root && !jjState.commits.some(child => !child.hidden && child.parents.includes(commit.commit))).map(commit=>commit.change.slice(0,8));
  $('#jj-current-state').innerHTML = `
    <div class="state-block"><span>working copy @</span><code>${working ? `${working.change.slice(0,8)} ${working.commit}` : 'not initialized'}</code></div>
    <div class="state-block"><span>change description</span><strong>${working ? escapeHtml(working.description) : 'none'}</strong></div>
    <div class="state-block"><span>visible heads</span><code>${visibleHeads.join(', ') || 'root only'}</code></div>
    <div class="state-block"><span>bookmarks</span><code>${Object.entries(jjState.bookmarks).map(([name,oid])=>`${name} → ${oid}`).join('\n') || 'none'}</code></div>
    <div class="state-block"><span>operation head</span><code>${jjState.operations.at(-1)?.id || 'none'}</code></div>
    <div class="state-block"><span>conflict state</span><strong>${jjState.conflict ? 'recorded in @' : 'clean'}</strong></div>`;
}

function renderJJ() {
  renderCommands(); renderMainView(); renderCurrentState();
  $('#jj-terminal-command').textContent = jjState.terminalCommand;
  $('#jj-terminal-output').textContent = jjState.terminalOutput;
}

function resetJJ() { jjState = initialState(); renderJJ(); showToast('Repository and operation history reset.'); }

function shortHash(input) {
  let h1=0x811c9dc5, h2=0x9e3779b9;
  for (const char of input) { h1=Math.imul(h1^char.charCodeAt(0),0x01000193); h2=Math.imul(h2+char.charCodeAt(0),0x85ebca6b); }
  return ((h1>>>0).toString(16).padStart(8,'0')+(h2>>>0).toString(16).padStart(8,'0')).slice(0,12);
}

function updateIdentity() {
  const data = `${$('#jj-description').value}\0${$('#jj-parent').value}\0${$('#jj-content').value}`;
  const id = shortHash(data); $('#jj-commit-id').textContent = id; $('#identity-tip').textContent = id.slice(0,7);
}

const revsets = [
  { expression:'@', selected:['E'], title:'The working-copy commit', text:'The symbol <code>@</code> selects the commit currently assigned to this workspace.' },
  { expression:'@-', selected:['D'], title:'Parents of the working copy', text:'The postfix <code>-</code> selects parents. With one parent, <code>@-</code> is one revision.' },
  { expression:'::@', selected:['A','B','D','E'], title:'Ancestors including @', text:'The prefix <code>::</code> walks backward through all ancestors and includes the endpoint.' },
  { expression:'main..@', selected:['D','E'], title:'Reachable from @, not main', text:'Set difference selects revisions in <code>::@</code> that are not in <code>::main</code>.' },
  { expression:'heads(all())', selected:['C','E'], title:'Visible graph heads', text:'Returns revisions with no descendants inside the visible set. Anonymous heads count.' },
  { expression:'mutable()', selected:['D','E'], title:'Rewritable revisions', text:'A configurable alias commonly selects local work that is safe to rewrite, excluding immutable history.' },
];
let activeRevset = 0;
const revNodes = { A:[30,230], B:[150,180], C:[280,85], D:[280,255], E:[430,190] };
const revEdges = [['A','B'],['B','C'],['B','D'],['D','E']];

function renderRevsets() {
  const set = revsets[activeRevset];
  $('#revset-options').innerHTML = revsets.map((item,index)=>`<button class="${index===activeRevset?'active':''}" data-revset="${index}">${escapeHtml(item.expression)}</button>`).join('');
  $$('[data-revset]').forEach(button=>button.addEventListener('click',()=>{activeRevset=Number(button.dataset.revset);renderRevsets();}));
  let graph=''; revEdges.forEach(([a,b])=>{const [x1,y1]=revNodes[a],[x2,y2]=revNodes[b],len=Math.hypot(x2-x1,y2-y1),ang=Math.atan2(y2-y1,x2-x1)*180/Math.PI;graph+=`<i class="rev-line" style="left:${x1+26}px;top:${y1+26}px;width:${len}px;transform:rotate(${ang}deg)"></i>`;});
  Object.entries(revNodes).forEach(([name,[x,y]])=>{graph+=`<div class="rev-node ${set.selected.includes(name)?'selected':''}" style="--rx:${x}px;--ry:${y}px">${name}${name==='E'?' @':''}</div>`;});
  $('#revset-graph').innerHTML=graph; $('#revset-result').textContent=set.selected.join(', '); $('#revset-explanation').innerHTML=`<h3>${set.title}</h3><p>${set.text}</p><p>Revsets return sets, so union, intersection, difference, ancestry, and predicates compose without parsing log output.</p>`;
}

const opExamples = [
  { id:'91ee2c7', title:'snapshot working copy', before:'@ 3d45e11 · empty', after:'@ 52bb91a · README added', detail:'The next JJ command observed filesystem changes and rewrote the working-copy commit.' },
  { id:'02d8ff4', title:'set bookmark main', before:'main absent', after:'main → ca792d4', detail:'The repository view gained a bookmark target. No commit content changed.' },
  { id:'7c941aa', title:'rebase main onto @', before:'main → ca792d4', after:'main → f65d021', detail:'The old commit became hidden, a rewritten successor retained its change ID, and the bookmark followed it.' },
  { id:'add94c0', title:'new conflicted merge', before:'@ → d13a5c2', after:'@ → ab831e9 conflict', detail:'The view’s working-copy target changed to a two-parent commit containing a logical conflict.' },
];
let selectedOp = 2;
function renderOps() {
  $('#op-timeline').innerHTML=opExamples.map((op,index)=>`<button class="op-item ${index===selectedOp?'active':''}" data-op="${index}"><i></i><code>${op.id}</code><span>${op.title}</span></button>`).join('');
  $$('[data-op]').forEach(button=>button.addEventListener('click',()=>{selectedOp=Number(button.dataset.op);renderOps();}));
  const op=opExamples[selectedOp]; $('#op-inspector').innerHTML=`<span>operation ${op.id}</span><h3>${op.title}</h3><p>${op.detail}</p><div class="view-diff"><div><span>view before</span><code>${op.before}</code></div><i>→</i><div><span>view after</span><code>${op.after}</code></div></div><button type="button" id="restore-op">Restore this view</button>`;
  $('#restore-op').addEventListener('click',()=>showToast(`A new operation would restore the repository view from ${op.id}; existing operations remain in the log.`));
}

function showToast(message) { const toast=$('#jj-toast');toast.textContent=message;toast.classList.add('show');clearTimeout(showToast.timer);showToast.timer=setTimeout(()=>toast.classList.remove('show'),3800); }
function escapeHtml(value='') { return String(value).replace(/[&<>'"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;',"'":'&#39;','"':'&quot;'})[c]); }
function setupReveals() { if(!('IntersectionObserver'in window)){ $$('.reveal').forEach(el=>el.classList.add('visible'));return;} const observer=new IntersectionObserver(entries=>entries.forEach(entry=>{if(entry.isIntersecting){entry.target.classList.add('visible');observer.unobserve(entry.target);}}),{threshold:.07,rootMargin:'0px 0px -25px'});$$('.reveal').forEach(el=>observer.observe(el)); }

$('#jj-reset').addEventListener('click',resetJJ);
$$('[data-jj-view]').forEach(button=>button.addEventListener('click',()=>{jjState.view=button.dataset.jjView;renderMainView();}));
['#jj-description','#jj-parent','#jj-content'].forEach(selector=>$(selector).addEventListener('input',updateIdentity));
$('#conflict-toggle').addEventListener('click',event=>{const markers=$('#conflict-markers');markers.hidden=!markers.hidden;event.currentTarget.textContent=markers.hidden?'Materialize conflict':'Hide materialization';});

renderJJ(); updateIdentity(); renderRevsets(); renderOps(); setupReveals();
