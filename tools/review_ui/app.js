const state = {
  captureInProgress: false,
  annotations: [],
  screenshotBytes: null,
  screenshotBitmap: null,
  screenshotSource: "none",
  screenshotMimeType: "",
  importedFilename: "",
  lastSessionDir: "",
  tool: "select",
  drawing: null,
  color: "#ff5a36",
  strokeWidth: 4,
  dragIndex: -1,
  dragOffset: { x: 0, y: 0 },
};

const el = {
  importPhotoBtn: document.getElementById("importPhotoBtn"),
  photoInput: document.getElementById("photoInput"),
  downloadSourceBtn: document.getElementById("downloadSourceBtn"),
  downloadPngBtn: document.getElementById("downloadPngBtn"),
  undoBtn: document.getElementById("undoBtn"),
  clearBtn: document.getElementById("clearBtn"),
  copyForChatBtn: document.getElementById("copyForChatBtn"),
  copyNotesBtn: document.getElementById("copyNotesBtn"),
  copyJsonBtn: document.getElementById("copyJsonBtn"),
  saveSessionBtn: document.getElementById("saveSessionBtn"),
  openSessionsBtn: document.getElementById("openSessionsBtn"),
  colorInput: document.getElementById("colorInput"),
  sizeInput: document.getElementById("sizeInput"),
  noteInput: document.getElementById("noteInput"),
  captureStatus: document.getElementById("captureStatus"),
  sessionStatus: document.getElementById("sessionStatus"),
  canvasMeta: document.getElementById("canvasMeta"),
  logOutput: document.getElementById("logOutput"),
  canvas: document.getElementById("reviewCanvas"),
  toolButtons: {
    select: document.getElementById("toolSelectBtn"),
    pen: document.getElementById("toolPenBtn"),
    rect: document.getElementById("toolRectBtn"),
    arrow: document.getElementById("toolArrowBtn"),
  },
};

const ctx = el.canvas.getContext("2d");

function log(message) {
  const ts = new Date().toLocaleTimeString();
  el.logOutput.textContent = `[${ts}] ${message}\n` + el.logOutput.textContent;
}

function setCaptureStatus(message) {
  el.captureStatus.textContent = message;
}

function setSessionStatus(message) {
  el.sessionStatus.textContent = message;
}

function setCaptureBusy(isBusy) {
  state.captureInProgress = isBusy;
  el.importPhotoBtn.disabled = isBusy;
  el.downloadSourceBtn.disabled = isBusy;
  el.saveSessionBtn.disabled = isBusy;
}

async function apiJson(url, options = {}) {
  const response = await fetch(url, {
    ...options,
    headers: {
      "Content-Type": "application/json",
      ...(options.headers || {}),
    },
  });

  if (!response.ok) {
    const text = await response.text();
    throw new Error(text || `Request failed: ${response.status}`);
  }
  return response.json();
}

function setTool(tool) {
  state.tool = tool;
  for (const [name, btn] of Object.entries(el.toolButtons)) {
    btn.classList.toggle("active", name === tool);
  }
  el.canvas.style.cursor = tool === "select" ? "grab" : "crosshair";
}

function resizeCanvasToBitmap(bitmap) {
  el.canvas.width = bitmap.width;
  el.canvas.height = bitmap.height;
  el.canvasMeta.textContent = `Canvas: ${bitmap.width} x ${bitmap.height} (${state.screenshotSource})`;
}

function drawArrow(x1, y1, x2, y2, color, width) {
  const head = Math.max(10, width * 3.5);
  const angle = Math.atan2(y2 - y1, x2 - x1);
  ctx.strokeStyle = color;
  ctx.fillStyle = color;
  ctx.lineWidth = width;
  ctx.lineCap = "round";
  ctx.lineJoin = "round";
  ctx.beginPath();
  ctx.moveTo(x1, y1);
  ctx.lineTo(x2, y2);
  ctx.stroke();

  ctx.beginPath();
  ctx.moveTo(x2, y2);
  ctx.lineTo(x2 - head * Math.cos(angle - Math.PI / 6), y2 - head * Math.sin(angle - Math.PI / 6));
  ctx.lineTo(x2 - head * Math.cos(angle + Math.PI / 6), y2 - head * Math.sin(angle + Math.PI / 6));
  ctx.closePath();
  ctx.fill();
}

