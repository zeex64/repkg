"use strict";

const $ = id => document.getElementById(id);

const state = {
  directory: null,
  fallbackFiles: new Map(),
  writable: false,
  screens: [],
  widgetHandles: new Map(),
  screen: null,
  view: null,
  widget: null,
  selectedNode: null,
  selectedArray: 0,
  selectedItem: 0,
  widgetCache: new Map(),
  transform: { x: 40, y: 40, scale: 1 },
};

function setStatus(message) { $("status").textContent = message; }

function updateDirty() {
  const dirty = hasDirty();
  $("dirty-state").textContent = dirty ? "Unsaved changes" : "No unsaved changes";
  $("dirty-state").classList.toggle("dirty", dirty);
  $("save-all").disabled = !dirty || !state.writable;
}

function hasDirty() {
  return state.screens.some(screen => screen.dirty)
    || [...state.widgetCache.values()].some(widget => widget.dirty);
}

function normalizeTag(tag) { return String(tag || "").replace(/^0x/i, "").toUpperCase().padStart(8, "0"); }

async function readJsonHandle(handle) {
  const file = await handle.getFile();
  return JSON.parse(await file.text());
}

async function writeJsonHandle(handle, value) {
  const writable = await handle.createWritable();
  await writable.write(JSON.stringify(value, null, 2) + "\n");
  await writable.close();
}

async function directoryAt(root, parts) {
  let current = root;
  for (const part of parts) current = await current.getDirectoryHandle(part);
  return current;
}

async function scanScreens(directory, prefix = "") {
  const screens = [];
  for await (const [name, handle] of directory.entries()) {
    if (handle.kind === "directory") screens.push(...await scanScreens(handle, `${prefix}${name}/`));
    else if (name === "layout.json") screens.push({ path: `${prefix}${name}`, handle, data: await readJsonHandle(handle) });
  }
  return screens;
}

async function loadDirectory(directory) {
  setStatus("Scanning UI resources…");
  state.screens = [];
  try {
    const screenDir = await directoryAt(directory, ["assets", "ui", "screens"]);
    state.screens = await scanScreens(screenDir);
  } catch (_) {
    // Widget-only packages do not contain named screen roots.
  }
  state.widgetHandles.clear();
  state.widgetCache.clear();
  await addWidgetDirectory(directory);
  state.directory = directory;
  state.writable = true;
  state.fallbackFiles.clear();
  $("project-label").textContent = directory.name;
  await finishProjectLoad();
}

async function loadFallback(files) {
  state.fallbackFiles.clear();
  state.widgetHandles.clear();
  state.widgetCache.clear();
  state.screens = [];
  for (const file of files) state.fallbackFiles.set(file.webkitRelativePath.replace(/\\/g, "/"), file);
  for (const [path, file] of state.fallbackFiles) {
    if (/assets\/ui\/screens\/.+\/layout\.json$/i.test(path)) {
      state.screens.push({ path, handle: file, data: JSON.parse(await file.text()) });
    } else {
      const match = path.match(/assets\/ui\/widget_tables\/([0-9a-f]{8})\.json$/i);
      if (match) state.widgetHandles.set(match[1].toUpperCase(), file);
    }
  }
  state.directory = null;
  state.writable = false;
  $("project-label").textContent = files[0]?.webkitRelativePath.split("/")[0] || "Imported folder";
  await finishProjectLoad();
  setStatus("Folder imported read-only; use a Chromium browser for direct saving");
}

async function finishProjectLoad() {
  state.screens.sort((a, b) => a.data.screen.name.localeCompare(b.data.screen.name));
  const select = $("screen-select");
  select.replaceChildren();
  for (let i = 0; i < state.screens.length; ++i) {
    const option = document.createElement("option");
    option.value = String(i);
    option.textContent = state.screens[i].data.screen.name;
    select.append(option);
  }
  select.disabled = state.screens.length === 0;
  $("view-filter").disabled = state.screens.length === 0;
  $("reload-project").disabled = !state.directory;
  $("add-widget-project").disabled = !window.showDirectoryPicker;
  for (const screen of state.screens) screen.dirty = false;
  updateDirty();
  if (state.screens.length) await selectScreen(0);
  else setStatus("No decoded UI screens were found in this project");
}

async function selectScreen(index) {
  state.screen = state.screens[index] || null;
  state.view = null;
  state.widget = null;
  state.selectedNode = null;
  renderViewList();
  const first = state.screen?.data.views?.[0];
  if (first) await selectView(first);
}

