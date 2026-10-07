/* ==========================================================================
   AquaGuard — Next-Gen Smart Water Tank Controller (Client Logic)
   - Real-time MQTT Pub/Sub
   - 5–8s Statistical Rolling Window Filter (Outlier Rejection / Anti-Spike)
   - Fluid Smooth Filling & Draining Interpolation (60 FPS)
   - Client-side Water Volume (Liters) Calculation
   - Smart Damping Toggle (Smoothed vs Raw Live Feed)
   ========================================================================== */

// --- TANK GEOMETRY STATE (Defaults) ---
let tankRadius = 50.0;     // cm
let tankHeight = 100.0;    // cm
let sensorOffset = 0.0;    // cm
let motorOnPercent = 25.0; // %
let motorOffPercent = 75.0;// %
let settingsChanged = false;

// --- DAMPING & STATISTICAL BUFFER STATE ---
const WINDOW_MS = 5000;          // 5-second rolling analysis window
const EVAL_INTERVAL_MS = 5000;   // Evaluate and commit target every 5 seconds
let sampleHistory = [];          // [{ timestamp, percent, dist, level }]
let lastEvalTime = 0;            // Timestamp of last 5-second evaluation
let isDampingEnabled = true;

// Load damping preference
try {
  const savedDamping = localStorage.getItem('aquaguard_damping');
  if (savedDamping !== null) {
    isDampingEnabled = savedDamping === 'true';
  }
} catch (e) {}

// --- ANIMATION & WATER STATE ---
let targetWaterPercent = 0.0;
let currentWaterPercent = 0.0;
let isFirstPacket = true;
let previousWaterPercent = 0.0;
let lastTrendCheckTime = Date.now();

// --- WATER USAGE STATE (Discrete Motor Cycle Calculation) ---
let totalAccumulatedUsedLiters = 0.0;
let fullBaselinePercent = null;
let previousMotorRunning = null;

try {
  const savedUsed = localStorage.getItem('aquaguard_accumulated_used');
  if (savedUsed !== null) {
    totalAccumulatedUsedLiters = parseFloat(savedUsed) || 0.0;
  }
  const savedBaseline = localStorage.getItem('aquaguard_full_baseline');
  if (savedBaseline !== null) {
    fullBaselinePercent = parseFloat(savedBaseline);
  }
} catch (e) {}

// --- DEFAULT MQTT CONFIGURATION ---
const DEFAULT_CONFIG = {
  host: window.location.hostname || 'www.rahat.eu.cc',
  port: window.location.protocol === 'https:' ? 443 : 9001,
  path: '/mqtt',
  user: 'rahat300809',
  pass: 'RAHAT678',
  telemetryTopic: 'devices/ESP32_WATER_01/telemetry',
  commandTopic: 'devices/ESP32_WATER_01/command'
};

// If testing locally on host, set fallback
if (!DEFAULT_CONFIG.host || DEFAULT_CONFIG.host === 'localhost' || DEFAULT_CONFIG.host === '127.0.0.1') {
  DEFAULT_CONFIG.host = '192.168.0.104';
  DEFAULT_CONFIG.port = 9001;
  DEFAULT_CONFIG.path = '';
}

let config = { ...DEFAULT_CONFIG };
try {
  const saved = localStorage.getItem('water_mqtt_config');
  if (saved) {
    config = { ...config, ...JSON.parse(saved) };
  }
} catch (e) {}

let client = null;

/* ==========================================================================
   MQTT CLIENT CONNECTION
   ========================================================================== */