function drawAnnotation(annotation) {
  ctx.save();
  ctx.strokeStyle = annotation.color;
  ctx.fillStyle = annotation.color;
  ctx.lineWidth = annotation.strokeWidth;
  ctx.lineCap = "round";
  ctx.lineJoin = "round";

  if (annotation.type === "pen" && annotation.points.length) {
    ctx.beginPath();
    ctx.moveTo(annotation.points[0].x, annotation.points[0].y);
    for (const pt of annotation.points.slice(1)) {
      ctx.lineTo(pt.x, pt.y);
    }
    ctx.stroke();
  }

  if (annotation.type === "rect") {
    ctx.strokeRect(annotation.x, annotation.y, annotation.w, annotation.h);
  }

  if (annotation.type === "arrow") {
    drawArrow(annotation.x1, annotation.y1, annotation.x2, annotation.y2, annotation.color, annotation.strokeWidth);
  }

  ctx.restore();
}

function redraw() {
  ctx.clearRect(0, 0, el.canvas.width, el.canvas.height);
  if (state.screenshotBitmap) {
    ctx.drawImage(state.screenshotBitmap, 0, 0);
  } else {
    ctx.fillStyle = "#0a1320";
    ctx.fillRect(0, 0, el.canvas.width, el.canvas.height);
    ctx.fillStyle = "#aabbd0";
    ctx.font = "20px Segoe UI";
    ctx.fillText("No screenshot loaded yet", 24, 36);
  }

  for (const ann of state.annotations) {
    drawAnnotation(ann);
  }

  if (state.drawing) {
    ctx.save();
    ctx.globalAlpha = 0.75;
    drawAnnotation(state.drawing);
    ctx.restore();
  }
}

function canvasPoint(evt) {
  const rect = el.canvas.getBoundingClientRect();
  const scaleX = el.canvas.width / rect.width;
  const scaleY = el.canvas.height / rect.height;
  return {
    x: (evt.clientX - rect.left) * scaleX,
    y: (evt.clientY - rect.top) * scaleY,
  };
}

function hitAnnotation(point) {
  for (let i = state.annotations.length - 1; i >= 0; i--) {
    const ann = state.annotations[i];
    if (ann.type === "rect") {
      const minX = Math.min(ann.x, ann.x + ann.w) - 10;
      const maxX = Math.max(ann.x, ann.x + ann.w) + 10;
      const minY = Math.min(ann.y, ann.y + ann.h) - 10;
      const maxY = Math.max(ann.y, ann.y + ann.h) + 10;
      if (point.x >= minX && point.x <= maxX && point.y >= minY && point.y <= maxY) return i;
    }
    if (ann.type === "arrow") {
      const minX = Math.min(ann.x1, ann.x2) - 12;
      const maxX = Math.max(ann.x1, ann.x2) + 12;
      const minY = Math.min(ann.y1, ann.y2) - 12;
      const maxY = Math.max(ann.y1, ann.y2) + 12;
      if (point.x >= minX && point.x <= maxX && point.y >= minY && point.y <= maxY) return i;
    }
    if (ann.type === "pen") {
      for (const pt of ann.points) {
        if (Math.hypot(pt.x - point.x, pt.y - point.y) <= 12) return i;
      }
    }
  }
  return -1;
}

function normalizeRect(rect) {
  if (rect.w < 0) {
    rect.x += rect.w;
    rect.w *= -1;
  }
  if (rect.h < 0) {
    rect.y += rect.h;
    rect.h *= -1;
  }
  return rect;
}

function inferMimeType(file) {
  if (file.type) return file.type;
  const lower = file.name.toLowerCase();
  if (lower.endsWith(".bmp")) return "image/bmp";
  if (lower.endsWith(".png")) return "image/png";
  if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) return "image/jpeg";
  if (lower.endsWith(".webp")) return "image/webp";
  if (lower.endsWith(".gif")) return "image/gif";
  return "application/octet-stream";
}

function suggestedFilename() {
  if (!state.importedFilename) return "crowpanel-source.png";
  return state.importedFilename;
}

async function setScreenshotFromBytes(bytes, source, importedFilename = "", mimeType = "image/png") {
  state.screenshotBytes = bytes;
  state.screenshotSource = source;
  state.screenshotMimeType = mimeType;
  state.importedFilename = importedFilename;
  const blob = new Blob([bytes], { type: mimeType });
  const bitmap = await createImageBitmap(blob);
  state.screenshotBitmap = bitmap;
  resizeCanvasToBitmap(bitmap);
  redraw();
}

async function importPhoto(file) {
  setCaptureBusy(true);
  try {
    const bytes = new Uint8Array(await file.arrayBuffer());
    const mimeType = inferMimeType(file);
    await setScreenshotFromBytes(bytes, "imported-file", file.name, mimeType);
    setCaptureStatus(`Imported screenshot: ${file.name}`);
    log(`Imported screenshot ${file.name}.`);
  } finally {
    setCaptureBusy(false);
  }
}

