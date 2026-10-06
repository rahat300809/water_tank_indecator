/* =====================================================
   Smart Water Tank Controller - Web Frontend Logic
   Universal MQTT Pub/Sub + Fluid Tank Animation + Optimistic Triggers
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

if (!DEFAULT_CONFIG.host || DEFAULT_CONFIG.host === 'localhost' || DEFAULT_CONFIG.host === '127.0.0.1') {
  DEFAULT_CONFIG.host = '192.168.110.133';
  DEFAULT_CONFIG.port = 9001;
  DEFAULT_CONFIG.path = '';
}

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
let telemetryReceived = false;

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

  const isSsl = window.location.protocol === 'https:' || config.port === 443;
  const protocol = isSsl ? 'wss' : 'ws';
  
  let cleanPath = config.path || '';
  if (cleanPath && !cleanPath.startsWith('/')) cleanPath = '/' + cleanPath;
  
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
      updateMqttStatus('connected', 'MQTT Connected');
      showToast('Connected to MQTT Broker', 'success');

      // Subscribe to telemetry topic
      client.subscribe(config.telemetryTopic, { qos: 0 }, (err) => {
        if (err) {
          console.error('Subscription error:', err);
          showToast('Failed to subscribe to telemetry', 'error');
        } else {
          console.log('Subscribed to:', config.telemetryTopic);
          // Send request for status
          sendCommand({ command: 'STATUS', action: 'status' });
        }
      });
    });

    client.on('message', (topic, payload) => {
      if (topic === config.telemetryTopic) {
        try {
          const rawStr = payload.toString();
          const data = JSON.parse(rawStr);
          lastTelemetryTime = Date.now();
          telemetryReceived = true;
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
   COMMAND PUBLISHER (UNIVERSAL FORMAT)
   ===================================================== */

function sendCommand(cmdObj) {
  if (!client || !client.connected) {
    showToast('MQTT not connected! Please check connection.', 'error');
    return false;
  }

  const payload = JSON.stringify(cmdObj);
  client.publish(config.commandTopic, payload, { qos: 0 }, (err) => {
    if (err) {
      console.error('Command publish failed:', err);
      showToast('Failed to send command', 'error');
    } else {
      console.log('Command sent to', config.commandTopic, ':', payload);
    }
  });
  return true;
}

/* =====================================================
   DATA TELEMETRY HANDLER
   Supports both standard and alternative JSON keys
   ===================================================== */