function connectMqtt() {
  if (client) {
    try { client.end(true); } catch (e) {}
  }

  updateMqttStatus('connecting', 'Connecting');

  const isSsl = window.location.protocol === 'https:' || config.port === 443;
  const protocol = isSsl ? 'wss' : 'ws';
  
  let cleanPath = config.path || '';
  if (cleanPath && !cleanPath.startsWith('/')) cleanPath = '/' + cleanPath;
  
  const portPart = (config.port && config.port !== 80 && config.port !== 443) ? `:${config.port}` : '';
  const brokerUrl = `${protocol}://${config.host}${portPart}${cleanPath}`;

  console.log('[AquaGuard] Connecting to MQTT:', brokerUrl);

  const clientId = 'aquaguard_web_' + Math.random().toString(16).substring(2, 9);
  const options = {
    clientId: clientId,
    username: config.user,
    password: config.pass,
    clean: true,
    keepalive: 30,
    connectTimeout: 8000,
    reconnectPeriod: 4000
  };

  try {
    client = mqtt.connect(brokerUrl, options);

    client.on('connect', () => {
      console.log('[AquaGuard] Connected successfully to', brokerUrl);
      updateMqttStatus('connected', 'Online');
      showToast('Connected to MQTT Broker', 'success');

      // Subscribe to telemetry topic
      client.subscribe(config.telemetryTopic, { qos: 0 }, (err) => {
        if (err) {
          console.error('[AquaGuard] Subscription error:', err);
          showToast('Failed to subscribe to telemetry', 'error');
        } else {
          // Request instant status
          sendCommand({ command: 'STATUS', action: 'status' });
        }
      });
    });

    client.on('message', (topic, payload) => {
      if (topic === config.telemetryTopic) {
        try {
          const data = JSON.parse(payload.toString());
          processIncomingTelemetry(data);
        } catch (err) {
          console.error('[AquaGuard] Invalid JSON telemetry:', err);
        }
      }
    });

    client.on('error', (err) => {
      console.error('[AquaGuard] Client error:', err);
      updateMqttStatus('disconnected', 'Error');
    });

    client.on('close', () => {
      updateMqttStatus('disconnected', 'Offline');
    });

    client.on('offline', () => {
      updateMqttStatus('disconnected', 'Offline');
    });

    client.on('reconnect', () => {
      updateMqttStatus('connecting', 'Reconnecting');
    });

  } catch (err) {
    console.error('[AquaGuard] Setup exception:', err);
    updateMqttStatus('disconnected', 'Failed');
  }
}

function updateMqttStatus(statusClass, label) {
  const badge = document.getElementById('mqttBadge');
  const text = document.getElementById('mqttStatusText');
  if (badge) badge.className = `status-pill ${statusClass}`;
  if (text) text.innerText = label;
}

function sendCommand(cmdObj) {
  if (!client || !client.connected) {
    showToast('MQTT not connected. Check broker settings.', 'error');
    return false;
  }
  const payload = JSON.stringify(cmdObj);
  client.publish(config.commandTopic, payload, { qos: 0 }, (err) => {
    if (err) {
      console.error('[AquaGuard] Publish error:', err);
      showToast('Failed to send command', 'error');
    }
  });
  return true;
}

/* ==========================================================================
   ANTI-SPIKE STATISTICAL FILTER & TELEMETRY PROCESSOR
   ========================================================================== */

function evaluateRollingWindow(now) {
  lastEvalTime = now;
  if (sampleHistory.length === 0) return;

  // Filter samples within the last 5-second window
  const recentSamples = sampleHistory.filter(s => s.timestamp >= now - WINDOW_MS);
  const pool = recentSamples.length >= 2 ? recentSamples : sampleHistory;

  // Extract water percentages
  const percentages = pool.map(s => s.percent);

  // 1. Sort values to find the median
  percentages.sort((a, b) => a - b);
  const mid = Math.floor(percentages.length / 2);
  const median = percentages.length % 2 !== 0 ? percentages[mid] : (percentages[mid - 1] + percentages[mid]) / 2;

  // 2. Reject outliers: keep values within +/- 10% of median (filter sensor spikes)
  const cluster = percentages.filter(val => Math.abs(val - median) <= 10.0);
  const robustCluster = cluster.length > 0 ? cluster : percentages;

  // 3. Dominant cluster average
  const clusterMean = robustCluster.reduce((sum, v) => sum + v, 0) / robustCluster.length;
  targetWaterPercent = clusterMean;
}