function renderViewList() {
  const list = $("view-list");
  list.replaceChildren();
  const filter = $("view-filter").value.trim().toLowerCase();
  const views = state.screen?.data.views || [];
  for (const view of views) {
    if (filter && !view.name.toLowerCase().includes(filter)
        && !view.name_hash.toLowerCase().includes(filter)) continue;
    const button = document.createElement("button");
    button.className = "view-button" + (view === state.view ? " active" : "");
    button.innerHTML = `${escapeHtml(view.name)}<small>${escapeHtml(view.name_hash)} · ${view.node_count} nodes</small>`;
    button.onclick = () => selectView(view);
    list.append(button);
  }
  list.classList.toggle("empty", list.children.length === 0);
  if (!list.children.length) list.textContent = views.length ? "No matching views" : "No views";
}

async function selectView(view) {
  state.view = view;
  state.selectedNode = null;
  state.selectedArray = 0;
  state.selectedItem = 0;
  renderViewList();
  $("view-title").textContent = view.name;
  $("view-summary").textContent = `${view.node_count} nodes · ${view.edges.length} edges`;
  $("node-search").disabled = false;
  $("fit-graph").disabled = false;
  $("empty-state").hidden = true;
  renderGraph();
  requestAnimationFrame(fitGraph);
  renderNodeInspector();
  renderRawDetails();
  await loadWidgetTable(view.widget_table_tag);
}

async function loadWidgetTable(tag) {
  state.widget = null;
  const key = normalizeTag(tag);
  const handle = state.widgetHandles.get(key);
  if (!handle) {
    $("widget-editor").hidden = true;
    $("widget-missing").hidden = false;
    $("widget-missing").textContent = `Widget table ${tag} is external or not present. Open the depkg project containing that tag to edit its presentation data.`;
    return;
  }
  setStatus(`Loading widget table ${tag}…`);
  if (!state.widgetCache.has(key)) {
    const data = typeof FileSystemFileHandle !== "undefined" && handle instanceof FileSystemFileHandle
      ? await readJsonHandle(handle)
      : JSON.parse(await handle.text());
    state.widgetCache.set(key, { handle, data, dirty: false });
  }
  state.widget = state.widgetCache.get(key);
  $("widget-missing").hidden = true;
  $("widget-editor").hidden = false;
  renderArraySelect();
  setStatus(`Loaded ${state.view.name}`);
}

function hierarchyMaps(view) {
  const parent = new Map();
  const children = new Map();
  for (const edge of view.edges) {
    parent.set(edge.child, edge.parent);
    if (edge.parent !== null) {
      if (!children.has(edge.parent)) children.set(edge.parent, []);
      children.get(edge.parent).push(edge.child);
    }
  }
  return { parent, children };
}

function graphLayout(view) {
  const { parent, children } = hierarchyMaps(view);
  const roots = [];
  for (let i = 0; i < view.node_count; ++i) if (!parent.has(i) || parent.get(i) === null) roots.push(i);
  const positions = new Map();
  const visited = new Set();
  let row = 0;
  function place(node, depth) {
    if (visited.has(node)) return positions.get(node)?.y ?? row++ * 54;
    visited.add(node);
    const kids = (children.get(node) || []).filter(child => !visited.has(child));
    let y;
    if (!kids.length) y = row++ * 54;
    else {
      const ys = kids.map(child => place(child, depth + 1));
      y = (ys[0] + ys[ys.length - 1]) / 2;
    }
    positions.set(node, { x: depth * 170, y });
    return y;
  }
  for (const root of roots) { place(root, 0); row += 1; }
  for (let i = 0; i < view.node_count; ++i) if (!visited.has(i)) place(i, 0);
  return { positions, parent, children };
}

function svgElement(name, attributes = {}) {
  const element = document.createElementNS("http://www.w3.org/2000/svg", name);
  for (const [key, value] of Object.entries(attributes)) element.setAttribute(key, value);
  return element;
}

function renderGraph() {
  const world = $("graph-world");
  world.replaceChildren();
  if (!state.view) return;
  const { positions, parent } = graphLayout(state.view);
  const search = Number.parseInt($("node-search").value, 10);
  for (const [child, parentId] of parent) {
    if (parentId === null || !positions.has(parentId) || !positions.has(child)) continue;
    const a = positions.get(parentId), b = positions.get(child);
    world.append(svgElement("path", {
      class: "edge",
      d: `M ${a.x + 18} ${a.y} C ${a.x + 82} ${a.y}, ${b.x - 82} ${b.y}, ${b.x - 18} ${b.y}`,
    }));
  }
  for (const [node, position] of positions) {
    const isRoot = !parent.has(node) || parent.get(node) === null;
    const group = svgElement("g", {
      class: `graph-node${isRoot ? " root" : ""}${state.selectedNode === node ? " selected" : ""}${search === node ? " search-match" : ""}`,
      transform: `translate(${position.x} ${position.y})`,
    });
    group.append(svgElement("circle", { r: 18 }));
    const text = svgElement("text", { y: 4 });
    text.textContent = String(node);
    group.append(text);
    group.onclick = event => { event.stopPropagation(); selectNode(node); };
    world.append(group);
  }
  applyGraphTransform();
}

