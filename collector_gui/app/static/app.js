const maxSamples = 500;
let samples = [];
let totalSamples = 0;
let latestEvent = null;
let mqttSnapshot = null;
let deviceStatus = null;
let socket = null;

const fields = {
  acc: [
    ["acc_x_mg", "Acc X", "#087f8c"],
    ["acc_y_mg", "Acc Y", "#3454d1"],
    ["acc_z_mg", "Acc Z", "#b86b14"],
  ],
  gyr: [
    ["gyr_x_dps", "Gyr X", "#7b3fb2"],
    ["gyr_y_dps", "Gyr Y", "#287a3e"],
    ["gyr_z_dps", "Gyr Z", "#b3261e"],
  ],
};

const els = {
  brokerLine: document.querySelector("#brokerLine"),
  mqttBadge: document.querySelector("#mqttBadge"),
  deviceBadge: document.querySelector("#deviceBadge"),
  sampleCount: document.querySelector("#sampleCount"),
  lastSample: document.querySelector("#lastSample"),
  uptime: document.querySelector("#uptime"),
  chartRange: document.querySelector("#chartRange"),
  chart: document.querySelector("#signalChart"),
  legend: document.querySelector("#legend"),
  accValues: document.querySelector("#accValues"),
  gyrValues: document.querySelector("#gyrValues"),
  collectorEvent: document.querySelector("#collectorEvent"),
  sampleRows: document.querySelector("#sampleRows"),
  refreshButton: document.querySelector("#refreshButton"),
  fieldControls: document.querySelector(".field-controls"),
};

function formatNumber(value, digits = 2) {
  return Number.isFinite(value) ? value.toFixed(digits) : "--";
}

function formatTime(value) {
  if (!value) return "--";
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return value;
  return date.toLocaleString();
}

function formatUptime(ms) {
  if (!Number.isFinite(ms)) return "--";
  const totalSeconds = Math.floor(ms / 1000);
  const minutes = Math.floor(totalSeconds / 60);
  const seconds = totalSeconds % 60;
  return `${minutes}m ${seconds}s`;
}

function vectorText(sample, prefix, unit) {
  if (!sample) return "--";
  return [
    `x ${formatNumber(sample[`${prefix}_x_${unit}`])}`,
    `y ${formatNumber(sample[`${prefix}_y_${unit}`])}`,
    `z ${formatNumber(sample[`${prefix}_z_${unit}`])}`,
  ].join("  ");
}

function updateBadge(el, text, state) {
  el.textContent = text;
  el.classList.remove("badge-ok", "badge-warn", "badge-bad");
  if (state) el.classList.add(state);
}

function selectedSeries() {
  const selected = [...els.fieldControls.querySelectorAll("input:checked")].map(
    (input) => input.value,
  );
  return selected.flatMap((key) => fields[key] || []);
}

function normalizeSample(sample) {
  const normalized = { ...sample };
  for (const key of Object.keys(normalized)) {
    if (
      key.endsWith("_mg") ||
      key.endsWith("_dps")
    ) {
      normalized[key] =
        normalized[key] === null || normalized[key] === undefined
          ? null
          : Number(normalized[key]);
    }
  }
  normalized.uptime_ms =
    normalized.uptime_ms === null || normalized.uptime_ms === undefined
      ? null
      : Number(normalized.uptime_ms);
  return normalized;
}

function setSummary(summary) {
  mqttSnapshot = summary.mqtt || mqttSnapshot;
  deviceStatus = summary.latest_status || deviceStatus;
  latestEvent = summary.latest_event || latestEvent;
  totalSamples = Number(summary.total_samples || totalSamples || 0);
  if (summary.latest_sample) {
    const latest = normalizeSample(summary.latest_sample);
    const exists = samples.some((sample) => sample.id === latest.id);
    if (!exists) {
      samples.push(latest);
      samples = samples.slice(-maxSamples);
    }
  }
  render();
}

async function loadSamples() {
  const [samplesResponse, summaryResponse] = await Promise.all([
    fetch("/api/samples?limit=500"),
    fetch("/api/summary"),
  ]);
  const samplePayload = await samplesResponse.json();
  const summaryPayload = await summaryResponse.json();
  samples = samplePayload.samples.map(normalizeSample);
  totalSamples = summaryPayload.total_samples || samples.length;
  setSummary(summaryPayload);
}

function connectWebSocket() {
  const protocol = window.location.protocol === "https:" ? "wss" : "ws";
  socket = new WebSocket(`${protocol}://${window.location.host}/ws/live`);

  socket.addEventListener("message", (event) => {
    const message = JSON.parse(event.data);
    if (message.type === "hello") {
      setSummary(message.summary);
      return;
    }
    if (message.type === "imu_sample") {
      samples.push(normalizeSample(message.sample));
      samples = samples.slice(-maxSamples);
      totalSamples += 1;
      render();
      return;
    }
    if (message.type === "device_status") {
      deviceStatus = message.status;
      render();
      return;
    }
    if (message.type === "collector_event") {
      latestEvent = message.event;
      mqttSnapshot = message.mqtt || mqttSnapshot;
      render();
    }
  });

  socket.addEventListener("close", () => {
    updateBadge(els.mqttBadge, "MQTT reconnecting", "badge-warn");
    window.setTimeout(connectWebSocket, 1500);
  });
}