function processIncomingTelemetry(d) {
  const now = Date.now();

  // Extract raw percent
  const rawPercent = d.percent !== undefined ? parseFloat(d.percent) : (d.water_percent !== undefined ? parseFloat(d.water_percent) : 0);
  const rawDist = d.distance !== undefined ? parseFloat(d.distance) : (d.raw_distance !== undefined ? parseFloat(d.raw_distance) : 0);
  const rawLevel = d.level !== undefined ? parseFloat(d.level) : (d.water_level !== undefined ? parseFloat(d.water_level) : 0);

  // First packet init: instant setup without waiting 5 seconds on first page load
  if (isFirstPacket) {
    targetWaterPercent = rawPercent;
    currentWaterPercent = rawPercent;
    lastEvalTime = now;
    isFirstPacket = false;
  }

  // Store into rolling statistical window
  sampleHistory.push({
    timestamp: now,
    percent: rawPercent,
    dist: rawDist,
    level: rawLevel
  });

  // Prune points older than WINDOW_MS + 2000
  sampleHistory = sampleHistory.filter(s => s.timestamp >= now - (WINDOW_MS + 2000));

  // Determine target water percentage
  if (!isDampingEnabled) {
    // Raw live feed: instant 1:1
    targetWaterPercent = rawPercent;
    currentWaterPercent = rawPercent; // instantaneous jump if damping disabled
  } else {
    // 5-second evaluation interval: only re-evaluate target every 5 seconds!
    if (now - lastEvalTime >= EVAL_INTERVAL_MS) {
      evaluateRollingWindow(now);
    }
  }

  // Update Tank Dimensions (if sent from controller & user not editing)
  if (!settingsChanged) {
    if (d.radius !== undefined || d.tank_radius !== undefined) {
      tankRadius = parseFloat(d.radius || d.tank_radius);
      const radEl = document.getElementById('radius');
      if (radEl) radEl.value = tankRadius;
    }
    if (d.height !== undefined || d.tank_height !== undefined) {
      tankHeight = parseFloat(d.height || d.tank_height);
      const hEl = document.getElementById('height');
      if (hEl) hEl.value = tankHeight;
    }
    if (d.on !== undefined || d.motor_on_percent !== undefined) {
      motorOnPercent = parseFloat(d.on || d.motor_on_percent);
      const onEl = document.getElementById('on');
      if (onEl) onEl.value = motorOnPercent;
    }
    if (d.off !== undefined || d.motor_off_percent !== undefined) {
      motorOffPercent = parseFloat(d.off || d.motor_off_percent);
      const offEl = document.getElementById('off');
      if (offEl) offEl.value = motorOffPercent;
    }
    if (d.offset !== undefined || d.sensor_offset !== undefined) {
      sensorOffset = parseFloat(d.offset || d.sensor_offset);
      const offEl = document.getElementById('offset');
      if (offEl) offEl.value = sensorOffset;
    }
  }

  // Update secondary metrics
  const distEl = document.getElementById('distDisplay');
  if (distEl) distEl.innerText = rawDist.toFixed(1);

  // Live Raw Height directly from MQTT (untouched by smoothing or damping)
  const rawHeightEl = document.getElementById('rawHeightDisplay');
  if (rawHeightEl) {
    rawHeightEl.innerText = rawLevel.toFixed(1);
    rawHeightEl.classList.add('flash-pulse');
    setTimeout(() => rawHeightEl.classList.remove('flash-pulse'), 250);
  }

  const rawMiniEl = document.getElementById('rawHeightMini');
  if (rawMiniEl) rawMiniEl.innerText = rawLevel.toFixed(1);

  // Motor Status & Mode (Strictly drives filling stream animation & water usage cycle)
  const isRelayOn = d.relay === true || d.relay === 'true' || d.motor === true || d.motor === 'true' || d.motor_state === 'ON';
  updateMotorVisuals(isRelayOn, d.mode || d.motor_mode || 'AUTO');
  updateWaterUsage(isRelayOn, currentWaterPercent);

  // Verification & Countdown State
  updateSystemStateBanner(d);

  // Calibration Info
  updateCalibrationUI(d);
}