function selectNode(node) {
  state.selectedNode = node;
  renderGraph();
  renderNodeInspector();
}

function renderNodeInspector() {
  const target = $("node-inspector");
  target.replaceChildren();
  if (!state.view || state.selectedNode === null) {
    target.className = "empty";
    target.textContent = "Select a graph node";
    return;
  }
  target.className = "";
  const { parent, children } = hierarchyMaps(state.view);
  const node = state.selectedNode;
  const grid = document.createElement("div");
  grid.className = "inspector-grid";
  grid.innerHTML = `<label>Node</label><strong>${node}</strong><label>Parent</label>`;
  const parentInput = document.createElement("input");
  parentInput.type = "number";
  parentInput.min = "0";
  parentInput.max = String(state.view.node_count - 1);
  parentInput.placeholder = "Root";
  parentInput.value = parent.get(node) ?? "";
  parentInput.onchange = () => changeParent(node, parentInput.value);
  grid.append(parentInput);
  const childLabel = document.createElement("label"); childLabel.textContent = "Children";
  const childBox = document.createElement("div"); childBox.className = "children";
  for (const child of children.get(node) || []) {
    const chip = document.createElement("button"); chip.className = "child-chip"; chip.textContent = child;
    chip.onclick = () => selectNode(child); childBox.append(chip);
  }
  if (!childBox.children.length) childBox.textContent = "None";
  grid.append(childLabel, childBox);
  target.append(grid);
}

function changeParent(node, rawValue) {
  const value = rawValue === "" ? null : Number(rawValue);
  if (value !== null && (!Number.isInteger(value) || value < 0 || value >= state.view.node_count || value === node)) {
    setStatus("Parent must be another valid node, or blank for a root");
    renderNodeInspector();
    return;
  }
  if (value !== null) {
    const { parent } = hierarchyMaps(state.view);
    let ancestor = value;
    const visited = new Set();
    while (ancestor !== null && ancestor !== undefined && !visited.has(ancestor)) {
      if (ancestor === node) {
        setStatus("That parent would create a hierarchy cycle");
        renderNodeInspector();
        return;
      }
      visited.add(ancestor);
      ancestor = parent.get(ancestor);
    }
  }
  const edge = state.view.edges.find(row => row.child === node);
  if (edge) edge.parent = value;
  else state.view.edges.push({ parent: value, child: node });
  state.screen.dirty = true;
  updateDirty();
  renderGraph();
  renderNodeInspector();
  setStatus(`Node ${node} parent updated`);
}

function renderArraySelect() {
  const select = $("array-select");
  select.replaceChildren();
  const arrays = state.widget?.data.arrays || [];
  arrays.forEach((array, index) => {
    const option = document.createElement("option");
    option.value = String(index);
    option.textContent = `${array.name} (${array.items.length})`;
    select.append(option);
  });
  state.selectedArray = Math.min(state.selectedArray, Math.max(0, arrays.length - 1));
  select.value = String(state.selectedArray);
  renderArrayEditor();
}

function renderArrayEditor() {
  const array = state.widget?.data.arrays?.[state.selectedArray];
  if (!array) return;
  $("array-summary").innerHTML = `<strong>${escapeHtml(array.name)}</strong><br>Class ${escapeHtml(array.item_class)} · ${array.item_size} bytes each<br>Field ${escapeHtml(array.field_offset)} → data ${escapeHtml(array.data_offset)}`;
  state.selectedItem = Math.min(state.selectedItem, Math.max(0, array.items.length - 1));
  $("item-index").max = String(Math.max(0, array.items.length - 1));
  $("item-index").value = String(state.selectedItem);
  $("item-count").textContent = `of ${array.items.length}`;
  renderItemFields();
}

