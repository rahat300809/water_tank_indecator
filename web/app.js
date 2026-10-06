/* =====================================================
   Smart Water Tank Controller - Web Frontend Logic
   Realtime MQTT Pub/Sub + Fluid Tank Animation
   ===================================================== */

let settingsChanged = false;
let uiWaterPercent = 0;
let targetWaterPercent = 0;
let uiAnimationStarted = false;

// Default MQTT Config
const DEFAULT_CONFIG = {
  host: window.location.hostname || '192.168.110.133',
  port: window.location.protocol === 'https:' ? 443 : 9001,
  path: window.location.protocol === 'https:' ? '/mqtt' : '/mqtt',
  user: 'rahat300809',
  pass: 'RAHAT678',
  telemetryTopic: 'devices/ESP32_WATER_01/telemetry',
  commandTopic: 'devices/ESP32_WATER_01/command'
};

// If loaded directly from local file (file://) or localhost
if (!DEFAULT_CONFIG.host || DEFAULT_CONFIG.host === 'localhost' || DEFAULT_CONFIG.host === '127.0.0.1') {
  DEFAULT_CONFIG.host = '192.168.110.133';
  DEFAULT_CONFIG.port = 9001;
  DEFAULT_CONFIG.path = '';
}

// Load persisted config or defaults
let config = { ...DEFAULT_CONFIG };
try {
  const saved = localStorage.getItem('water_mqtt_config');
  if (saved) {
    config = { ...config, ...JSON.parse(saved) };
  }
} catch (e) {
  console.warn('LocalStorage error:', e);
}

let client = null;
let lastTelemetryTime = 0;

/* =====================================================
   MQTT CONNECTION
   ===================================================== */

function connectMqtt() {
  if (client) {
    try {
      client.end(true);
    } catch (e) {}
  }

  updateMqttStatus('connecting', 'Connecting...');

  // Build WebSocket URL
  const isSsl = window.location.protocol === 'https:' || config.port === 443;
  const protocol = isSsl ? 'wss' : 'ws';
  
  // Format clean URL
  let cleanPath = config.path || '';
  if (cleanPath && !cleanPath.startsWith('/')) cleanPath = '/' + cleanPath;
  
  // Port omission for standard 80/443
  const portPart = (config.port && config.port !== 80 && config.port !== 443) ? `:${config.port}` : '';
  const brokerUrl = `${protocol}://${config.host}${portPart}${cleanPath}`;

  console.log('Connecting to MQTT Broker:', brokerUrl);

  const clientId = 'web_tank_' + Math.random().toString(16).substring(2, 10);
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
      console.log('MQTT Connected successfully to', brokerUrl);
      updateMqttStatus('connected', 'Live MQTT');
      showToast('Connected to MQTT Broker', 'success');

      // Subscribe to telemetry topic
      client.subscribe(config.telemetryTopic, { qos: 0 }, (err) => {
        if (err) {
          console.error('Subscription error:', err);
          showToast('Failed to subscribe to telemetry', 'error');
        } else {
          console.log('Subscribed to:', config.telemetryTopic);
          // Request immediate status from ESP32
          sendCommand({ action: 'status' });
        }
      });
    });

    client.on('message', (topic, payload) => {
      if (topic === config.telemetryTopic) {
        try {
          const data = JSON.parse(payload.toString());
          lastTelemetryTime = Date.now();
          applyTelemetry(data);
        } catch (err) {
          console.error('Invalid telemetry JSON:', err, payload.toString());
        }
      }
    });

    client.on('error', (err) => {
      console.error('MQTT Client Error:', err);
      updateMqttStatus('disconnected', 'MQTT Error');
    });

    client.on('close', () => {
      updateMqttStatus('disconnected', 'Disconnected');
    });

    client.on('offline', () => {
      updateMqttStatus('disconnected', 'Offline');
    });

    client.on('reconnect', () => {
      updateMqttStatus('connecting', 'Reconnecting...');
    });

  } catch (err) {
    console.error('MQTT setup failed:', err);
    updateMqttStatus('disconnected', 'Connection Failed');
  }
}

function updateMqttStatus(statusClass, label) {
  const badge = document.getElementById('mqttBadge');
  const text = document.getElementById('mqttStatusText');
  if (badge) {
    badge.className = `mqtt-badge ${statusClass}`;
  }
  if (text) {
    text.innerText = label;
  }
}

/* =====================================================
   COMMAND PUBLISHER
   ===================================================== */