/* ==========================================================================
   FLUID LIQUID SIMULATION & DYNAMIC LITERS ENGINE (60 FPS)
   ========================================================================== */

function fluidLoop() {
  const now = Date.now();

  // If damping is enabled and 5-second interval has elapsed, evaluate rolling window
  if (isDampingEnabled && (now - lastEvalTime >= EVAL_INTERVAL_MS)) {
    evaluateRollingWindow(now);
  }

  // Smooth, slow fluid slide towards targetWaterPercent
  if (isDampingEnabled) {
    const diff = targetWaterPercent - currentWaterPercent;
    if (Math.abs(diff) > 0.02) {
      // Slow, relaxing fluid sliding motion (approx 8-10% per second for full transitions)
      const maxSpeed = 0.16;  // max 0.16% per frame => ~9.6% per second at 60 FPS
      const minSpeed = 0.015; // gentle landing speed near target
      let step = diff * 0.012; // slow easing coefficient

      if (Math.abs(step) > maxSpeed) {
        step = Math.sign(diff) * maxSpeed;
      } else if (Math.abs(step) < minSpeed) {
        step = Math.sign(diff) * minSpeed;
      }

      if (Math.abs(step) >= Math.abs(diff)) {
        currentWaterPercent = targetWaterPercent;
      } else {
        currentWaterPercent += step;
      }
    } else {
      currentWaterPercent = targetWaterPercent;
    }
  } else {
    currentWaterPercent = targetWaterPercent;
  }

  // Constrain bounds [0, 100]
  if (currentWaterPercent < 0) currentWaterPercent = 0;
  if (currentWaterPercent > 100) currentWaterPercent = 100;

  // --- DYNAMIC WATER LITERS & HEIGHT CALCULATION ---
  // Capacity = (pi * r^2 * h) / 1000 Liters
  const tankCapacityLiters = (Math.PI * Math.pow(tankRadius, 2) * tankHeight) / 1000.0;
  const currentLiters = tankCapacityLiters * (currentWaterPercent / 100.0);
  const currentDepthCm = tankHeight * (currentWaterPercent / 100.0);

  // Update UI Elements
  const percentEl = document.getElementById('waterPercentDisplay');
  if (percentEl) percentEl.innerText = currentWaterPercent.toFixed(1);

  const litersEl = document.getElementById('litersCalculated');
  if (litersEl) litersEl.innerText = currentLiters.toFixed(1);

  const capacityEl = document.getElementById('capacityTotal');
  if (capacityEl) capacityEl.innerText = tankCapacityLiters.toFixed(1);

  const levelEl = document.getElementById('levelDisplay');
  if (levelEl) levelEl.innerText = currentDepthCm.toFixed(1);

  // Update Realistic Water Height
  const waterBody = document.getElementById('waterBody');
  if (waterBody) waterBody.style.height = `${currentWaterPercent}%`;

  // Update Water Usage Liters (Strictly discrete cycle & geometry based)
  updateWaterUsage(isMotorActive, currentWaterPercent);

  // Update Trend Badge every 2 seconds
  if (now - lastTrendCheckTime >= 2000) {
    lastTrendCheckTime = now;
    const delta = currentWaterPercent - previousWaterPercent;
    previousWaterPercent = currentWaterPercent;

    const trendText = document.getElementById('trendText');
    const trendBadge = document.getElementById('trendBadge');

    if (isMotorActive || delta > 0.4) {
      if (trendText) trendText.innerText = 'Filling';
      if (trendBadge) trendBadge.innerHTML = '<span class="trend-icon">🟢</span> Filling';
    } else if (delta < -0.4) {
      if (trendText) trendText.innerText = 'Draining';
      if (trendBadge) trendBadge.innerHTML = '<span class="trend-icon">🔻</span> Draining';
    } else {
      if (trendText) trendText.innerText = 'Stable';
      if (trendBadge) trendBadge.innerHTML = '<span class="trend-icon">⚪</span> Stable';
    }
  }

  requestAnimationFrame(fluidLoop);
}