function renderItemFields() {
  const target = $("item-fields");
  target.replaceChildren();
  const item = state.widget?.data.arrays?.[state.selectedArray]?.items?.[state.selectedItem];
  if (!item) { target.textContent = "This array is empty"; return; }
  if (item.floats_le) {
    item.floats_le.forEach((field, index) => {
      const row = document.createElement("div"); row.className = "field-row";
      row.innerHTML = `<div class="field-title">Float ${index}</div><div class="field-pair"></div>`;
      const bits = document.createElement("input"); bits.value = field.bits; bits.title = "Exact IEEE-754 bits";
      const value = document.createElement("input"); value.value = field.value; value.title = "Readable float32 value";
      bits.onchange = () => editFloatBits(field, bits.value);
      value.onchange = () => editFloatValue(field, value.value);
      row.querySelector(".field-pair").append(bits, value); target.append(row);
    });
  } else {
    (item.words_le || []).forEach((word, index) => {
      const row = document.createElement("div"); row.className = "field-row";
      row.innerHTML = `<div class="field-title">Word ${index} · +0x${(index * 4).toString(16).toUpperCase().padStart(2, "0")}</div>`;
      const input = document.createElement("input"); input.value = word;
      input.onchange = () => {
        const parsed = parseHex32(input.value);
        if (parsed === null) { input.value = word; setStatus("Enter a 32-bit hexadecimal or decimal value"); return; }
        item.words_le[index] = hex32(parsed); markWidgetDirty(); renderItemFields();
      };
      row.append(input); target.append(row);
    });
  }
  if (item.tail_hex) {
    const row = document.createElement("div"); row.className = "field-row";
    row.innerHTML = `<div class="field-title">Tail bytes</div>`;
    const input = document.createElement("input"); input.value = item.tail_hex;
    input.onchange = () => { item.tail_hex = input.value.toUpperCase(); markWidgetDirty(); };
    row.append(input); target.append(row);
  }
}

function editFloatValue(field, text) {
  const value = Number(text);
  if (!Number.isFinite(value)) { setStatus("Enter a finite float value"); renderItemFields(); return; }
  const data = new DataView(new ArrayBuffer(4)); data.setFloat32(0, value, true);
  field.bits = hex32(data.getUint32(0, true));
  field.value = String(data.getFloat32(0, true));
  markWidgetDirty(); renderItemFields();
}

function editFloatBits(field, text) {
  const bits = parseHex32(text);
  if (bits === null) { setStatus("Enter 32-bit IEEE-754 bits"); renderItemFields(); return; }
  const data = new DataView(new ArrayBuffer(4)); data.setUint32(0, bits, true);
  const value = data.getFloat32(0, true);
  field.bits = hex32(bits);
  field.value = Number.isFinite(value) ? String(value) : `bits:${hex32(bits)}`;
  markWidgetDirty(); renderItemFields();
}

function markWidgetDirty() { state.widget.dirty = true; updateDirty(); setStatus("Widget value updated"); }

function renderRawDetails() {
  const target = $("raw-details");
  if (!state.view || !state.screen) { target.className = "empty"; target.textContent = "Select a view"; return; }
  target.className = "";
  target.innerHTML = `<table class="raw-table">
    <tr><td>Screen tag</td><td>${escapeHtml(state.screen.data.screen.tag)}</td></tr>
    <tr><td>View hash</td><td>${escapeHtml(state.view.name_hash)}</td></tr>
    <tr><td>Hierarchy tag</td><td>${escapeHtml(state.view.hierarchy_tag)}</td></tr>
    <tr><td>Hierarchy entry</td><td>${state.view.hierarchy_entry}</td></tr>
    <tr><td>Widget table</td><td>${escapeHtml(state.view.widget_table_tag)}</td></tr>
    <tr><td>Nodes</td><td>${state.view.node_count}</td></tr>
    <tr><td>Edges</td><td>${state.view.edges.length}</td></tr>
  </table>`;
}

async function saveAll() {
  if (!state.writable) return;
  try {
    for (const screen of state.screens) {
      if (screen.dirty) { await writeJsonHandle(screen.handle, screen.data); screen.dirty = false; }
    }
    for (const widget of state.widgetCache.values()) {
      if (widget.dirty) { await writeJsonHandle(widget.handle, widget.data); widget.dirty = false; }
    }
    updateDirty(); setStatus("Changes saved");
  } catch (error) { setStatus(`Save failed: ${error.message}`); }
}

async function addWidgetDirectory(directory) {
  try {
    const widgets = await directoryAt(directory, ["assets", "ui", "widget_tables"]);
    for await (const [name, handle] of widgets.entries()) {
      if (handle.kind === "file" && name.toLowerCase().endsWith(".json")) {
        state.widgetHandles.set(name.slice(0, -5).toUpperCase(), handle);
      }
    }
  } catch (_) {
    // Not every depkg project contains widget tables.
  }
}