function sendCommand(cmdObj) {
  if (!client || !client.connected) {
    showToast('MQTT not connected! Cannot send command.', 'error');
    return false;
  }

  const payload = JSON.stringify(cmdObj);
  client.publish(config.commandTopic, payload, { qos: 0 }, (err) => {
    if (err) {
      console.error('Command publish failed:', err);
      showToast('Failed to send command', 'error');
    } else {
      console.log('Command sent:', payload);
    }
  });
  return true;
}

/* =====================================================
   DATA TELEMETRY HANDLER
   Matches ESP32 payload structure 1:1
   ===================================================== */

function applyTelemetry(d) {
  // Set target water percent for smooth animation
  if (typeof d.percent === 'number') {
    targetWaterPercent = d.percent;
    if (!uiAnimationStarted) {
      uiWaterPercent = targetWaterPercent;
      uiAnimationStarted = true;
    }
  }

  // Stats Grid
  if (d.distance !== undefined) document.getElementById('distance').innerText = Number(d.distance).toFixed(2);
  if (d.level !== undefined) document.getElementById('level').innerText = Number(d.level).toFixed(2);
  if (d.liters !== undefined) document.getElementById('liters').innerText = Number(d.liters).toFixed(2);
  if (d.capacity !== undefined) document.getElementById('capacity').innerText = Number(d.capacity).toFixed(2);
  if (d.used !== undefined) document.getElementById('used').innerText = Number(d.used).toFixed(2);

  // Motor Status & Mode
  const motorStatusEl = document.getElementById('motorStatus');
  const isRelayOn = d.relay === true || d.relay === 'true';
  if (motorStatusEl) {
    motorStatusEl.innerText = isRelayOn ? 'ON' : 'OFF';
    motorStatusEl.className = 'motor-status ' + (isRelayOn ? 'motor-on' : 'motor-off');
  }

  if (d.mode) {
    document.getElementById('motorMode').innerText = d.mode;
  }

  // System State & Countdowns
  const stateEl = document.getElementById('systemState');
  const countdownEl = document.getElementById('countdown');
  const state = d.state || 'NORMAL';

  if (stateEl) {
    stateEl.innerText = state;
    stateEl.className = 'notice';

    if (state === 'NORMAL') {
      if (countdownEl) countdownEl.innerText = 'System stable';
    } else if (state === 'VERIFY_ON') {
      stateEl.classList.add('verify-on');
      if (countdownEl) countdownEl.innerText = `🔎 Confirming MOTOR ON — ${d.remaining || 0}s remaining`;
    } else if (state === 'VERIFY_OFF') {
      stateEl.classList.add('verify-off');
      if (countdownEl) countdownEl.innerText = `🔎 Confirming MOTOR OFF — ${d.remaining || 0}s remaining`;
    } else if (state === 'CALIBRATING') {
      stateEl.classList.add('calibrating');
      if (countdownEl) countdownEl.innerText = `⏳ Calibration — ${d.remaining || 0}s remaining`;
    }
  }

  // Motor Thresholds
  if (d.on !== undefined) document.getElementById('onDisplay').innerText = Number(d.on).toFixed(1);
  if (d.off !== undefined) document.getElementById('offDisplay').innerText = Number(d.off).toFixed(1);

  // Calibration Info
  if (d.empty !== undefined) document.getElementById('empty').innerText = Number(d.empty).toFixed(2);
  if (d.full !== undefined) document.getElementById('full').innerText = Number(d.full).toFixed(2);

  const isCalibrated = d.calibrated === true || d.calibrated === 'true';
  const calBadge = document.getElementById('calibrated');
  if (calBadge) {
    calBadge.innerText = isCalibrated ? 'YES' : 'NO';
    calBadge.style.color = isCalibrated ? 'var(--success)' : 'var(--danger)';
  }

  // Calibration progress
  const isCalRunning = d.calibrationRunning === true || d.calibrationRunning === 'true';
  const calNoticeEl = document.getElementById('calNotice');
  const calBarEl = document.getElementById('calBar');
  const calTimerEl = document.getElementById('calTimer');

  if (isCalRunning) {
    if (calNoticeEl) calNoticeEl.innerText = `⏳ KEEP TANK STEADY — ${d.remaining || 0}s remaining`;
    if (calBarEl) calBarEl.style.width = (d.progress || 0) + '%';
    if (calTimerEl) calTimerEl.innerText = `${d.remaining || 0} seconds`;
  } else {
    if (calNoticeEl) calNoticeEl.innerText = isCalibrated ? 'Calibration ready' : 'Keep tank steady before calibration.';
    if (calBarEl) calBarEl.style.width = '0%';
    if (calTimerEl) calTimerEl.innerText = 'Ready';
  }

  // Form Fields (Prevent overwriting if user is editing)
  if (!settingsChanged) {
    if (d.radius !== undefined) document.getElementById('radius').value = d.radius;
    if (d.height !== undefined) document.getElementById('height').value = d.height;
    if (d.on !== undefined) document.getElementById('on').value = d.on;
    if (d.off !== undefined) document.getElementById('off').value = d.off;
    if (d.offset !== undefined) document.getElementById('offset').value = d.offset;
  }
}