function renderMetrics() {
  const latest = samples[samples.length - 1];
  els.sampleCount.textContent = String(totalSamples || samples.length);
  els.lastSample.textContent = latest ? formatTime(latest.received_at) : "--";
  els.uptime.textContent = latest ? formatUptime(latest.uptime_ms) : "--";

  if (latest) {
    els.accValues.textContent = vectorText(latest, "acc", "mg");
    els.gyrValues.textContent = vectorText(latest, "gyr", "dps");
  }

  els.collectorEvent.textContent = latestEvent
    ? `${latestEvent.level}: ${latestEvent.message}`
    : "--";

  if (mqttSnapshot) {
    els.brokerLine.textContent = `MQTT broker: ${mqttSnapshot.host}:${mqttSnapshot.port}  topic: ${mqttSnapshot.topic}`;
    updateBadge(
      els.mqttBadge,
      mqttSnapshot.connected ? "MQTT connected" : "MQTT offline",
      mqttSnapshot.connected ? "badge-ok" : "badge-warn",
    );
  }

  if (deviceStatus) {
    const isOnline = String(deviceStatus.status).toLowerCase() === "online";
    updateBadge(
      els.deviceBadge,
      `Device ${deviceStatus.status}`,
      isOnline ? "badge-ok" : "badge-bad",
    );
  }
}

function renderTable() {
  const rows = samples.slice(-120).reverse();
  if (rows.length === 0) {
    els.sampleRows.innerHTML = '<tr><td colspan="4">No samples yet.</td></tr>';
    return;
  }

  els.sampleRows.innerHTML = rows
    .map(
      (sample) => `
        <tr>
          <td>${formatTime(sample.received_at)}</td>
          <td>${formatUptime(sample.uptime_ms)}</td>
          <td>${formatNumber(sample.acc_x_mg)}, ${formatNumber(sample.acc_y_mg)}, ${formatNumber(sample.acc_z_mg)}</td>
          <td>${formatNumber(sample.gyr_x_dps)}, ${formatNumber(sample.gyr_y_dps)}, ${formatNumber(sample.gyr_z_dps)}</td>
        </tr>
      `,
    )
    .join("");
}

function canvasSize(canvas) {
  const rect = canvas.getBoundingClientRect();
  const ratio = window.devicePixelRatio || 1;
  const width = Math.max(320, Math.floor(rect.width * ratio));
  const height = Math.max(260, Math.floor(rect.height * ratio));
  if (canvas.width !== width || canvas.height !== height) {
    canvas.width = width;
    canvas.height = height;
  }
  return { width, height, ratio };
}

function drawChart() {
  const canvas = els.chart;
  const ctx = canvas.getContext("2d");
  const { width, height, ratio } = canvasSize(canvas);
  const pad = {
    left: 58 * ratio,
    right: 18 * ratio,
    top: 24 * ratio,
    bottom: 36 * ratio,
  };
  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = "#fbfcfb";
  ctx.fillRect(0, 0, width, height);

  const series = selectedSeries();
  els.legend.innerHTML = series
    .map(([, label, color]) => `<span><i style="background:${color}"></i>${label}</span>`)
    .join("");

  if (samples.length < 2 || series.length === 0) {
    ctx.fillStyle = "#657077";
    ctx.font = `${14 * ratio}px sans-serif`;
    ctx.fillText("Waiting for live samples", pad.left, height / 2);
    els.chartRange.textContent = "Waiting for samples";
    return;
  }

  const chartSamples = samples.slice(-maxSamples);
  const values = [];
  for (const sample of chartSamples) {
    for (const [key] of series) {
      const value = sample[key];
      if (Number.isFinite(value)) values.push(value);
    }
  }
  if (values.length === 0) return;

  let min = Math.min(...values);
  let max = Math.max(...values);
  if (min === max) {
    min -= 1;
    max += 1;
  }
  const padding = (max - min) * 0.08;
  min -= padding;
  max += padding;

  const x0 = pad.left;
  const x1 = width - pad.right;
  const y0 = height - pad.bottom;
  const y1 = pad.top;

  ctx.strokeStyle = "#d9dfdd";
  ctx.lineWidth = 1 * ratio;
  ctx.fillStyle = "#657077";
  ctx.font = `${11 * ratio}px sans-serif`;
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";

  for (let i = 0; i <= 4; i += 1) {
    const y = y0 - ((y0 - y1) * i) / 4;
    const value = min + ((max - min) * i) / 4;
    ctx.beginPath();
    ctx.moveTo(x0, y);
    ctx.lineTo(x1, y);
    ctx.stroke();
    ctx.fillText(formatNumber(value, 1), x0 - 8 * ratio, y);
  }

  const xFor = (index) =>
    x0 + ((x1 - x0) * index) / Math.max(1, chartSamples.length - 1);
  const yFor = (value) => y0 - ((value - min) / (max - min)) * (y0 - y1);

  for (const [key, , color] of series) {
    ctx.beginPath();
    ctx.strokeStyle = color;
    ctx.lineWidth = 2 * ratio;
    let started = false;
    chartSamples.forEach((sample, index) => {
      const value = sample[key];
      if (!Number.isFinite(value)) return;
      const x = xFor(index);
      const y = yFor(value);
      if (!started) {
        ctx.moveTo(x, y);
        started = true;
      } else {
        ctx.lineTo(x, y);
      }
    });
    ctx.stroke();
  }

  const first = chartSamples[0];
  const last = chartSamples[chartSamples.length - 1];
  els.chartRange.textContent = `${formatTime(first.received_at)} - ${formatTime(
    last.received_at,
  )}`;
}

function render() {
  renderMetrics();
  renderTable();
  drawChart();
}

els.refreshButton.addEventListener("click", () => {
  loadSamples().catch((error) => {
    latestEvent = { level: "error", message: error.message };
    render();
  });
});

els.fieldControls.addEventListener("change", drawChart);
window.addEventListener("resize", drawChart);

loadSamples()
  .then(connectWebSocket)
  .catch((error) => {
    latestEvent = { level: "error", message: error.message };
    render();
    connectWebSocket();
  });