function fitGraph() {
  const box = $("graph-world").getBBox();
  const viewport = $("graph-viewport").getBoundingClientRect();
  if (!box.width || !box.height) return;
  const scale = Math.min(1.5, Math.max(.05, Math.min((viewport.width - 80) / box.width, (viewport.height - 80) / box.height)));
  state.transform = { scale, x: 40 - box.x * scale, y: 40 - box.y * scale };
  applyGraphTransform();
}

function applyGraphTransform() {
  const { x, y, scale } = state.transform;
  $("graph-world").setAttribute("transform", `translate(${x} ${y}) scale(${scale})`);
}

function setupGraphNavigation() {
  const svg = $("graph");
  let drag = null;
  svg.addEventListener("pointerdown", event => { drag = { x: event.clientX, y: event.clientY, ox: state.transform.x, oy: state.transform.y }; svg.setPointerCapture(event.pointerId); });
  svg.addEventListener("pointermove", event => { if (!drag) return; state.transform.x = drag.ox + event.clientX - drag.x; state.transform.y = drag.oy + event.clientY - drag.y; applyGraphTransform(); });
  svg.addEventListener("pointerup", () => { drag = null; });
  svg.addEventListener("wheel", event => {
    event.preventDefault();
    const rect = svg.getBoundingClientRect();
    const px = event.clientX - rect.left, py = event.clientY - rect.top;
    const beforeX = (px - state.transform.x) / state.transform.scale;
    const beforeY = (py - state.transform.y) / state.transform.scale;
    state.transform.scale = Math.min(3, Math.max(.03, state.transform.scale * (event.deltaY < 0 ? 1.12 : .89)));
    state.transform.x = px - beforeX * state.transform.scale;
    state.transform.y = py - beforeY * state.transform.scale;
    applyGraphTransform();
  }, { passive: false });
}

function parseHex32(text) {
  const value = Number(text.trim());
  return Number.isInteger(value) && value >= 0 && value <= 0xFFFFFFFF ? value >>> 0 : null;
}
function hex32(value) { return `0x${(value >>> 0).toString(16).toUpperCase().padStart(8, "0")}`; }
function escapeHtml(text) { const node = document.createElement("span"); node.textContent = String(text); return node.innerHTML; }

$("open-project").onclick = async () => {
  if (hasDirty() && !window.confirm("Discard unsaved UI changes and open another project?")) return;
  if (window.showDirectoryPicker) {
    try { await loadDirectory(await window.showDirectoryPicker({ mode: "readwrite" })); }
    catch (error) { if (error.name !== "AbortError") setStatus(`Open failed: ${error.message}`); }
  } else $("folder-fallback").click();
};
$("folder-fallback").onchange = event => loadFallback([...event.target.files]);
$("add-widget-project").onclick = async () => {
  try {
    const directory = await window.showDirectoryPicker({ mode: "readwrite" });
    await addWidgetDirectory(directory);
    if (state.view) await loadWidgetTable(state.view.widget_table_tag);
    setStatus(`Added widget project ${directory.name}`);
  } catch (error) { if (error.name !== "AbortError") setStatus(`Open failed: ${error.message}`); }
};
$("reload-project").onclick = () => {
  if (!state.directory) return;
  if (hasDirty() && !window.confirm("Discard unsaved UI changes and reload the project?")) return;
  loadDirectory(state.directory);
};
$("save-all").onclick = saveAll;
$("screen-select").onchange = event => selectScreen(Number(event.target.value));
$("view-filter").oninput = renderViewList;
$("node-search").oninput = renderGraph;
$("fit-graph").onclick = fitGraph;
$("array-select").onchange = event => { state.selectedArray = Number(event.target.value); state.selectedItem = 0; renderArrayEditor(); };
$("item-index").onchange = event => { state.selectedItem = Math.max(0, Number(event.target.value) || 0); renderArrayEditor(); };
for (const tab of document.querySelectorAll(".tab")) tab.onclick = () => {
  document.querySelectorAll(".tab").forEach(item => item.classList.toggle("active", item === tab));
  document.querySelectorAll(".tab-content").forEach(content => content.classList.toggle("active", content.id === `tab-${tab.dataset.tab}`));
};
window.addEventListener("beforeunload", event => {
  if (hasDirty()) {
    event.preventDefault(); event.returnValue = "";
  }
});
setupGraphNavigation();