function applyTelemetry(d) {
  // Extract percent (support .percent or .water_percent)
  const percentVal = d.percent !== undefined ? d.percent : d.water_percent;
  if (percentVal !== undefined && percentVal !== null) {
    targetWaterPercent = parseFloat(percentVal);
    if (!uiAnimationStarted) {
      uiWaterPercent = targetWaterPercent;
      uiAnimationStarted = true;
    }
  }

  // Stats Grid
  const distVal = d.distance !== undefined ? d.distance : (d.raw_distance !== undefined ? d.raw_distance : null);
  if (distVal !== null) document.getElementById('distance').innerText = Number(distVal).toFixed(2);

  const levelVal = d.level !== undefined ? d.level : d.water_level;
  if (levelVal !== undefined) document.getElementById('level').innerText = Number(levelVal).toFixed(2);

  const litersVal = d.liters !== undefined ? d.liters : d.water_liters;
  if (litersVal !== undefined) document.getElementById('liters').innerText = Number(litersVal).toFixed(2);

  const capVal = d.capacity !== undefined ? d.capacity : d.tank_capacity;
  if (capVal !== undefined) document.getElementById('capacity').innerText = Number(capVal).toFixed(2);

  const usedVal = d.used !== undefined ? d.used : d.water_used;
  if (usedVal !== undefined) document.getElementById('used').innerText = Number(usedVal).toFixed(2);

  // Motor Status & Mode
  const isRelayOn = d.relay === true || d.relay === 'true' || d.motor === true || d.motor === 'true' || d.motor_state === 'ON';
  const motorStatusEl = document.getElementById('motorStatus');
  if (motorStatusEl) {
    motorStatusEl.innerText = isRelayOn ? 'ON' : 'OFF';
    motorStatusEl.className = 'motor-status ' + (isRelayOn ? 'motor-on' : 'motor-off');
  }

  const modeVal = d.mode || d.motor_mode;
  if (modeVal) {
    document.getElementById('motorMode').innerText = modeVal.toUpperCase();
  }

  // System State & Countdowns
  const stateEl = document.getElementById('systemState');
  const countdownEl = document.getElementById('countdown');
  const state = d.state || d.system_state || 'NORMAL';

  if (stateEl) {
    stateEl.innerText = state;
    stateEl.className = 'notice';

    if (state === 'NORMAL') {
      if (countdownEl) countdownEl.innerText = 'System stable • Telemetry active';
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
  const onVal = d.on !== undefined ? d.on : d.motor_on_percent;
  if (onVal !== undefined) document.getElementById('onDisplay').innerText = Number(onVal).toFixed(1);

  const offVal = d.off !== undefined ? d.off : d.motor_off_percent;
  if (offVal !== undefined) document.getElementById('offDisplay').innerText = Number(offVal).toFixed(1);

  // Calibration Info
  const emptyVal = d.empty !== undefined ? d.empty : d.empty_distance;
  if (emptyVal !== undefined) document.getElementById('empty').innerText = Number(emptyVal).toFixed(2);

  const fullVal = d.full !== undefined ? d.full : d.full_distance;
  if (fullVal !== undefined) document.getElementById('full').innerText = Number(fullVal).toFixed(2);

  const isCalibrated = d.calibrated === true || d.calibrated === 'true';
  const calBadge = document.getElementById('calibrated');
  if (calBadge) {
    calBadge.innerText = isCalibrated ? 'YES' : 'NO';
    calBadge.style.color = isCalibrated ? 'var(--success)' : 'var(--danger)';
  }

  // Calibration progress
  const isCalRunning = d.calibrationRunning === true || d.calibrationRunning === 'true' || d.calibration_running === true || d.calibration_running === 'true';
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

  // Form Fields (Prevent overwriting if user is typing)
  if (!settingsChanged) {
    const radVal = d.radius !== undefined ? d.radius : d.tank_radius;
    if (radVal !== undefined) document.getElementById('radius').value = radVal;

    const hVal = d.height !== undefined ? d.height : d.tank_height;
    if (hVal !== undefined) document.getElementById('height').value = hVal;

    if (onVal !== undefined) document.getElementById('on').value = onVal;
    if (offVal !== undefined) document.getElementById('off').value = offVal;

    const offsetVal = d.offset !== undefined ? d.offset : d.sensor_offset;
    if (offsetVal !== undefined) document.getElementById('offset').value = offsetVal;
  }
}

/* =====================================================
   FLUID WATER ANIMATION (20 FPS)
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
   USER TRIGGER ACTIONS (OPTIMISTIC + DUAL SCHEMA)
   ===================================================== */

function sendMotorCommand(state) {
  const motorStatusEl = document.getElementById('motorStatus');
  const motorModeEl = document.getElementById('motorMode');

  // Universal payload that matches both sketch versions
  let payload = {};
  if (state === 'on') {
    payload = {
      command: 'MOTOR',
      value: 'ON',
      action: 'motor',
      state: 'on'
    };
    // Optimistic UI update
    if (motorStatusEl) {
      motorStatusEl.innerText = 'ON';
      motorStatusEl.className = 'motor-status motor-on';
    }
    if (motorModeEl) motorModeEl.innerText = 'MANUAL';
  } else if (state === 'off') {
    payload = {
      command: 'MOTOR',
      value: 'OFF',
      action: 'motor',
      state: 'off'
    };
    if (motorStatusEl) {
      motorStatusEl.innerText = 'OFF';
      motorStatusEl.className = 'motor-status motor-off';
    }
    if (motorModeEl) motorModeEl.innerText = 'MANUAL';
  } else if (state === 'auto') {
    payload = {
      command: 'MODE',
      value: 'AUTO',
      action: 'motor',
      state: 'auto'
    };
    if (motorModeEl) motorModeEl.innerText = 'AUTO';
  }

  if (sendCommand(payload)) {
    showToast(`Trigger sent: Motor ${state.toUpperCase()}`, 'info');
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

  // Universal payload
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
    document.getElementById('onDisplay').innerText = on.toFixed(1);
    document.getElementById('offDisplay').innerText = off.toFixed(1);
    showToast('Settings saved & sent to ESP32', 'success');
  }
}

function calibrate(type) {
  if (!confirm(`Keep the tank completely steady for 10 seconds. Start ${type.toUpperCase()} calibration?`)) {
    return;
  }

  const payload = {
    command: 'CALIBRATE',
    type: type,
    action: 'calibrate'
  };

  if (sendCommand(payload)) {
    showToast(`Starting ${type} calibration`, 'info');
  }
}

function resetUsage() {
  if (!confirm('Reset total water usage to 0?')) {
    return;
  }

  const payload = {
    command: 'RESET_USAGE',
    action: 'resetUsage'
  };

  if (sendCommand(payload)) {
    document.getElementById('used').innerText = '0.00';
    showToast('Water usage reset sent', 'info');
  }
}

function resetAll() {
  if (!confirm('Reset ALL settings, calibration data, and water usage to defaults?')) {
    return;
  }

  const payload = {
    command: 'RESET_ALL',
    action: 'resetAll'
  };

  if (sendCommand(payload)) {
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