function markupObject() {
  const screenshot = state.screenshotBitmap
    ? {
        width: state.screenshotBitmap.width,
        height: state.screenshotBitmap.height,
        source: state.screenshotSource,
        imported_filename: state.importedFilename || null,
        mime_type: state.screenshotMimeType || null,
      }
    : null;
  return {
    type: "crowpanel-review",
    screenshot,
    note: el.noteInput.value.trim(),
    annotations: state.annotations.map((ann) => structuredClone(ann)),
    exported_at: new Date().toISOString(),
  };
}

function notesText() {
  const review = markupObject();
  return [
    "CrowPanel review",
    `Source: ${review.screenshot?.source || "none"}`,
    `File: ${review.screenshot?.imported_filename || "n/a"}`,
    "",
    "Notes:",
    review.note || "(no notes)",
    "",
    "Annotations JSON:",
    JSON.stringify(review.annotations, null, 2),
  ].join("\n");
}

async function copyText(value) {
  await navigator.clipboard.writeText(value);
}

async function copyMarkup(pretty = true) {
  const data = markupObject();
  const json = JSON.stringify(data, null, 2);
  await copyText(pretty ? `\`\`\`json\n${json}\n\`\`\`` : json);
  log(pretty ? "Copied fenced review markup." : "Copied raw JSON review markup.");
}

async function copyForChat() {
  const data = markupObject();
  const json = JSON.stringify(data, null, 2);
  const text = [
    "CrowPanel review payload:",
    "",
    "```json",
    json,
    "```",
  ].join("\n");
  await copyText(text);
  log("Copied chat-ready review payload.");
}

async function copyNotes() {
  await copyText(notesText());
  log("Copied notes summary.");
}

function downloadBlob(filename, blob) {
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}

function downloadSource() {
  if (!state.screenshotBytes) {
    setCaptureStatus("No imported screenshot available yet.");
    return;
  }
  downloadBlob(
    suggestedFilename(),
    new Blob([state.screenshotBytes], { type: state.screenshotMimeType || "application/octet-stream" }),
  );
}

function downloadAnnotatedPng() {
  redraw();
  el.canvas.toBlob((blob) => {
    if (!blob) return;
    downloadBlob("crowpanel-annotated.png", blob);
  }, "image/png");
}

function bytesToBase64(bytes) {
  let binary = "";
  const chunkSize = 0x8000;
  for (let i = 0; i < bytes.length; i += chunkSize) {
    const slice = bytes.subarray(i, i + chunkSize);
    binary += String.fromCharCode(...slice);
  }
  return btoa(binary);
}

function canvasToPngBase64() {
  return el.canvas.toDataURL("image/png").split(",")[1];
}

async function saveSession() {
  if (!state.screenshotBitmap) {
    setSessionStatus("Import an image first.");
    return;
  }

  const review = markupObject();
  const payload = {
    source_image_base64: state.screenshotBytes ? bytesToBase64(state.screenshotBytes) : "",
    source_image_filename: state.importedFilename || "source-image",
    annotated_png_base64: canvasToPngBase64(),
    review,
  };

  const result = await apiJson("/api/save-session", {
    method: "POST",
    body: JSON.stringify(payload),
  });
  state.lastSessionDir = result.session_dir;
  setSessionStatus(`Saved session: ${result.session_dir}`);
  log(`Saved review session to ${result.session_dir}`);
}

async function openSessionsFolder() {
  const result = await apiJson("/api/open-sessions-folder", { method: "POST", body: "{}" });
  setSessionStatus(`Opened sessions folder: ${result.path}`);
  log(`Opened sessions folder: ${result.path}`);
}

el.importPhotoBtn.addEventListener("click", () => el.photoInput.click());
el.photoInput.addEventListener("change", async (evt) => {
  const file = evt.target.files?.[0];
  if (!file) return;
  try {
    await importPhoto(file);
  } catch (err) {
    setCaptureStatus(err.message);
    log(`Import failed: ${err.message}`);
  } finally {
    el.photoInput.value = "";
  }
});

el.downloadSourceBtn.addEventListener("click", downloadSource);
el.downloadPngBtn.addEventListener("click", downloadAnnotatedPng);
el.saveSessionBtn.addEventListener("click", async () => {
  try {
    await saveSession();
  } catch (err) {
    setSessionStatus(err.message);
    log(`Save session failed: ${err.message}`);
  }
});
el.openSessionsBtn.addEventListener("click", async () => {
  try {
    await openSessionsFolder();
  } catch (err) {
    setSessionStatus(err.message);
    log(`Open folder failed: ${err.message}`);
  }
});

el.undoBtn.addEventListener("click", () => {
  state.annotations.pop();
  redraw();
});

el.clearBtn.addEventListener("click", () => {
  state.annotations = [];
  redraw();
});

el.copyForChatBtn.addEventListener("click", async () => {
  try {
    await copyForChat();
  } catch (err) {
    log(`Copy for chat failed: ${err.message}`);
  }
});