// Start 60fps fluid simulation loop
requestAnimationFrame(fluidLoop);

/* ==========================================================================
   WATER USAGE CALCULATION ENGINE (DISCRETE CYCLES & GEOMETRY)
   - Calculated with Tank Radius, Height, and Water Percentage
   - Discrete Motor ON / OFF cycles (NO continuous sensor jitter summation)
   ========================================================================== */

function updateWaterUsage(isMotorRunning, currentPercent) {
  if (tankRadius <= 0 || tankHeight <= 0 || isNaN(currentPercent) || currentPercent <= 0) {
    return totalAccumulatedUsedLiters;
  }

  // Tank Capacity: V = (π * r² * h) / 1000 Liters
  const tankCapacityLiters = (Math.PI * Math.pow(tankRadius, 2) * tankHeight) / 1000.0;

  // Discrete Cycle Transition Detection
  if (previousMotorRunning !== null) {
    // 1. Motor was RUNNING -> now turned OFF (Tank reached peak fill level)
    if (previousMotorRunning && !isMotorRunning) {
      fullBaselinePercent = currentPercent;
      try {
        localStorage.setItem('aquaguard_full_baseline', fullBaselinePercent.toString());
      } catch (e) {}
    }
    // 2. Motor was OFF -> now turned ON (Refill started, lock consumption of completed cycle)
    else if (!previousMotorRunning && isMotorRunning) {
      if (fullBaselinePercent !== null && fullBaselinePercent > currentPercent) {
        const cycleConsumedLiters = tankCapacityLiters * ((fullBaselinePercent - currentPercent) / 100.0);
        if (cycleConsumedLiters > 0) {
          totalAccumulatedUsedLiters += cycleConsumedLiters;
          try {
            localStorage.setItem('aquaguard_accumulated_used', totalAccumulatedUsedLiters.toString());
          } catch (e) {}
        }
      }
      fullBaselinePercent = null;
      try {
        localStorage.removeItem('aquaguard_full_baseline');
      } catch (e) {}
    }
  } else {
    // Initial startup: anchor baseline to current level if motor is off
    if (!isMotorRunning && fullBaselinePercent === null && currentPercent > 0) {
      fullBaselinePercent = currentPercent;
      try {
        localStorage.setItem('aquaguard_full_baseline', fullBaselinePercent.toString());
      } catch (e) {}
    }
  }

  previousMotorRunning = isMotorRunning;

  // Calculate ongoing cycle usage while motor is OFF
  let currentCycleUsedLiters = 0.0;
  if (!isMotorRunning && fullBaselinePercent !== null) {
    if (currentPercent > fullBaselinePercent) {
      // Water level rose without motor (manual top-up or positive sensor drift)
      fullBaselinePercent = currentPercent;
      try {
        localStorage.setItem('aquaguard_full_baseline', fullBaselinePercent.toString());
      } catch (e) {}
    } else {
      currentCycleUsedLiters = tankCapacityLiters * ((fullBaselinePercent - currentPercent) / 100.0);
    }
  }

  const displayedUsage = Math.max(0, totalAccumulatedUsedLiters + currentCycleUsedLiters);

  const usedEl = document.getElementById('usedDisplay');
  if (usedEl) {
    usedEl.innerText = displayedUsage.toFixed(1);
  }

  return displayedUsage;
}

/* ==========================================================================
   MOTOR CONTROLS & VISUALS
   ========================================================================== */

let isMotorActive = false;