/* =====================================================
   FLUID WATER ANIMATION (20 FPS)
   Identical smooth convergence formula to original ESP32 code
   ===================================================== */

function animateWater() {
  let difference = targetWaterPercent - uiWaterPercent;

  if (Math.abs(difference) > 20) {
    uiWaterPercent += difference * 0.08;
  } else if (Math.abs(difference) > 10) {
    uiWaterPercent += difference * 0.06;
  } else if (Math.abs(difference) > 3) {
    uiWaterPercent += difference * 0.04;
  } else {
    uiWaterPercent += difference * 0.025;
  }

  // Snap to target if very close
  if (Math.abs(targetWaterPercent - uiWaterPercent) < 0.05) {
    uiWaterPercent = targetWaterPercent;
  }

  if (uiWaterPercent < 0) uiWaterPercent = 0;
  if (uiWaterPercent > 100) uiWaterPercent = 100;

  const waterEl = document.getElementById('water');
  const textEl = document.getElementById('waterText');

  if (waterEl) waterEl.style.height = uiWaterPercent + '%';
  if (textEl) textEl.innerText = uiWaterPercent.toFixed(1) + '%';
}

setInterval(animateWater, 50);

/* =====================================================
   USER ACTIONS
   ===================================================== */

function sendMotorCommand(state) {
  if (sendCommand({ action: 'motor', state: state })) {
    showToast(`Command sent: Motor ${state.toUpperCase()}`, 'info');
  }
}

function saveSettings() {
  const radius = parseFloat(document.getElementById('radius').value);
  const height = parseFloat(document.getElementById('height').value);
  const on = parseFloat(document.getElementById('on').value);
  const off = parseFloat(document.getElementById('off').value);
  const offset = parseFloat(document.getElementById('offset').value) || 0;

  if (isNaN(radius) || radius <= 0 || isNaN(height) || height <= 0) {
    alert('Invalid tank dimensions! Radius and Height must be positive numbers.');
    return;
  }

  if (isNaN(on) || isNaN(off) || on >= off) {
    alert('Motor OFF percentage must be strictly greater than Motor ON percentage.');
    return;
  }

  const payload = {
    action: 'save',
    radius: radius,
    height: height,
    on: on,
    off: off,
    offset: offset
  };

  if (sendCommand(payload)) {
    settingsChanged = false;
    showToast('Settings saved & transmitted to ESP32', 'success');
  }
}

function calibrate(type) {
  if (!confirm(`Keep the tank completely steady for 10 seconds. Start ${type.toUpperCase()} calibration?`)) {
    return;
  }

  if (sendCommand({ action: 'calibrate', type: type })) {
    showToast(`Starting ${type} calibration (10 seconds)`, 'info');
  }
}

function resetUsage() {
  if (!confirm('Reset total water usage to 0?')) {
    return;
  }

  if (sendCommand({ action: 'resetUsage' })) {
    showToast('Water usage reset sent', 'info');
  }
}

function resetAll() {
  if (!confirm('Reset ALL settings, calibration data, and water usage to factory defaults?')) {
    return;
  }

  if (sendCommand({ action: 'resetAll' })) {
    settingsChanged = false;
    uiWaterPercent = 0;
    targetWaterPercent = 0;
    uiAnimationStarted = false;
    showToast('All settings reset sent to ESP32', 'warning');
  }
}

/* =====================================================
   SETTINGS MODAL & PERSISTENCE
   ===================================================== */

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

/* =====================================================
   TOAST NOTIFICATION HELPER
   ===================================================== */

function showToast(message, type = 'info') {
  const container = document.getElementById('toastContainer');
  if (!container) return;

  const toast = document.createElement('div');
  toast.className = `toast toast-${type}`;
  toast.innerText = message;

  container.appendChild(toast);

  setTimeout(() => {
    toast.style.opacity = '0';
    toast.style.transition = 'opacity 0.3s ease';
    setTimeout(() => toast.remove(), 300);
  }, 3500);
}

// Start connection on load
window.addEventListener('DOMContentLoaded', () => {
  connectMqtt();
});