el.copyNotesBtn.addEventListener("click", async () => {
  try {
    await copyNotes();
  } catch (err) {
    log(`Copy notes failed: ${err.message}`);
  }
});

el.copyJsonBtn.addEventListener("click", async () => {
  try {
    await copyMarkup(false);
  } catch (err) {
    log(`Copy JSON failed: ${err.message}`);
  }
});

el.colorInput.addEventListener("input", () => {
  state.color = el.colorInput.value;
});

el.sizeInput.addEventListener("input", () => {
  state.strokeWidth = Number(el.sizeInput.value);
});

for (const [tool, button] of Object.entries(el.toolButtons)) {
  button.addEventListener("click", () => setTool(tool));
}

el.canvas.addEventListener("pointerdown", (evt) => {
  const pt = canvasPoint(evt);
  el.canvas.setPointerCapture(evt.pointerId);

  if (state.tool === "select") {
    const hit = hitAnnotation(pt);
    if (hit >= 0) {
      state.dragIndex = hit;
      const ann = state.annotations[hit];
      if (ann.type === "rect") {
        state.dragOffset = { x: pt.x - ann.x, y: pt.y - ann.y };
      } else if (ann.type === "arrow") {
        state.dragOffset = { x: pt.x - ann.x1, y: pt.y - ann.y1 };
      } else if (ann.type === "pen" && ann.points.length) {
        state.dragOffset = { x: pt.x - ann.points[0].x, y: pt.y - ann.points[0].y };
      }
    }
    return;
  }

  if (state.tool === "pen") {
    state.drawing = {
      type: "pen",
      color: state.color,
      strokeWidth: state.strokeWidth,
      points: [pt],
    };
  }

  if (state.tool === "rect") {
    state.drawing = {
      type: "rect",
      color: state.color,
      strokeWidth: state.strokeWidth,
      x: pt.x,
      y: pt.y,
      w: 0,
      h: 0,
    };
  }

  if (state.tool === "arrow") {
    state.drawing = {
      type: "arrow",
      color: state.color,
      strokeWidth: state.strokeWidth,
      x1: pt.x,
      y1: pt.y,
      x2: pt.x,
      y2: pt.y,
    };
  }

  redraw();
});

el.canvas.addEventListener("pointermove", (evt) => {
  const pt = canvasPoint(evt);

  if (state.dragIndex >= 0 && state.tool === "select") {
    const ann = state.annotations[state.dragIndex];
    if (ann.type === "rect") {
      ann.x = pt.x - state.dragOffset.x;
      ann.y = pt.y - state.dragOffset.y;
    } else if (ann.type === "arrow") {
      const dx = ann.x2 - ann.x1;
      const dy = ann.y2 - ann.y1;
      ann.x1 = pt.x - state.dragOffset.x;
      ann.y1 = pt.y - state.dragOffset.y;
      ann.x2 = ann.x1 + dx;
      ann.y2 = ann.y1 + dy;
    } else if (ann.type === "pen" && ann.points.length) {
      const dx = pt.x - state.dragOffset.x - ann.points[0].x;
      const dy = pt.y - state.dragOffset.y - ann.points[0].y;
      ann.points = ann.points.map((p) => ({ x: p.x + dx, y: p.y + dy }));
    }
    redraw();
    return;
  }

  if (!state.drawing) return;

  if (state.drawing.type === "pen") {
    state.drawing.points.push(pt);
  }

  if (state.drawing.type === "rect") {
    state.drawing.w = pt.x - state.drawing.x;
    state.drawing.h = pt.y - state.drawing.y;
  }

  if (state.drawing.type === "arrow") {
    state.drawing.x2 = pt.x;
    state.drawing.y2 = pt.y;
  }

  redraw();
});

el.canvas.addEventListener("pointerup", () => {
  if (state.dragIndex >= 0) {
    state.dragIndex = -1;
    return;
  }

  if (!state.drawing) return;

  let final = structuredClone(state.drawing);
  if (final.type === "rect") {
    final = normalizeRect(final);
    if (final.w < 2 || final.h < 2) {
      state.drawing = null;
      redraw();
      return;
    }
  }
  if (final.type === "arrow" && Math.hypot(final.x2 - final.x1, final.y2 - final.y1) < 4) {
    state.drawing = null;
    redraw();
    return;
  }
  if (final.type === "pen" && final.points.length < 2) {
    state.drawing = null;
    redraw();
    return;
  }

  state.annotations.push(final);
  state.drawing = null;
  redraw();
});

el.canvas.addEventListener("pointerleave", () => {
  if (state.dragIndex >= 0) {
    state.dragIndex = -1;
  }
});

redraw();
log("Review UI ready.");
setCaptureStatus("Import a screenshot to begin.");