function updateMotorVisuals(isOn, mode) {
  isMotorActive = isOn;
  const statusBadge = document.getElementById('motorStatusBadge');
  const pumpIcon = document.getElementById('pumpIcon');
  const modeBadge = document.getElementById('motorModeBadge');
  const inflowStream = document.getElementById('inflowStream');

  if (statusBadge) {
    statusBadge.innerText = isOn ? 'PUMP RUNNING' : 'PUMP OFF';
    statusBadge.className = `motor-state-text ${isOn ? 'on' : 'off'}`;
  }

  if (pumpIcon) {
    if (isOn) {
      pumpIcon.classList.add('pumping');
    } else {
      pumpIcon.classList.remove('pumping');
    }
  }

  if (modeBadge) {
    modeBadge.innerText = (mode || 'AUTO').toUpperCase();
  }

  // Inflow water filling stream turns ON/OFF STRICTLY with motor state
  if (inflowStream) {
    if (isOn) {
      inflowStream.classList.add('flowing');
    } else {
      inflowStream.classList.remove('flowing');
    }
  }
}

function sendMotorCommand(action) {
  let payload = {};
  if (action === 'on') {
    payload = { command: 'MOTOR', value: 'ON', action: 'motor', state: 'on' };
    updateMotorVisuals(true, 'MANUAL');
    updateWaterUsage(true, currentWaterPercent);
    showToast('Sent: Manual Motor ON', 'info');
  } else if (action === 'off') {
    payload = { command: 'MOTOR', value: 'OFF', action: 'motor', state: 'off' };
    updateMotorVisuals(false, 'MANUAL');
    updateWaterUsage(false, currentWaterPercent);
    showToast('Sent: Manual Motor OFF', 'info');
  } else if (action === 'auto') {
    payload = { command: 'MODE', value: 'AUTO', action: 'motor', state: 'auto' };
    const modeBadge = document.getElementById('motorModeBadge');
    if (modeBadge) modeBadge.innerText = 'AUTO';
    showToast('Sent: Mode AUTO', 'info');
  }

  sendCommand(payload);
}

/* ==========================================================================
   SYSTEM STATE & COUNTDOWN NOTICES
   ========================================================================== */

function updateSystemStateBanner(d) {
  const banner = document.getElementById('countdownBanner');
  const text = document.getElementById('countdownText');
  const stateBadge = document.getElementById('tankStateBadge');
  const state = d.state || d.system_state || 'NORMAL';

  if (!banner || !text) return;

  if (stateBadge) stateBadge.innerText = state;

  if (state === 'NORMAL') {
    banner.className = 'system-status-banner normal';
    text.innerText = 'System Stable • Live Monitoring Active';
  } else if (state === 'VERIFY_ON') {
    banner.className = 'system-status-banner verify-on';
    text.innerText = `⏳ Confirming LOW WATER ON: ${d.remaining || 0}s remaining`;
  } else if (state === 'VERIFY_OFF') {
    banner.className = 'system-status-banner verify-off';
    text.innerText = `⏳ Confirming HIGH WATER OFF: ${d.remaining || 0}s remaining`;
  } else if (state === 'CALIBRATING') {
    banner.className = 'system-status-banner calibrating';
    text.innerText = `🎯 Calibration Active: ${d.remaining || 0}s remaining`;
  }
}

function updateCalibrationUI(d) {
  const isCalRunning = d.calibrationRunning === true || d.calibrationRunning === 'true' || d.calibration_running === true;
  const isCalibrated = d.calibrated === true || d.calibrated === 'true';

  const notice = document.getElementById('calNotice');
  const bar = document.getElementById('calBar');
  const timer = document.getElementById('calTimer');
  const calStatus = document.getElementById('calibratedStatus');

  if (calStatus) {
    calStatus.innerText = isCalibrated ? 'CALIBRATED' : 'NOT READY';
    calStatus.style.color = isCalibrated ? 'var(--success)' : 'var(--danger)';
  }

  if (isCalRunning) {
    if (notice) notice.innerText = `KEEP SENSOR STEADY — ${d.remaining || 0}s remaining`;
    if (bar) bar.style.width = (d.progress || 0) + '%';
    if (timer) timer.innerText = `${d.remaining || 0}s remaining`;
  } else {
    if (notice) notice.innerText = isCalibrated ? 'Sensor calibrated and saved in EEPROM.' : 'Keep tank steady before starting 10-second calibration.';
    if (bar) bar.style.width = '0%';
    if (timer) timer.innerText = 'Ready';
  }

  const emptyVal = d.empty !== undefined ? d.empty : d.empty_distance;
  if (emptyVal !== undefined) {
    const el = document.getElementById('emptyDist');
    if (el) el.innerText = `${Number(emptyVal).toFixed(2)} cm`;
  }

  const fullVal = d.full !== undefined ? d.full : d.full_distance;
  if (fullVal !== undefined) {
    const el = document.getElementById('fullDist');
    if (el) el.innerText = `${Number(fullVal).toFixed(2)} cm`;
  }
}

/* ==========================================================================
   SMART DAMPING TOGGLE HANDLER
   ========================================================================== */

const dampingToggle = document.getElementById('dampingToggle');
const dampingBadge = document.getElementById('dampingBadge');

if (dampingToggle) {
  dampingToggle.checked = isDampingEnabled;
  updateDampingBadge(isDampingEnabled);

  dampingToggle.addEventListener('change', (e) => {
    isDampingEnabled = e.target.checked;
    updateDampingBadge(isDampingEnabled);
    try {
      localStorage.setItem('aquaguard_damping', isDampingEnabled ? 'true' : 'false');
    } catch (err) {}

    showToast(isDampingEnabled ? 'Smart Anti-Spike Damping Enabled' : 'Raw Live Feed Enabled (Immediate Update)', 'info');
  });
}

function updateDampingBadge(enabled) {
  if (!dampingBadge) return;
  if (enabled) {
    dampingBadge.innerText = 'SMOOTHED';
    dampingBadge.className = 'mode-pill active';
  } else {
    dampingBadge.innerText = 'RAW LIVE';
    dampingBadge.className = 'mode-pill raw';
  }
}

/* ==========================================================================
   SETTINGS MODAL & TABS LOGIC
   ========================================================================== */

document.getElementById('settingsBtn').addEventListener('click', () => {
  document.getElementById('mqttHost').value = config.host;
  document.getElementById('mqttPort').value = config.port;
  document.getElementById('mqttPath').value = config.path;
  document.getElementById('mqttUser').value = config.user;
  document.getElementById('mqttPass').value = config.pass;
  document.getElementById('mqttTelemetryTopic').value = config.telemetryTopic;
  document.getElementById('mqttCommandTopic').value = config.commandTopic;

  document.getElementById('settingsModal').classList.add('open');
});

function closeSettingsModal() {
  document.getElementById('settingsModal').classList.remove('open');
}

// Close when clicking outside sheet
document.getElementById('settingsModal').addEventListener('click', (e) => {
  if (e.target.id === 'settingsModal') closeSettingsModal();
});

function switchTab(tabId) {
  document.querySelectorAll('.tab-btn').forEach(btn => btn.classList.remove('active'));
  document.querySelectorAll('.tab-content').forEach(content => content.classList.remove('active'));

  const activeContent = document.getElementById(tabId);
  if (activeContent) activeContent.classList.add('active');

  const buttons = document.querySelectorAll('.tab-btn');
  if (tabId === 'tabTank' && buttons[0]) buttons[0].classList.add('active');
  if (tabId === 'tabCalib' && buttons[1]) buttons[1].classList.add('active');
  if (tabId === 'tabMqtt' && buttons[2]) buttons[2].classList.add('active');
}

function saveSettings() {
  const radius = parseFloat(document.getElementById('radius').value);
  const height = parseFloat(document.getElementById('height').value);
  const on = parseFloat(document.getElementById('on').value);
  const off = parseFloat(document.getElementById('off').value);
  const offset = parseFloat(document.getElementById('offset').value) || 0;

  if (isNaN(radius) || radius <= 0 || isNaN(height) || height <= 0) {
    alert('Invalid tank dimensions! Radius and Height must be positive.');
    return;
  }

  if (isNaN(on) || isNaN(off) || on >= off) {
    alert('Motor OFF percentage must be strictly greater than Motor ON percentage.');
    return;
  }

  tankRadius = radius;
  tankHeight = height;
  motorOnPercent = on;
  motorOffPercent = off;
  sensorOffset = offset;

  const payload = {
    command: 'SETTINGS',
    action: 'save',
    motor_on_percent: on,
    motor_off_percent: off,
    tank_radius: radius,
    tank_height: height,
    sensor_offset: offset,
    on: on,
    off: off,
    radius: radius,
    height: height,
    offset: offset
  };

  if (sendCommand(payload)) {
    settingsChanged = false;
    showToast('Settings saved & applied', 'success');
    closeSettingsModal();
  }
}

function saveMqttConfig() {
  config.host = document.getElementById('mqttHost').value.trim() || DEFAULT_CONFIG.host;
  config.port = parseInt(document.getElementById('mqttPort').value) || DEFAULT_CONFIG.port;
  config.path = document.getElementById('mqttPath').value.trim();
  config.user = document.getElementById('mqttUser').value.trim();
  config.pass = document.getElementById('mqttPass').value.trim();
  config.telemetryTopic = document.getElementById('mqttTelemetryTopic').value.trim() || DEFAULT_CONFIG.telemetryTopic;
  config.commandTopic = document.getElementById('mqttCommandTopic').value.trim() || DEFAULT_CONFIG.commandTopic;

  try {
    localStorage.setItem('water_mqtt_config', JSON.stringify(config));
  } catch (e) {}

  closeSettingsModal();
  showToast('Connecting with new MQTT settings...', 'info');
  connectMqtt();
}

function calibrate(type) {
  if (!confirm(`Keep the sensor steady over the ${type.toUpperCase()} position for 10 seconds. Start calibration?`)) {
    return;
  }

  const payload = {
    command: 'CALIBRATE',
    type: type,
    action: 'calibrate'
  };

  if (sendCommand(payload)) {
    showToast(`Starting ${type} calibration...`, 'info');
    closeSettingsModal();
  }
}

function resetUsage() {
  if (!confirm('Reset total cumulative water usage back to 0 Liters?')) return;
  totalAccumulatedUsedLiters = 0.0;
  fullBaselinePercent = currentWaterPercent;
  try {
    localStorage.setItem('aquaguard_accumulated_used', '0');
    localStorage.setItem('aquaguard_full_baseline', fullBaselinePercent.toString());
  } catch (e) {}
  const usedEl = document.getElementById('usedDisplay');
  if (usedEl) usedEl.innerText = '0.0';
  const payload = { command: 'RESET_USAGE', action: 'resetUsage' };
  if (sendCommand(payload)) {
    showToast('Water usage reset to 0 L', 'info');
  }
}

function resetAll() {
  if (!confirm('Reset ALL settings and calibrations to factory default?')) return;
  const payload = { command: 'RESET_ALL', action: 'resetAll' };
  if (sendCommand(payload)) {
    showToast('Factory reset command sent', 'warning');
    closeSettingsModal();
  }
}

/* ==========================================================================
   TOAST HELPER
   ========================================================================== */

function showToast(message, type = 'info') {
  const hub = document.getElementById('toastContainer');
  if (!hub) return;

  const toast = document.createElement('div');
  toast.className = `toast toast-${type}`;
  toast.innerText = message;
  hub.appendChild(toast);

  setTimeout(() => {
    toast.style.opacity = '0';
    toast.style.transition = 'opacity 0.3s ease';
    setTimeout(() => toast.remove(), 300);
  }, 3500);
}

// Start MQTT on DOM load
window.addEventListener('DOMContentLoaded', () => {
  connectMqtt();
});
